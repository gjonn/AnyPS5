#include "ControlFlow/UserDataCalls.hpp"
#include "ControlFlow/GraphBuilder.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <algorithm>
#include <cstdio>
#include <array>
#include <optional>
#include <string>
#include <limits>
#include <stdexcept>

namespace ShaderRecompiler {

namespace {

constexpr std::size_t MaxCalleeWords = 16384;
constexpr std::size_t MaxCombinedWords = 262144;

void requireLeafInstruction(const RdnaInstruction& instruction, bool last, std::uint32_t link) {
    if (instruction.op == RdnaOpcode::SSwappcB64) {
        throw std::invalid_argument("nested scalar calls in a captured callee are unsupported");
    }
    if (instruction.op == RdnaOpcode::SGetpcB64) throw std::invalid_argument("PC-relative captured callees are unsupported");
    if (instruction.op == RdnaOpcode::SEndpgm || instruction.op == RdnaOpcode::SCodeEnd) {
        throw std::invalid_argument("captured callee ends without returning");
    }
    if (instruction.op == RdnaOpcode::SSetpcB64 && (!last || instruction.source0.kind != RdnaOperandKind::ScalarRegister || instruction.source0.reg != link)) {
        throw std::invalid_argument("captured callee has an unsupported return");
    }
}

constexpr std::size_t MaxTableTargets = 128;
constexpr std::size_t MaxTableEntries = 4096;

struct ScalarLoad {
    std::uint32_t data = 0;
    std::uint32_t base = 0;
    std::uint32_t offset = 0;
    std::uint32_t dwords = 0;
};

std::optional<ScalarLoad> decodeScalarLoad(const RdnaInstruction& instruction) {
    std::uint32_t dwords = 0;
    switch (instruction.op) {
    case RdnaOpcode::SLoadDwordx4: case RdnaOpcode::SBufferLoadDwordx4: dwords = 4; break;
    case RdnaOpcode::SLoadDwordx8: case RdnaOpcode::SBufferLoadDwordx8: dwords = 8; break;
    case RdnaOpcode::SBufferLoadDwordx2: dwords = 2; break;
    case RdnaOpcode::SBufferLoadDwordx16: dwords = 16; break;
    default: return std::nullopt;
    }
    if (instruction.wordCount < 2) return std::nullopt;
    const auto word0 = instruction.rawWords[0];
    const auto word1 = instruction.rawWords[1];
    return ScalarLoad{(word0 >> 6u) & 0x7fu, (word0 & 0x3fu) * 2u, word1 & 0x1fffffu, dwords};
}

std::optional<std::uint32_t> lastScalarWriter(const RdnaProgram& program, std::uint32_t before, std::uint32_t reg) {
    for (auto index = before; index-- > 0;) {
        if (WritesScalarRegister(program.instructions[index], reg)) return index;
    }
    return std::nullopt;
}

bool readWord(SrtMemoryReader reader, void* context, std::uint64_t address, std::uint32_t& word) {
    return reader(context, address, &word);
}

std::vector<std::uint32_t> readLeafCallee(SrtMemoryReader reader, void* context, std::uint64_t address, std::uint32_t link) {
    std::vector<std::uint32_t> words;
    std::size_t position = 0;
    bool readable = true;
    while (position < MaxCalleeWords) {
        while (readable && words.size() < position + MaxRdnaInstructionRawWords) {
            std::uint32_t word = 0;
            if (!readWord(reader, context, address + words.size() * 4u, word)) {
                readable = false;
                break;
            }
            words.push_back(word);
        }
        if (position >= words.size()) throw std::invalid_argument("table callee is unmapped");
        const auto instruction = DecodeRdnaInstruction(static_cast<std::uint32_t>(position * 4u), words, static_cast<std::uint32_t>(position));
        const bool returned = instruction.op == RdnaOpcode::SSetpcB64;
        requireLeafInstruction(instruction, returned, link);
        position += instruction.wordCount;
        if (returned) {
            words.resize(position);
            return words;
        }
    }
    throw std::invalid_argument("table callee exceeds the size limit without returning");
}

std::uint32_t sopp(std::uint32_t op, std::int64_t words) {
    if (words < -32768 || words > 32767) throw std::invalid_argument("table call stub branch is out of range");
    return 0xbf800000u | (op << 16u) | (static_cast<std::uint32_t>(words) & 0xffffu);
}

CapturedCallProgram resolveTableCalls(const RecompileRequest& request, const RdnaProgram& program, SrtMemoryReader reader, void* context) {
    CapturedCallProgram result;
    const auto userBase = request.context.userDataBaseRegister;
    const auto userCount = static_cast<std::uint32_t>(request.context.userData.size());
    const auto isUserPair = [&](std::uint32_t reg) { return reg >= userBase && reg + 1u < userBase + userCount; };
    struct TableCall {
        std::uint32_t callIndex;
        std::uint32_t targetRegister;
        std::uint32_t link;
        std::vector<std::uint64_t> targets;
    };
    std::vector<TableCall> calls;
    for (std::uint32_t index = 0; index < program.instructions.size(); ++index) {
        const auto& call = program.instructions[index];
        if (call.op != RdnaOpcode::SSwappcB64 || call.source0.kind != RdnaOperandKind::ScalarRegister || call.destination.kind != RdnaOperandKind::ScalarRegister) continue;
        auto pair = call.source0.reg;
        auto writer = lastScalarWriter(program, index, pair);
        if (!writer) return {};
        if (program.instructions[*writer].op == RdnaOpcode::SMovB32) {
            const auto high = lastScalarWriter(program, index, pair + 1u);
            const auto& low = program.instructions[*writer];
            if (!high || program.instructions[*high].op != RdnaOpcode::SMovB32 || low.source0.kind != RdnaOperandKind::ScalarRegister || program.instructions[*high].source0.kind != RdnaOperandKind::ScalarRegister || program.instructions[*high].source0.reg != low.source0.reg + 1u) return {};
            pair = low.source0.reg;
            writer = lastScalarWriter(program, std::min(*writer, *high), pair);
            if (!writer) return {};
        }
        const auto entry = decodeScalarLoad(program.instructions[*writer]);
        if (!entry || program.instructions[*writer].op == RdnaOpcode::SLoadDwordx4 || program.instructions[*writer].op == RdnaOpcode::SLoadDwordx8 || pair < entry->data || pair + 1u >= entry->data + entry->dwords) return {};
        const auto descriptorWriter = lastScalarWriter(program, *writer, entry->base);
        if (!descriptorWriter) return {};
        const auto descriptor = decodeScalarLoad(program.instructions[*descriptorWriter]);
        if (!descriptor || (program.instructions[*descriptorWriter].op != RdnaOpcode::SLoadDwordx4 && program.instructions[*descriptorWriter].op != RdnaOpcode::SLoadDwordx8) || entry->base < descriptor->data || entry->base + 3u >= descriptor->data + descriptor->dwords) return {};
        if (!isUserPair(descriptor->base) || lastScalarWriter(program, *descriptorWriter, descriptor->base) || lastScalarWriter(program, *descriptorWriter, descriptor->base + 1u)) return {};
        const auto userIndex = descriptor->base - userBase;
        const auto pointer = static_cast<std::uint64_t>(request.context.userData[userIndex]) | (static_cast<std::uint64_t>(request.context.userData[userIndex + 1u]) << 32u);
        const auto descriptorAddress = pointer + descriptor->offset + (entry->base - descriptor->data) * 4u;
        std::array<std::uint32_t, 4> sharp{};
        for (std::uint32_t word = 0; word < 4; ++word) {
            if (!readWord(reader, context, descriptorAddress + word * 4u, sharp[word])) throw std::invalid_argument("table call descriptor is unmapped");
        }
        const auto base = static_cast<std::uint64_t>(sharp[0]) | (static_cast<std::uint64_t>(sharp[1] & 0xffffu) << 32u);
        const auto stride = (sharp[1] >> 16u) & 0x3fffu;
        const auto bytes = stride == 0 ? static_cast<std::uint64_t>(sharp[2]) : static_cast<std::uint64_t>(stride) * sharp[2];
        const auto entryBytes = entry->dwords * 4u;
        const auto entryOffset = (pair - entry->data) * 4u + entry->offset;
        if (base == 0 || bytes < entryOffset + 8u) throw std::invalid_argument("table call descriptor is empty");
        const auto entries = std::min<std::uint64_t>((bytes - entryOffset - 8u) / entryBytes + 1u, MaxTableEntries);
        std::vector<std::uint64_t> targets;
        for (std::uint64_t slot = 0; slot < entries; ++slot) {
            std::uint32_t low = 0;
            std::uint32_t high = 0;
            if (!readWord(reader, context, base + slot * entryBytes + entryOffset, low) || !readWord(reader, context, base + slot * entryBytes + entryOffset + 4u, high)) break;
            const auto target = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32u);
            if (target == 0 || (target & 3u) != 0 || std::ranges::find(targets, target) != targets.end()) continue;
            targets.push_back(target);
        }
        if (targets.size() > MaxTableTargets) throw std::invalid_argument("table call has " + std::to_string(targets.size()) + " distinct targets in " + std::to_string(entries) + " entries, more than " + std::to_string(MaxTableTargets));
        if (targets.empty()) throw std::invalid_argument("table call has no targets");
        calls.push_back({index, call.source0.reg, call.destination.reg, std::move(targets)});
    }
    if (calls.empty()) return {};
    if (std::ranges::any_of(program.instructions, [](const RdnaInstruction& instruction) { return instruction.op == RdnaOpcode::SGetpcB64; })) throw std::invalid_argument("table-call shader reads its own program counter");
    const auto& last = program.instructions.back();
    const auto callerWords = (last.programCounter + last.wordCount * 4u) / 4u;
    result.code.assign(request.shader.code.begin(), request.shader.code.begin() + callerWords);
    for (const auto& table : calls) {
        const auto& call = program.instructions[table.callIndex];
        const auto returnPc = static_cast<std::int64_t>(call.programCounter + 4u);
        const auto here = [&] { return static_cast<std::int64_t>(result.code.size() * 4u); };
        result.code[call.programCounter / 4u] = sopp(2u, (here() - returnPc) / 4);
        auto previousExit = returnPc;
        std::size_t emitted = 0;
        for (const auto target : table.targets) {
            std::vector<std::uint32_t> callee;
            try {
                callee = readLeafCallee(reader, context, target, table.link);
            } catch (const std::exception& error) {
                std::uint32_t first = 0;
                static_cast<void>(readWord(reader, context, target, first));
                std::fprintf(stderr, "[table-call] shader 0x%llx target 0x%llx (first word %08x) dropped: %s\n", static_cast<unsigned long long>(request.shader.codeAddress), static_cast<unsigned long long>(target), first, error.what());
                continue;
            }
            ++emitted;
            if (callee.size() + 8u > MaxCombinedWords - result.code.size()) throw std::invalid_argument("table-call shader exceeds the combined program size limit");
            const auto testPc = here();
            const auto bodyPc = testPc + 24;
            const auto exitPc = bodyPc + static_cast<std::int64_t>(callee.size()) * 4;
            const auto nextPc = exitPc + 4;
            result.code.push_back(0xbf000000u | (6u << 16u) | (0xffu << 8u) | table.targetRegister);
            result.code.push_back(static_cast<std::uint32_t>(target));
            result.code.push_back(sopp(4u, (nextPc - (testPc + 12)) / 4));
            result.code.push_back(0xbf000000u | (6u << 16u) | (0xffu << 8u) | (table.targetRegister + 1u));
            result.code.push_back(static_cast<std::uint32_t>(target >> 32u));
            result.code.push_back(sopp(4u, (nextPc - (testPc + 24)) / 4));
            callee.back() = sopp(2u, (exitPc - (exitPc - 4 + 4)) / 4);
            result.code.insert(result.code.end(), callee.begin(), callee.end());
            result.code.push_back(sopp(2u, (previousExit - (exitPc + 4)) / 4));
            previousExit = exitPc;
        }
        if (emitted == 0) throw std::invalid_argument("table call has no decodable targets");
        result.code.push_back(sopp(2u, (previousExit - (here() + 4)) / 4));
    }
    result.code.push_back(0xbf810000u);
    auto shader = request.shader;
    shader.code = result.code;
    static_cast<void>(GraphBuilder{}.Build(DecodeShaderProgram(shader), nullptr));
    return result;
}

}

CapturedCallProgram ResolveUserDataCalls(const RecompileRequest& request, SrtMemoryReader reader, void* context) {
    if (!request.shader.capturedCalls.empty()) throw std::invalid_argument("shader calls have already been captured");
    if (request.shader.code.size() > MaxCombinedWords) throw std::invalid_argument("captured shader exceeds the combined program size limit");
    const auto program = RdnaInstructionDecoder{}.Decode(request.shader.code);
    const SwappcInfo info{request.context.vertex.has_value(), request.context.userDataBaseRegister, static_cast<std::uint32_t>(request.context.userData.size())};
    std::vector<UserDataCall> targets;
    try {
        targets = AnalyzeUserDataCalls(program, info);
    } catch (const std::invalid_argument&) {
        if (reader == nullptr) throw;
        auto table = resolveTableCalls(request, program, reader, context);
        if (table.code.empty()) throw;
        return table;
    }
    CapturedCallProgram result;
    if (targets.empty()) return result;
    if (reader == nullptr) throw std::invalid_argument("captured scalar calls require a memory reader");
    result.code.assign(request.shader.code.begin(), request.shader.code.end());
    for (const auto& target : targets) {
        const auto address = static_cast<std::uint64_t>(request.context.userData[target.userDataIndex]) |
            (static_cast<std::uint64_t>(request.context.userData[target.userDataIndex + 1u]) << 32u);
        if (address == 0 || (address & 3u) != 0u) throw std::invalid_argument("captured scalar call target is null or misaligned");
        if (address >= request.shader.codeAddress && address - request.shader.codeAddress < request.shader.code.size_bytes()) {
            throw std::invalid_argument("recursive captured scalar call targets the caller");
        }
        const auto& call = program.instructions[target.callIndex];
        const auto start = static_cast<std::uint32_t>(result.code.size() * 4u);
        std::vector<std::uint32_t> callee;
        bool returned = false;
        while (callee.size() < MaxCalleeWords) {
            const auto wordIndex = static_cast<std::uint32_t>(callee.size());
            RdnaInstruction instruction;
            for (std::size_t words = 0; ; ++words) {
                if (words >= MaxRdnaInstructionRawWords || callee.size() >= MaxCalleeWords) throw std::invalid_argument("captured callee instruction exceeds the size limit");
                const auto offset = callee.size() * 4u;
                if (address > std::numeric_limits<std::uint64_t>::max() - offset - 4u) throw std::invalid_argument("captured scalar call target arithmetic overflow");
                std::uint32_t word = 0;
                if (!reader(context, address + offset, &word)) throw std::invalid_argument("captured scalar call target is unmapped");
                callee.push_back(word);
                try {
                    instruction = DecodeRdnaInstruction(wordIndex * 4u, callee, wordIndex);
                    break;
                } catch (const std::out_of_range&) {
                    if (words + 1u == MaxRdnaInstructionRawWords) throw;
                }
            }
            returned = instruction.op == RdnaOpcode::SSetpcB64;
            requireLeafInstruction(instruction, returned, call.destination.reg);
            for (const auto& source : targets) {
                const auto reg = request.context.userDataBaseRegister + source.userDataIndex;
                if (WritesScalarRegister(instruction, reg) || WritesScalarRegister(instruction, reg + 1u)) {
                    throw std::invalid_argument("captured callee clobbers a user-data call target");
                }
            }
            if (returned) break;
        }
        if (!returned) throw std::invalid_argument("captured callee exceeds the size limit without returning");
        if (callee.size() > MaxCombinedWords - result.code.size()) throw std::invalid_argument("captured shader exceeds the combined program size limit");
        result.calls.push_back({call.programCounter, start, static_cast<std::uint32_t>(start + callee.size() * 4u - 4u), address, target.userDataIndex});
        result.code.insert(result.code.end(), callee.begin(), callee.end());
    }
    auto shader = request.shader;
    shader.code = result.code;
    shader.capturedCalls = result.calls;
    const auto combined = DecodeShaderProgram(shader);
    auto capturedInfo = info;
    capturedInfo.capturedCalls = result.calls;
    static_cast<void>(GraphBuilder{}.Build(combined, &capturedInfo));
    return result;
}

RdnaProgram DecodeShaderProgram(const ShaderBinary& shader) {
    if (shader.capturedCalls.size() > MaxCapturedShaderCalls) throw std::invalid_argument("too many captured scalar calls");
    if (!shader.capturedCalls.empty() && shader.code.size() > MaxCombinedWords) throw std::invalid_argument("captured shader exceeds the combined program size limit");
    auto program = RdnaInstructionDecoder{}.Decode(shader.code);
    const auto callerEnd = program.instructions.back().programCounter + program.instructions.back().wordCount * 4u;
    auto end = callerEnd;
    for (const auto& call : shader.capturedCalls) {
        const auto* instruction = FindInstructionAtProgramCounter(program, call.callProgramCounter);
        if (instruction == nullptr || instruction->op != RdnaOpcode::SSwappcB64 || call.callProgramCounter >= callerEnd ||
            call.targetProgramCounter < end || call.returnProgramCounter < call.targetProgramCounter || (call.targetProgramCounter & 3u) != 0 ||
            (call.returnProgramCounter & 3u) != 0 || static_cast<std::uint64_t>(call.returnProgramCounter) + 4u > shader.code.size_bytes()) {
            throw std::invalid_argument("invalid captured scalar call boundaries");
        }
        const auto bytes = static_cast<std::uint64_t>(call.returnProgramCounter) + 4u - call.targetProgramCounter;
        if (bytes > MaxCalleeWords * 4u || call.targetAddress == 0 || (call.targetAddress & 3u) != 0 || call.targetAddress > std::numeric_limits<std::uint64_t>::max() - bytes) {
            throw std::invalid_argument("invalid captured scalar call target or size");
        }
        const auto link = instruction->destination.reg;
        for (auto pc = call.targetProgramCounter; pc <= call.returnProgramCounter; ) {
            auto decoded = DecodeRdnaInstruction(pc, shader.code, pc / 4u);
            const bool last = pc == call.returnProgramCounter;
            requireLeafInstruction(decoded, last, link);
            if (last && decoded.op != RdnaOpcode::SSetpcB64) throw std::invalid_argument("captured callee has no return");
            if (decoded.wordCount * 4u > call.returnProgramCounter + 4u - pc) throw std::invalid_argument("captured callee return is not an instruction boundary");
            pc += decoded.wordCount * 4u;
            program.instructions.push_back(std::move(decoded));
        }
        end = call.returnProgramCounter + 4u;
    }
    return program;
}

}
