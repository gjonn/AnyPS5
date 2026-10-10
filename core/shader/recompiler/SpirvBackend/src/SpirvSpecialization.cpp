#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp>
#undef SPV_ENABLE_UTILITY_CODE
#include "SpirvBackend/SpirvSpecialization.hpp"
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ShaderRecompiler {
namespace {

using Instruction = std::vector<std::uint32_t>;

spv::Op Opcode(const Instruction& instruction) {
    return instruction.empty() ? spv::OpNop : static_cast<spv::Op>(instruction[0] & 0xffffu);
}

std::uint32_t Result(const Instruction& instruction) {
    if (instruction.empty()) return 0u;
    bool result = false;
    bool type = false;
    spv::HasResultAndType(Opcode(instruction), &result, &type);
    return result ? instruction.at(type ? 2u : 1u) : 0u;
}

Instruction Make(spv::Op op, std::initializer_list<std::uint32_t> operands) {
    Instruction result{(static_cast<std::uint32_t>(operands.size() + 1u) << 16u) | op};
    result.insert(result.end(), operands);
    return result;
}

bool PureInstruction(const Instruction& instruction) {
    switch (Opcode(instruction)) {
    case spv::OpVectorExtractDynamic:
    case spv::OpVectorInsertDynamic:
    case spv::OpVectorShuffle:
    case spv::OpCompositeConstruct:
    case spv::OpCompositeExtract:
    case spv::OpCompositeInsert:
    case spv::OpCopyObject:
    case spv::OpTranspose:
    case spv::OpConvertFToU:
    case spv::OpConvertFToS:
    case spv::OpConvertSToF:
    case spv::OpConvertUToF:
    case spv::OpUConvert:
    case spv::OpSConvert:
    case spv::OpFConvert:
    case spv::OpQuantizeToF16:
    case spv::OpConvertPtrToU:
    case spv::OpSatConvertSToU:
    case spv::OpSatConvertUToS:
    case spv::OpConvertUToPtr:
    case spv::OpPtrCastToGeneric:
    case spv::OpGenericCastToPtr:
    case spv::OpGenericCastToPtrExplicit:
    case spv::OpBitcast:
    case spv::OpSNegate:
    case spv::OpFNegate:
    case spv::OpIAdd:
    case spv::OpFAdd:
    case spv::OpISub:
    case spv::OpFSub:
    case spv::OpIMul:
    case spv::OpFMul:
    case spv::OpUDiv:
    case spv::OpSDiv:
    case spv::OpFDiv:
    case spv::OpUMod:
    case spv::OpSRem:
    case spv::OpSMod:
    case spv::OpFRem:
    case spv::OpFMod:
    case spv::OpVectorTimesScalar:
    case spv::OpMatrixTimesScalar:
    case spv::OpVectorTimesMatrix:
    case spv::OpMatrixTimesVector:
    case spv::OpMatrixTimesMatrix:
    case spv::OpOuterProduct:
    case spv::OpDot:
    case spv::OpIAddCarry:
    case spv::OpISubBorrow:
    case spv::OpUMulExtended:
    case spv::OpSMulExtended:
    case spv::OpAny:
    case spv::OpAll:
    case spv::OpIsNan:
    case spv::OpIsInf:
    case spv::OpIsFinite:
    case spv::OpIsNormal:
    case spv::OpSignBitSet:
    case spv::OpLessOrGreater:
    case spv::OpOrdered:
    case spv::OpUnordered:
    case spv::OpLogicalEqual:
    case spv::OpLogicalNotEqual:
    case spv::OpLogicalOr:
    case spv::OpLogicalAnd:
    case spv::OpLogicalNot:
    case spv::OpSelect:
    case spv::OpIEqual:
    case spv::OpINotEqual:
    case spv::OpUGreaterThan:
    case spv::OpSGreaterThan:
    case spv::OpUGreaterThanEqual:
    case spv::OpSGreaterThanEqual:
    case spv::OpULessThan:
    case spv::OpSLessThan:
    case spv::OpULessThanEqual:
    case spv::OpSLessThanEqual:
    case spv::OpFOrdEqual:
    case spv::OpFUnordEqual:
    case spv::OpFOrdNotEqual:
    case spv::OpFUnordNotEqual:
    case spv::OpFOrdLessThan:
    case spv::OpFUnordLessThan:
    case spv::OpFOrdGreaterThan:
    case spv::OpFUnordGreaterThan:
    case spv::OpFOrdLessThanEqual:
    case spv::OpFUnordLessThanEqual:
    case spv::OpFOrdGreaterThanEqual:
    case spv::OpFUnordGreaterThanEqual:
    case spv::OpShiftRightLogical:
    case spv::OpShiftRightArithmetic:
    case spv::OpShiftLeftLogical:
    case spv::OpBitwiseOr:
    case spv::OpBitwiseXor:
    case spv::OpBitwiseAnd:
    case spv::OpNot:
    case spv::OpBitFieldInsert:
    case spv::OpBitFieldSExtract:
    case spv::OpBitFieldUExtract:
    case spv::OpBitReverse:
    case spv::OpBitCount:
    case spv::OpDPdx:
    case spv::OpDPdy:
    case spv::OpFwidth:
    case spv::OpDPdxFine:
    case spv::OpDPdyFine:
    case spv::OpFwidthFine:
    case spv::OpDPdxCoarse:
    case spv::OpDPdyCoarse:
    case spv::OpFwidthCoarse:
    case spv::OpAccessChain:
    case spv::OpInBoundsAccessChain:
    case spv::OpPtrAccessChain:
    case spv::OpInBoundsPtrAccessChain:
    case spv::OpArrayLength:
    case spv::OpPhi:
        return true;
    case spv::OpLoad:
        return instruction.size() == 4u || (instruction.size() == 5u && instruction[4] == spv::MemoryAccessMaskNone);
    default:
        return false;
    }
}

template<typename TVisitor>
bool VisitInputs(const Instruction& instruction, const TVisitor& visit) {
    std::size_t first = 3u;
    std::size_t end = instruction.size();
    switch (Opcode(instruction)) {
    case spv::OpVectorShuffle:
    case spv::OpCompositeInsert:
        end = 5u;
        break;
    case spv::OpCompositeExtract:
    case spv::OpGenericCastToPtrExplicit:
    case spv::OpArrayLength:
        end = 4u;
        break;
    case spv::OpLoad:
        if (instruction.size() > 5u) return false;
        end = 4u;
        break;
    case spv::OpStore:
        if (instruction.size() > 4u) return false;
        first = 1u;
        end = 3u;
        break;
    case spv::OpBranch:
    case spv::OpReturnValue:
        first = 1u;
        end = 2u;
        break;
    case spv::OpBranchConditional:
        first = 1u;
        end = 4u;
        break;
    case spv::OpSelectionMerge:
        first = 1u;
        end = 2u;
        break;
    case spv::OpLoopMerge:
        first = 1u;
        end = 3u;
        break;
    case spv::OpFunctionCall:
        break;
    case spv::OpReturn:
    case spv::OpUnreachable:
    case spv::OpKill:
    case spv::OpLabel:
    case spv::OpFunction:
    case spv::OpFunctionEnd:
    case spv::OpFunctionParameter:
    case spv::OpLine:
    case spv::OpNoLine:
        return true;
    default:
        if (!PureInstruction(instruction)) return false;
        break;
    }
    if (end > instruction.size()) throw std::runtime_error("truncated prepared instruction operands");
    for (auto index = first; index < end; ++index) visit(index);
    return true;
}

struct ScalarType {
    std::uint32_t width;
    bool boolean;
};

class Specialization {
public:
    explicit Specialization(std::span<const std::uint32_t> words) {
        if (words.size() < 5u || words[0] != spv::MagicNumber) throw std::runtime_error("invalid prepared SPIR-V header");
        header.assign(words.begin(), words.begin() + 5);
        const auto bound = static_cast<std::size_t>(header[3]);
        types.assign(bound, ScalarType{0u, false});
        values.assign(bound, 0u);
        known.assign(bound, 0u);
        resultTypes.assign(bound, 0u);
        removed.assign(bound, 0u);
        instructions.reserve(words.size() / 4u);
        for (std::size_t cursor = 5; cursor < words.size();) {
            const auto count = words[cursor] >> 16u;
            if (count == 0u || count > words.size() - cursor) throw std::runtime_error("truncated prepared SPIR-V instruction");
            instructions.emplace_back(words.begin() + cursor, words.begin() + cursor + count);
            const auto& instruction = instructions.back();
            const auto op = Opcode(instruction);
            bool hasResult = false;
            bool hasType = false;
            spv::HasResultAndType(op, &hasResult, &hasType);
            if (hasResult && hasType && resultType(instruction.at(2)) == 0u) at(resultTypes, instruction.at(2)) = instruction.at(1);
            if (op == spv::OpTypeBool && !type(instruction.at(1))) at(types, instruction.at(1)) = ScalarType{1u, true};
            if (op == spv::OpTypeInt && !type(instruction.at(1))) at(types, instruction.at(1)) = ScalarType{instruction.at(2), false};
            if (op == spv::OpConstant && type(instruction.at(1)) && type(instruction[1])->width <= 32u && !value(instruction.at(2))) setValue(instruction.at(2), instruction.at(3));
            if ((op == spv::OpConstantTrue || op == spv::OpConstantFalse) && !value(instruction.at(2))) setValue(instruction.at(2), op == spv::OpConstantTrue ? 1u : 0u);
            cursor += count;
        }
    }

    std::vector<std::uint32_t> Run() {
        bool changed = true;
        while (changed) {
            changed = fold();
            changed |= prune();
            changed |= propagateCopies();
        }
        removeDeadComputations();
        orderPhis();
        removeDeadScalarConstants();
        std::size_t total = header.size();
        for (const auto& constant : constants) total += constant.size();
        for (const auto& instruction : instructions) total += instruction.size();
        std::vector<std::uint32_t> words;
        words.reserve(total);
        words.insert(words.end(), header.begin(), header.end());
        bool inserted = false;
        for (const auto& instruction : instructions) {
            if (instruction.empty()) continue;
            if (!inserted && Opcode(instruction) == spv::OpFunction) {
                for (const auto& constant : constants) words.insert(words.end(), constant.begin(), constant.end());
                inserted = true;
            }
            if ((Opcode(instruction) == spv::OpName || Opcode(instruction) == spv::OpDecorate) && isRemoved(instruction.at(1))) continue;
            words.insert(words.end(), instruction.begin(), instruction.end());
        }
        return words;
    }

private:
    void removeDeadScalarConstants() {
        const auto scalarConstant = [](const Instruction& instruction) {
            const auto op = Opcode(instruction);
            return op == spv::OpConstant || op == spv::OpConstantTrue || op == spv::OpConstantFalse;
        };
        std::set<std::uint32_t> referenced;
        for (const auto& instruction : instructions) {
            const auto op = Opcode(instruction);
            if (instruction.empty() || scalarConstant(instruction) || op == spv::OpName || op == spv::OpDecorate) continue;
            bool hasResult = false;
            bool hasType = false;
            spv::HasResultAndType(op, &hasResult, &hasType);
            const auto resultIndex = hasResult ? (hasType ? 2u : 1u) : 0u;
            for (std::size_t index = 1; index < instruction.size(); ++index) {
                if (index != resultIndex) referenced.insert(instruction[index]);
            }
        }
        for (auto* collection : {&instructions, &constants}) {
            for (auto& instruction : *collection) {
                if (!scalarConstant(instruction) || referenced.contains(Result(instruction))) continue;
                at(removed, Result(instruction)) = 1u;
                instruction.clear();
            }
        }
    }

    struct Block {
        std::size_t first = 0;
        std::size_t last = 0;
        std::size_t count = 0;
    };

    template<typename T>
    static T& at(std::vector<T>& table, std::uint32_t id) {
        if (id >= table.size()) table.resize(static_cast<std::size_t>(id) + 1u, T{});
        return table[id];
    }

    template<typename T>
    static const T* find(const std::vector<T>& table, std::uint32_t id) {
        return id < table.size() ? &table[id] : nullptr;
    }

    const ScalarType* type(std::uint32_t id) const {
        const auto* entry = find(types, id);
        return entry != nullptr && entry->width != 0u ? entry : nullptr;
    }

    std::uint32_t resultType(std::uint32_t id) const {
        const auto* entry = find(resultTypes, id);
        return entry != nullptr ? *entry : 0u;
    }

    bool isRemoved(std::uint32_t id) const {
        const auto* entry = find(removed, id);
        return entry != nullptr && *entry != 0u;
    }

    void setValue(std::uint32_t id, std::uint32_t bits) {
        at(values, id) = bits;
        at(known, id) = 1u;
    }

    std::optional<std::uint32_t> value(std::uint32_t id) const {
        const auto* flag = find(known, id);
        return flag != nullptr && *flag != 0u ? std::optional(values[id]) : std::nullopt;
    }

    static bool stamped(const std::vector<std::uint32_t>& stamps, std::uint32_t id, std::uint32_t generation) {
        return id < stamps.size() && stamps[id] == generation;
    }

    static bool stamp(std::vector<std::uint32_t>& stamps, std::uint32_t id, std::uint32_t generation) {
        auto& entry = at(stamps, id);
        if (entry == generation) return false;
        entry = generation;
        return true;
    }

    std::optional<std::uint32_t> evaluate(const Instruction& instruction) const {
        const auto op = Opcode(instruction);
        if (instruction.size() < 4u) return std::nullopt;
        const auto* scalar = type(instruction[1]);
        if (scalar == nullptr || scalar->width > 32u) return std::nullopt;
        const auto left = value(instruction[3]);
        if (!left) return std::nullopt;
        if (op == spv::OpCopyObject) return left;
        if (op == spv::OpLogicalNot) return *left == 0u;
        if (op == spv::OpNot) return ~*left;
        if (instruction.size() < 5u) return std::nullopt;
        const auto right = value(instruction[4]);
        if (!right) return std::nullopt;
        switch (op) {
        case spv::OpIAdd: return *left + *right;
        case spv::OpISub: return *left - *right;
        case spv::OpIMul: return *left * *right;
        case spv::OpUDiv: return *right != 0u ? std::optional(*left / *right) : std::nullopt;
        case spv::OpUMod: return *right != 0u ? std::optional(*left % *right) : std::nullopt;
        case spv::OpBitwiseAnd: return *left & *right;
        case spv::OpBitwiseOr: return *left | *right;
        case spv::OpBitwiseXor: return *left ^ *right;
        case spv::OpShiftRightLogical: return *right < scalar->width ? std::optional(*left >> *right) : std::nullopt;
        case spv::OpShiftLeftLogical: return *right < scalar->width ? std::optional(*left << *right) : std::nullopt;
        case spv::OpIEqual: return *left == *right;
        case spv::OpINotEqual: return *left != *right;
        case spv::OpULessThan: return *left < *right;
        case spv::OpULessThanEqual: return *left <= *right;
        case spv::OpUGreaterThan: return *left > *right;
        case spv::OpUGreaterThanEqual: return *left >= *right;
        case spv::OpLogicalEqual: return (*left != 0u) == (*right != 0u);
        case spv::OpLogicalNotEqual: return (*left != 0u) != (*right != 0u);
        case spv::OpLogicalAnd: return *left != 0u && *right != 0u;
        case spv::OpLogicalOr: return *left != 0u || *right != 0u;
        case spv::OpBitFieldUExtract: {
            if (instruction.size() != 6u) throw std::runtime_error("invalid prepared bitfield instruction");
            const auto count = value(instruction[5]);
            if (!count || *right > 32u || *count > 32u - *right) return std::nullopt;
            if (*count == 0u) return 0u;
            const auto mask = *count == 32u ? UINT32_MAX : (1u << *count) - 1u;
            return (*left >> *right) & mask;
        }
        default: return std::nullopt;
        }
    }

    bool fold() {
        bool changed = false;
        bool function = false;
        const auto generation = ++extractGeneration;
        const auto extracted = [&](std::uint32_t id) { return stamped(extractStamps, id, generation); };
        for (auto& instruction : instructions) {
            if (instruction.empty()) continue;
            auto op = Opcode(instruction);
            if (op == spv::OpFunction) function = true;
            if (op == spv::OpFunctionEnd) function = false;
            if (!function) continue;
            if (op == spv::OpSelect && instruction.size() == 6u) {
                if (const auto condition = value(instruction[3])) {
                    instruction = Make(spv::OpCopyObject, {instruction[1], instruction[2], instruction[*condition != 0u ? 4u : 5u]});
                    changed = true;
                }
            }
            if (op == spv::OpVectorExtractDynamic && instruction.size() == 5u) {
                if (const auto index = value(instruction[4])) {
                    instruction = Make(spv::OpCompositeExtract, {instruction[1], instruction[2], instruction[3], *index});
                    changed = true;
                }
            }
            op = Opcode(instruction);
            if (op == spv::OpCompositeExtract && instruction.size() == 5u && stamp(extractStamps, instruction[2], generation)) {
                at(extractSources, instruction[2]) = instruction[3];
                at(extractChannels, instruction[2]) = instruction[4];
            }
            if (op == spv::OpCompositeConstruct && instruction.size() == 7u) {
                bool shuffle = extracted(instruction[3]);
                for (std::size_t index = 4; shuffle && index < instruction.size(); ++index) {
                    shuffle = extracted(instruction[index]) && extractSources[instruction[index]] == extractSources[instruction[3]];
                }
                if (shuffle) {
                    const auto source = extractSources[instruction[3]];
                    bool identity = resultType(source) != 0u && resultType(source) == instruction[1];
                    Instruction replacement = Make(spv::OpVectorShuffle, {instruction[1], instruction[2], source, source});
                    for (std::size_t index = 3; index < instruction.size(); ++index) {
                        const auto channel = extractChannels[instruction[index]];
                        replacement.push_back(channel);
                        identity &= channel == index - 3u;
                    }
                    replacement[0] = (static_cast<std::uint32_t>(replacement.size()) << 16u) | spv::OpVectorShuffle;
                    instruction = identity ? Make(spv::OpCopyObject, {instruction[1], instruction[2], source}) : std::move(replacement);
                    changed = true;
                }
            }
            const auto result = evaluate(instruction);
            if (!result) continue;
            const auto scalar = *type(instruction[1]);
            const auto bits = scalar.width == 32u ? *result : *result & ((1u << scalar.width) - 1u);
            setValue(instruction[2], bits);
            constants.push_back(scalar.boolean ? Make(bits != 0u ? spv::OpConstantTrue : spv::OpConstantFalse, {instruction[1], instruction[2]}) : Make(spv::OpConstant, {instruction[1], instruction[2], bits}));
            instruction.clear();
            changed = true;
        }
        return changed;
    }

    bool prune() {
        const auto generation = ++blockGeneration;
        labels.clear();
        std::vector<std::uint32_t> entries;
        std::uint32_t current = 0;
        bool entry = false;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            const auto& instruction = instructions[index];
            if (instruction.empty()) continue;
            const auto op = Opcode(instruction);
            if (op == spv::OpFunction) entry = true;
            if (op == spv::OpFunctionEnd) current = 0;
            if (op == spv::OpLabel) {
                current = instruction.at(1);
                if (entry) entries.push_back(current);
                entry = false;
            }
            if (current == 0u) continue;
            if (stamp(blockStamps, current, generation)) {
                at(blocks, current) = Block{index, index, 0u};
                labels.push_back(current);
            }
            auto& range = blocks[current];
            range.last = index;
            ++range.count;
        }
        std::sort(labels.begin(), labels.end());
        bool changed = false;
        for (const auto label : labels) {
            const auto range = blocks[label];
            auto& terminal = instructions[range.last];
            auto op = Opcode(terminal);
            std::uint32_t target = 0;
            bool loop = false;
            for (auto index = range.first; index <= range.last; ++index) loop |= Opcode(instructions[index]) == spv::OpLoopMerge;
            if (!loop && op == spv::OpBranchConditional) {
                if (const auto condition = value(terminal.at(1))) target = terminal.at(*condition != 0u ? 2u : 3u);
            } else if (!loop && op == spv::OpSwitch) {
                if (const auto selector = value(terminal.at(1))) {
                    target = terminal.at(2);
                    if ((terminal.size() - 3u) % 2u != 0u) throw std::runtime_error("invalid prepared 32-bit switch");
                    for (std::size_t index = 3; index < terminal.size(); index += 2u) if (terminal[index] == *selector) target = terminal[index + 1u];
                }
            }
            if (target != 0u) {
                for (auto index = range.first; index <= range.last; ++index) if (Opcode(instructions[index]) == spv::OpSelectionMerge) instructions[index].clear();
                terminal = Make(spv::OpBranch, {target});
                op = spv::OpBranch;
                changed = true;
            }
            auto& successors = at(edges, label);
            successors.clear();
            if (op == spv::OpBranch) successors.push_back(terminal.at(1));
            else if (op == spv::OpBranchConditional) successors = {terminal.at(2), terminal.at(3)};
            else if (op == spv::OpSwitch) {
                successors.push_back(terminal.at(2));
                const auto selectorType = resultType(terminal.at(1));
                const auto* selector = selectorType != 0u ? type(selectorType) : nullptr;
                const auto stride = selector != nullptr && selector->width == 64u ? 3u : 2u;
                for (std::size_t index = 3; index + stride <= terminal.size(); index += stride) successors.push_back(terminal[index + stride - 1u]);
            }
        }
        const auto live = ++liveGeneration;
        auto pending = entries;
        while (!pending.empty()) {
            const auto label = pending.back();
            pending.pop_back();
            if (!stamped(blockStamps, label, generation)) throw std::runtime_error("prepared branch references a missing block");
            if (!stamp(liveStamps, label, live)) continue;
            const auto& successors = edges[label];
            pending.insert(pending.end(), successors.begin(), successors.end());
        }
        const auto isLive = [&](std::uint32_t label) { return stamped(liveStamps, label, live); };
        for (const auto label : labels) {
            const auto range = blocks[label];
            if (!isLive(label)) {
                if (range.count == 2u && Opcode(instructions[range.last]) == spv::OpUnreachable) continue;
                for (auto index = range.first + 1u; index <= range.last; ++index) {
                    auto& instruction = instructions[index];
                    if (const auto result = Result(instruction)) at(removed, result) = 1u;
                    instruction.clear();
                }
                instructions[range.last] = Make(spv::OpUnreachable, {});
                changed = true;
                continue;
            }
            for (auto index = range.first; index <= range.last; ++index) {
                auto& instruction = instructions[index];
                if (instruction.empty() || Opcode(instruction) != spv::OpPhi) continue;
                Instruction phi(instruction.begin(), instruction.begin() + 3);
                for (std::size_t operand = 3; operand + 1u < instruction.size(); operand += 2u) {
                    const auto parent = instruction[operand + 1u];
                    if (isLive(parent) && std::ranges::find(edges[parent], label) != edges[parent].end()) phi.insert(phi.end(), {instruction[operand], parent});
                }
                if (phi.size() == 3u) throw std::runtime_error("reachable prepared phi has no predecessor");
                if (phi.size() == instruction.size() && phi.size() != 5u) continue;
                phi[0] = (static_cast<std::uint32_t>(phi.size()) << 16u) | spv::OpPhi;
                instruction = phi.size() == 5u ? Make(spv::OpCopyObject, {phi[1], phi[2], phi[3]}) : std::move(phi);
                changed = true;
            }
        }
        return changed;
    }

    void orderPhis() {
        std::vector<Instruction> ordered;
        ordered.reserve(instructions.size());
        std::vector<Instruction> copies;
        bool prefix = false;
        for (auto& instruction : instructions) {
            if (instruction.empty()) continue;
            const auto op = Opcode(instruction);
            if (op == spv::OpLabel) prefix = true;
            else if (prefix && op == spv::OpCopyObject) {
                copies.push_back(std::move(instruction));
                continue;
            } else if (prefix && op != spv::OpPhi && op != spv::OpLine && op != spv::OpNoLine) {
                for (auto& copy : copies) ordered.push_back(std::move(copy));
                copies.clear();
                prefix = false;
            }
            ordered.push_back(std::move(instruction));
        }
        if (!copies.empty()) throw std::runtime_error("unterminated prepared phi block");
        instructions = std::move(ordered);
    }

    bool propagateCopies() {
        copyKeys.clear();
        std::size_t copyCount = 0;
        for (const auto& instruction : instructions) {
            if (Opcode(instruction) != spv::OpCopyObject) continue;
            if (instruction.size() != 4u) throw std::runtime_error("invalid prepared copy instruction");
            auto& source = at(copies, instruction[2]);
            if (source != 0u) continue;
            source = instruction[3];
            copyKeys.push_back(instruction[2]);
            ++copyCount;
        }
        const auto clear = [&] {
            for (const auto key : copyKeys) copies[key] = 0u;
            copyKeys.clear();
        };
        if (copyCount == 0u) return false;
        const auto copyOf = [&](std::uint32_t id) { return id < copies.size() ? copies[id] : 0u; };
        for (const auto& instruction : instructions) {
            if (instruction.empty()) continue;
            const auto op = Opcode(instruction);
            if (op == spv::OpName || op == spv::OpMemberName) continue;
            if (VisitInputs(instruction, [](std::size_t) {})) continue;
            bool hasResult = false;
            bool hasType = false;
            spv::HasResultAndType(op, &hasResult, &hasType);
            const auto resultIndex = hasResult ? (hasType ? 2u : 1u) : 0u;
            for (std::size_t index = 1; index < instruction.size(); ++index) {
                if (index == resultIndex || copyOf(instruction[index]) == 0u) continue;
                copies[instruction[index]] = 0u;
                --copyCount;
            }
        }
        if (copyCount == 0u) {
            clear();
            return false;
        }
        const auto resolve = [&](std::uint32_t id) {
            std::size_t count = 0;
            while (copyOf(id) != 0u) {
                if (++count > copyCount) throw std::runtime_error("cyclic prepared copy chain");
                id = copies[id];
            }
            return id;
        };
        bool function = false;
        for (auto& instruction : instructions) {
            if (instruction.empty()) continue;
            const auto op = Opcode(instruction);
            if (op == spv::OpFunction) function = true;
            if (op == spv::OpFunctionEnd) function = false;
            if (!function) continue;
            if (op == spv::OpCopyObject && copyOf(instruction[2]) != 0u) {
                at(removed, instruction[2]) = 1u;
                instruction.clear();
                continue;
            }
            VisitInputs(instruction, [&](std::size_t index) { instruction[index] = resolve(instruction[index]); });
        }
        clear();
        return true;
    }

    void removeDeadComputations() {
        constexpr std::size_t None = static_cast<std::size_t>(-1);
        std::vector<std::size_t> definitions(header[3], None);
        std::vector<std::uint32_t> defined;
        std::vector<std::uint32_t> pending;
        bool function = false;
        for (std::size_t index = 0; index < instructions.size(); ++index) {
            const auto& instruction = instructions[index];
            if (instruction.empty()) continue;
            const auto op = Opcode(instruction);
            if (op == spv::OpFunction) function = true;
            if (op == spv::OpFunctionEnd) function = false;
            if (!function) {
                if (op == spv::OpDecorateId || op == spv::OpGroupDecorate || op == spv::OpGroupMemberDecorate) {
                    for (std::size_t operand = 1; operand < instruction.size(); ++operand) pending.push_back(instruction[operand]);
                }
                continue;
            }
            if (PureInstruction(instruction)) {
                const auto result = Result(instruction);
                auto& definition = at(definitions, result);
                if (definition == None) {
                    definition = index;
                    defined.push_back(result);
                }
                continue;
            }
            if (VisitInputs(instruction, [&](std::size_t operand) { pending.push_back(instruction[operand]); })) continue;
            bool hasResult = false;
            bool hasType = false;
            spv::HasResultAndType(op, &hasResult, &hasType);
            const auto resultIndex = hasResult ? (hasType ? 2u : 1u) : 0u;
            for (std::size_t operand = 1; operand < instruction.size(); ++operand) if (operand != resultIndex) pending.push_back(instruction[operand]);
        }
        const auto live = ++liveGeneration;
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            if (id >= definitions.size() || definitions[id] == None || !stamp(liveStamps, id, live)) continue;
            const auto& instruction = instructions[definitions[id]];
            if (!VisitInputs(instruction, [&](std::size_t operand) { pending.push_back(instruction[operand]); })) throw std::runtime_error("missing prepared pure instruction operands");
        }
        for (const auto id : defined) {
            if (stamped(liveStamps, id, live)) continue;
            at(removed, id) = 1u;
            instructions[definitions[id]].clear();
        }
    }

    std::vector<std::uint32_t> header;
    std::vector<Instruction> instructions;
    std::vector<Instruction> constants;
    std::vector<ScalarType> types;
    std::vector<std::uint32_t> values;
    std::vector<std::uint8_t> known;
    std::vector<std::uint32_t> resultTypes;
    std::vector<std::uint8_t> removed;
    std::vector<std::uint32_t> extractStamps;
    std::vector<std::uint32_t> extractSources;
    std::vector<std::uint32_t> extractChannels;
    std::uint32_t extractGeneration = 0;
    std::vector<Block> blocks;
    std::vector<std::uint32_t> blockStamps;
    std::vector<std::uint32_t> labels;
    std::uint32_t blockGeneration = 0;
    std::vector<std::vector<std::uint32_t>> edges;
    std::vector<std::uint32_t> liveStamps;
    std::uint32_t liveGeneration = 0;
    std::vector<std::uint32_t> copies;
    std::vector<std::uint32_t> copyKeys;
};

}

std::vector<std::uint32_t> SpecializeSpirv(std::span<const std::uint32_t> words) {
    return Specialization(words).Run();
}

}
