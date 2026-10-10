#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/src/Driver/Capture/CaptureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace AgcDriver::DriverDetail {

void Driver::RestoreCaptureState(std::span<const std::byte> state, bool prepareShaders) {
    CaptureFormat::Reader in(state);
    require(in.U32() == CaptureFormat::Version, "frame capture: unsupported state.bin version");
    const auto queueCount = in.U32();
    std::map<std::uint32_t, QueueState> restored;
    for (std::uint32_t i = 0; i < queueCount; ++i) {
        const auto id = in.U32();
        restored.insert_or_assign(id, CaptureFormat::ReadQueue(in));
    }
    const bool reset = in.U8() != 0;
    {
        std::lock_guard lock(mutex);
        for (auto& [id, queue] : restored) queues.insert_or_assign(id, std::move(queue));
        resetGraphics = reset;
    }
    const auto shaderCount = in.U32();
    std::vector<ShaderSnapshot> snapshots;
    snapshots.reserve(shaderCount);
    for (std::uint32_t i = 0; i < shaderCount; ++i) {
        const auto codeAddress = in.U64();
        const auto headerAddress = in.U64();
        const auto type = in.U8();
        auto code = in.Span<std::uint32_t>();
        auto header = in.Span<std::byte>();
        snapshots.push_back(ShaderSnapshot{codeAddress, headerAddress, type, std::move(code), std::move(header)});
    }
    require(in.Done(), "frame capture: trailing bytes in state.bin");
    RestoreRegistry(std::move(snapshots), prepareShaders);
}

}

namespace AgcDriver::FrameReplay {

namespace {

using DriverDetail::CaptureFormat::EventKind;
using DriverDetail::CaptureFormat::PageBytes;

std::atomic<WorkObserver> workObserver{nullptr};
std::atomic<std::uint64_t> drawCount{0};
std::atomic<std::uint64_t> dispatchCount{0};

struct Mapping {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint32_t prot;
};

struct Run {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t offset;
};

struct Manifest {
    std::uint64_t frame = 0;
    bool drained = false;
    bool complete = false;
    std::uint64_t aliasedBytes = 0;
    std::size_t aliases = 0;
    std::uint64_t nullPixelProgram = 0;
    std::uint64_t events = 0;
    std::vector<std::uint32_t> outputs;
    std::vector<Mapping> mappings;
    std::vector<Run> runs;
};

Manifest ReadManifest(const std::filesystem::path& directory) {
    std::ifstream file(directory / "manifest.txt");
    if (!file) throw std::runtime_error("frame replay: cannot open " + (directory / "manifest.txt").string());
    Manifest manifest;
    std::string line;
    bool versioned = false;
    while (std::getline(file, line)) {
        std::istringstream fields(line);
        std::string key;
        fields >> key;
        const auto number = [&] {
            std::string text;
            fields >> text;
            return std::stoull(text, nullptr, 0);
        };
        if (key == "aps5-frame-capture") {
            if (number() != DriverDetail::CaptureFormat::Version) throw std::runtime_error("frame replay: unsupported capture version");
            versioned = true;
        } else if (key == "frame") {
            manifest.frame = number();
        } else if (key == "drained") {
            manifest.drained = number() != 0;
        } else if (key == "null-pixel-program") {
            manifest.nullPixelProgram = number();
        } else if (key == "events") {
            manifest.events = number();
        } else if (key == "complete") {
            manifest.complete = number() != 0;
        } else if (key == "alias") {
            number();
            number();
            manifest.aliasedBytes += number();
            ++manifest.aliases;
        } else if (key == "output") {
            manifest.outputs.push_back(static_cast<std::uint32_t>(number()));
        } else if (key == "mapping") {
            Mapping mapping{};
            mapping.begin = number();
            mapping.end = number();
            mapping.prot = static_cast<std::uint32_t>(number());
            manifest.mappings.push_back(mapping);
        } else if (key == "run") {
            Run run{};
            run.begin = number();
            run.end = number();
            run.offset = number();
            manifest.runs.push_back(run);
        }
    }
    if (!versioned) throw std::runtime_error("frame replay: " + (directory / "manifest.txt").string() + " is not a frame capture manifest");
    std::sort(manifest.mappings.begin(), manifest.mappings.end(), [](const Mapping& a, const Mapping& b) { return a.begin < b.begin; });
    return manifest;
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("frame replay: cannot open " + path.string());
    file.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("frame replay: cannot read " + path.string());
    return bytes;
}

constexpr std::size_t CommitGranule = 0x4000;
constexpr std::size_t CommitChunk = 0x200000;

#ifdef _WIN32
constexpr std::uint64_t AllocationGranule = 0x10000;
std::set<std::uint64_t> ownedGranules;
#endif

// Maps [begin, end) at its guest address: inside the guest arena as the title's commits do, outside
// it (a title's own image, say) only where the replay process left the addresses free.
bool Commit(std::uint64_t begin, std::uint64_t end) {
    try {
#ifdef _WIN32
        auto* pointer = reinterpret_cast<void*>(begin);
        if (GuestArena::GuestArenaContains_nid_postfix(pointer, static_cast<std::size_t>(end - begin))) {
            GuestArena::GuestArenaCommit_nid_postfix(pointer, static_cast<std::size_t>(end - begin), PAGE_READWRITE, CommitChunk);
            return true;
        }
        const auto first = begin & ~(AllocationGranule - 1);
        const auto stop = (end + AllocationGranule - 1) & ~(AllocationGranule - 1);
        for (auto granule = first; granule < stop; granule += AllocationGranule) {
            if (ownedGranules.contains(granule)) continue;
            auto* base = reinterpret_cast<void*>(granule);
            MEMORY_BASIC_INFORMATION memory{};
            if (VirtualQuery(base, &memory, sizeof(memory)) != sizeof(memory) || memory.State != MEM_FREE || memory.RegionSize < AllocationGranule) return false;
            if (VirtualAlloc(base, AllocationGranule, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != base) return false;
            ownedGranules.insert(granule);
        }
        return true;
#else
        return mmap(reinterpret_cast<void*>(begin), static_cast<std::size_t>(end - begin), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == reinterpret_cast<void*>(begin);
#endif
    } catch (const std::exception& error) {
        std::fprintf(stderr, "[replay] cannot map 0x%llx+0x%llx: %s\n", static_cast<unsigned long long>(begin), static_cast<unsigned long long>(end - begin), error.what());
        return false;
    }
}

// Commits the pages of [begin, end) a replayed CPU write touches that the snapshot left reserved.
bool EnsureCommitted(std::uint64_t begin, std::uint64_t end) {
    const auto first = begin & ~(PageBytes - 1);
    const auto stop = (end + PageBytes - 1) & ~(PageBytes - 1);
    if (GuestMemory::Accessible(reinterpret_cast<const void*>(first), static_cast<std::size_t>(stop - first), true)) return true;
    for (auto page = first; page < stop; page += PageBytes) {
        if (GuestMemory::Accessible(reinterpret_cast<const void*>(page), PageBytes, true)) continue;
        const auto granule = page & ~std::uint64_t{CommitGranule - 1};
        if (!Commit(granule, granule + CommitGranule)) return false;
    }
    return true;
}

std::vector<Run> MapMemory(const std::filesystem::path& directory, const Manifest& manifest, bool verbose) {
    std::uint64_t mapped = 0;
    std::vector<Run> leftOut;
    for (const auto& mapping : manifest.mappings) {
        auto* pointer = reinterpret_cast<void*>(mapping.begin);
        const auto bytes = static_cast<std::size_t>(mapping.end - mapping.begin);
        if (GuestArena::GuestArenaContains_nid_postfix(pointer, bytes)) {
            try {
                GuestArena::GuestArenaMarkUsed_nid_postfix(pointer, bytes);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "[replay] %s\n", error.what());
            }
        }
    }
    std::FILE* memory = std::fopen((directory / "memory.bin").string().c_str(), "rb");
    if (memory == nullptr) throw std::runtime_error("frame replay: cannot open memory.bin");
    std::uint64_t skipped = 0;
    for (const auto& run : manifest.runs) {
        const auto begin = run.begin & ~std::uint64_t{CommitGranule - 1};
        const auto end = (run.end + CommitGranule - 1) & ~std::uint64_t{CommitGranule - 1};
        if (!Commit(begin, end)) {
            std::fprintf(stderr, "[replay] guest memory 0x%llx+0x%llx cannot be mapped at its address in this process; left out\n", static_cast<unsigned long long>(run.begin), static_cast<unsigned long long>(run.end - run.begin));
            skipped += run.end - run.begin;
            leftOut.push_back(run);
            continue;
        }
#ifdef _WIN32
        if (_fseeki64(memory, static_cast<long long>(run.offset), SEEK_SET) != 0) throw std::runtime_error("frame replay: cannot seek memory.bin");
#else
        if (fseeko(memory, static_cast<off_t>(run.offset), SEEK_SET) != 0) throw std::runtime_error("frame replay: cannot seek memory.bin");
#endif
        const auto size = static_cast<std::size_t>(run.end - run.begin);
        if (std::fread(reinterpret_cast<void*>(run.begin), 1, size, memory) != size) throw std::runtime_error("frame replay: memory.bin is shorter than its manifest");
        mapped += size;
    }
    std::fclose(memory);
    for (const auto& mapping : manifest.mappings) {
        try {
            GuestAllocations::Mutation().Add(reinterpret_cast<void*>(mapping.begin), static_cast<std::size_t>(mapping.end - mapping.begin), true, true);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[replay] mapping 0x%llx+0x%llx was not registered: %s\n", static_cast<unsigned long long>(mapping.begin), static_cast<unsigned long long>(mapping.end - mapping.begin), error.what());
        }
    }
    if (verbose || skipped != 0) std::fprintf(stderr, "[replay] mapped %zu ranges, %.1f MiB (%.1f MiB left out)\n", manifest.runs.size(), static_cast<double>(mapped) / 1048576.0, static_cast<double>(skipped) / 1048576.0);
    return leftOut;
}

class HeadlessFlip final : public IFlipRequest {
public:
    void GpuReady(const std::shared_ptr<FrameTiming>&) override {}
    void Fail(std::exception_ptr) noexcept override {}
};

class HeadlessWait final : public IRenderingWait {
public:
    void Wait() override {}
};

class HeadlessOutput final : public IVideoOutput {
public:
    std::shared_ptr<IFlipRequest> Reserve(const FlipInfo&) override { return std::make_shared<HeadlessFlip>(); }
    std::shared_ptr<IRenderingWait> CaptureRenderingWait(std::uint32_t) override { return std::make_shared<HeadlessWait>(); }
    void Fail(std::exception_ptr) noexcept override {}
};

std::string AfterDirectory;
std::uint64_t AfterDraw = 0;
std::uint64_t AfterDispatch = 0;

void DumpAfterWork(bool draw, std::uint64_t index) {
    if (index != (draw ? AfterDraw : AfterDispatch)) return;
    std::fprintf(stderr, "[replay] dumping the storage images after %s %llu\n", draw ? "draw" : "dispatch", static_cast<unsigned long long>(index));
    Graphics::DumpCachedStorageImages(VK_NULL_HANDLE, AfterDirectory, {}, 0u);
}

}

void SetWorkObserver(WorkObserver observer) {
    workObserver.store(observer, std::memory_order_release);
}

bool WorkObserved() {
    return workObserver.load(std::memory_order_acquire) != nullptr;
}

void NoteWork(bool draw) {
    const auto index = (draw ? drawCount : dispatchCount).fetch_add(1, std::memory_order_acq_rel) + 1;
    if (const auto observer = workObserver.load(std::memory_order_acquire)) observer(draw, index);
}

bool Settle(std::uint32_t limitMs) {
    return DriverDetail::Driver::Get().Settle(std::chrono::milliseconds(limitMs));
}

Summary Run(const Options& options) {
    const std::filesystem::path directory(options.directory);
    const auto manifest = ReadManifest(directory);
    Summary summary;
    if (!manifest.drained) std::fprintf(stderr, "[replay] the capture did not drain the GPU before its snapshot; results may differ\n");
    if (!manifest.complete) std::fprintf(stderr, "[replay] the capture did not reach its closing flip; replaying the events it recorded\n");
    if (manifest.aliases != 0) std::fprintf(stderr, "[replay] %zu captured ranges alias each other in the title (%.1f MiB); here they are separate copies, so GPU writes through one are not seen through the other\n", manifest.aliases, static_cast<double>(manifest.aliasedBytes) / 1048576.0);
    if (options.verbose && manifest.nullPixelProgram != DriverDetail::NullPixelProgramAddress()) std::fprintf(stderr, "[replay] the null pixel program is at 0x%llx here and was at 0x%llx in the title; both are registered\n", static_cast<unsigned long long>(DriverDetail::NullPixelProgramAddress()), static_cast<unsigned long long>(manifest.nullPixelProgram));
    const auto leftOut = MapMemory(directory, manifest, options.verbose);
    auto& driver = DriverDetail::Driver::Get();
    driver.RestoreCaptureState(ReadFile(directory / "state.bin"), options.prepareShaders);
    std::vector<std::shared_ptr<IVideoOutput>> outputs;
    for (const auto handle : manifest.outputs) {
        outputs.push_back(std::make_shared<HeadlessOutput>());
        driver.RegisterVideoOutput(handle, outputs.back());
    }
    AfterDraw = options.afterDraw;
    AfterDispatch = options.afterDispatch;
    AfterDirectory = !options.afterDirectory.empty() ? options.afterDirectory : (directory / (AfterDraw != 0 ? "draw-" + std::to_string(AfterDraw) : "dispatch-" + std::to_string(AfterDispatch))).string();
    SetWorkObserver(&DumpAfterWork);

    const auto events = ReadFile(directory / "events.bin");
    DriverDetail::CaptureFormat::Reader in(events);
    if (in.U32() != DriverDetail::CaptureFormat::EventsMagic || in.U32() != DriverDetail::CaptureFormat::Version) throw std::runtime_error("frame replay: events.bin has an unsupported header");
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::vector<std::uint32_t>> commandCopies;
    while (!in.Done()) {
        auto kind = EventKind::End;
        std::uint32_t writes = 0;
        DriverDetail::CaptureFormat::Reader record(std::span<const std::byte>{});
        try {
            kind = static_cast<EventKind>(in.U32());
            writes = in.U32();
            record = DriverDetail::CaptureFormat::Reader(in.Raw(static_cast<std::size_t>(in.U64())));
        } catch (const std::exception&) {
            if (manifest.complete) throw;
            std::fprintf(stderr, "[replay] events.bin ends inside a record (the capture was interrupted); stopping there\n");
            break;
        }
        for (std::uint32_t i = 0; i < writes; ++i) {
            const auto address = record.U64();
            const auto length = record.U32();
            const auto bytes = record.Raw(length);
            if (!EnsureCommitted(address, address + length)) {
                ++summary.skippedWrites;
                continue;
            }
            std::memcpy(reinterpret_cast<void*>(address), bytes.data(), length);
            summary.cpuWriteBytes += length;
        }
        ++summary.events;
        switch (kind) {
            case EventKind::Submit: {
                const auto queue = record.U32();
                const auto address = record.U64();
                const auto words = record.U32();
                auto recorded = record.Span<std::uint32_t>();
                const auto commandEnd = address + std::uint64_t{words} * sizeof(std::uint32_t);
                auto* commands = reinterpret_cast<std::uint32_t*>(address);
                const auto unmapped = std::ranges::find_if(leftOut, [&](const auto& run) { return run.begin < commandEnd && address < run.end; });
                if (unmapped != leftOut.end()) {
                    const auto where = "event " + std::to_string(summary.events) + " submits " + std::to_string(words) + " dwords at " + Hex(address) + ", in captured memory " + Hex(unmapped->begin) + "+" + Hex(unmapped->end - unmapped->begin) + " that could not be mapped at its address in this process";
                    if (recorded.size() != words) throw std::runtime_error("frame replay: " + where + ", and its recorded words are not the whole submission");
                    std::fprintf(stderr, "[replay] %s; submitting the recorded words from a copy\n", where.c_str());
                    commandCopies.push_back(std::move(recorded));
                    commands = commandCopies.back().data();
                }
                const Packet packet{commands, words, 0, {}};
                if (options.verbose) std::fprintf(stderr, "[replay] event %llu: submit queue 0x%x 0x%llx+%u dwords\n", static_cast<unsigned long long>(summary.events), queue, static_cast<unsigned long long>(address), words);
                Submit(&packet, queue);
                ++summary.submissions;
                break;
            }
            case EventKind::Suspend:
                if (options.verbose) std::fprintf(stderr, "[replay] event %llu: suspend point\n", static_cast<unsigned long long>(summary.events));
                AgcDriverSuspendPoint_nid_postfix();
                break;
            case EventKind::Shader: {
                const auto codeAddress = record.U64();
                const auto headerAddress = record.U64();
                const auto type = record.U8();
                auto code = record.Span<std::uint32_t>();
                auto header = record.Span<std::byte>();
                if (options.verbose) std::fprintf(stderr, "[replay] event %llu: register shader 0x%llx\n", static_cast<unsigned long long>(summary.events), static_cast<unsigned long long>(codeAddress));
                try {
                    driver.RestoreShader(codeAddress, headerAddress, type, std::move(code), std::move(header), options.prepareShaders);
                } catch (const std::exception& error) {
                    std::fprintf(stderr, "[replay] shader 0x%llx was not registered: %s\n", static_cast<unsigned long long>(codeAddress), error.what());
                }
                break;
            }
            case EventKind::End:
                break;
            default:
                throw std::runtime_error("frame replay: unknown event kind " + std::to_string(static_cast<std::uint32_t>(kind)));
        }
    }
    if (summary.skippedWrites != 0) std::fprintf(stderr, "[replay] %llu recorded CPU writes fell on memory this process could not map; they were left out\n", static_cast<unsigned long long>(summary.skippedWrites));
    const auto work = [] { return drawCount.load(std::memory_order_acquire) + dispatchCount.load(std::memory_order_acquire); };
    auto progress = work();
    auto lastProgress = std::chrono::steady_clock::now();
    while (!(summary.drained = Settle(10000))) {
        if (const auto current = work(); current != progress) {
            progress = current;
            lastProgress = std::chrono::steady_clock::now();
        } else if (std::chrono::steady_clock::now() - lastProgress >= std::chrono::seconds(60)) {
            break;
        }
    }
    if (!summary.drained) std::fprintf(stderr, "[replay] the frame stopped making progress for 60 s after %llu draws and %llu dispatches (a wait on memory the capture did not record?)\n", static_cast<unsigned long long>(drawCount.load(std::memory_order_acquire)), static_cast<unsigned long long>(dispatchCount.load(std::memory_order_acquire)));
    summary.draws = drawCount.load(std::memory_order_acquire);
    summary.dispatches = dispatchCount.load(std::memory_order_acquire);
    if (options.verbose) std::fprintf(stderr, "[replay] %llu events, %llu submissions, %.1f KiB of CPU writes, %llu draws, %llu dispatches in %.1f ms\n", static_cast<unsigned long long>(summary.events), static_cast<unsigned long long>(summary.submissions), static_cast<double>(summary.cpuWriteBytes) / 1024.0, static_cast<unsigned long long>(summary.draws), static_cast<unsigned long long>(summary.dispatches), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    if (!options.imagesDirectory.empty()) Graphics::DumpCachedStorageImages(VK_NULL_HANDLE, options.imagesDirectory, options.imageAddresses, options.imagesMinimumWidth);
    for (const auto& dump : options.memory) {
        std::vector<std::byte> bytes(static_cast<std::size_t>(dump.bytes));
        GuestMemory::Read(dump.address, bytes);
        std::ofstream file(dump.path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) throw std::runtime_error("frame replay: cannot write " + dump.path);
    }
    SetWorkObserver(nullptr);
    return summary;
}

}
