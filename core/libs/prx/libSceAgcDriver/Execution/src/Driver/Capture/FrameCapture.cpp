#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/src/Driver/Capture/CaptureFormat.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libkernel/DirectMemory/DirectMemory.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace AgcDriver::DriverDetail {

namespace {

using CaptureFormat::PageBytes;
using Page = std::array<std::byte, PageBytes>;

std::uint64_t EnvNumber(const char* name, std::uint64_t fallback) {
    const char* text = std::getenv(name);
    return text != nullptr && *text != '\0' ? std::strtoull(text, nullptr, 0) : fallback;
}

bool Seek(std::FILE* file, std::uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, static_cast<long long>(offset), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

struct Mapping {
    std::uint64_t begin;
    std::uint64_t end;
    int prot;
    bool late;
};

struct Run {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t offset;
};

struct Alias {
    std::uint64_t first;
    std::uint64_t second;
    std::uint64_t bytes;
};

// Guest ranges that map the same direct memory: a CPU store through one changes the other without
// marking its pages, so the captured side is compared whenever either is written (the other side
// is walked for writes too when the capture does not include it).
std::vector<Alias> FindAliases() {
    struct Direct {
        std::uint64_t begin;
        std::uint64_t end;
        std::uint64_t backing;
        std::uint64_t offset;
    };
    std::vector<Direct> direct;
    KernelDirectMappings_nid_postfix([](void* context, std::uintptr_t begin, std::uintptr_t end, std::uint64_t backing, std::uint64_t offset) {
        if (end > begin) static_cast<std::vector<Direct>*>(context)->push_back({begin, end, backing, offset});
    }, &direct);
    std::sort(direct.begin(), direct.end(), [](const Direct& a, const Direct& b) { return a.backing != b.backing ? a.backing < b.backing : a.offset < b.offset; });
    std::vector<Alias> aliases;
    for (std::size_t i = 0; i < direct.size(); ++i) {
        const auto& a = direct[i];
        const auto aEnd = a.offset + (a.end - a.begin);
        for (std::size_t j = i + 1; j < direct.size() && direct[j].backing == a.backing && direct[j].offset < aEnd; ++j) {
            const auto& b = direct[j];
            const auto low = std::max(a.offset, b.offset);
            const auto high = std::min(aEnd, b.offset + (b.end - b.begin));
            const auto first = a.begin + (low - a.offset);
            const auto second = b.begin + (low - b.offset);
            if (high > low && first != second) aliases.push_back({first, second, high - low});
        }
    }
    return aliases;
}

class Capture {
public:
    static constexpr std::uint64_t Disarmed = ~std::uint64_t{0};
    Capture() : trigger(std::getenv("APS5_CAPTURE_TRIGGER")) {
        if (std::getenv("APS5_CAPTURE_FRAME") != nullptr) target = EnvNumber("APS5_CAPTURE_FRAME", 0);
    }

    std::mutex mutex;
    // APS5_CAPTURE_TRIGGER=<file>: arm a capture of the next frame whenever the file appears (it is
    // deleted), each into <APS5_CAPTURE_DIR or .>/frame-capture-<flips>.
    const char* const trigger;
    std::atomic<std::uint64_t> target{Disarmed};
    std::uint64_t frame = 0;
    std::atomic<std::uint64_t> flips{0};
    std::atomic<bool> active{false};
    std::atomic<bool> done{false};
    std::atomic<bool> observing{false};
    std::atomic<bool> shadowing{false};

    void Start(Driver& driver);
    void Finish(Driver& driver);
    void Abort(const char* reason);
    void Arm();
    void Event(CaptureFormat::EventKind kind, const CaptureFormat::Writer& payload);
    void AddLateRanges(std::span<const std::pair<std::uint64_t, std::size_t>> segments);

    std::mutex dirtyMutex;
    std::vector<std::uint64_t> dirty;

    std::mutex shadowMutex;
    std::vector<Mapping> mappings;
    std::vector<Run> runs;
    std::vector<Alias> aliases;
    std::vector<Mapping> partners;
    std::unordered_map<std::uint64_t, std::unique_ptr<Page>> shadow;
    std::FILE* memory = nullptr;
    std::uint64_t memoryBytes = 0;

    bool InMappings(std::uint64_t page) const {
        const auto next = std::upper_bound(mappings.begin(), mappings.end(), page, [](std::uint64_t value, const Mapping& mapping) { return value < mapping.begin; });
        return next != mappings.begin() && page < std::prev(next)->end;
    }

    Page& ShadowPage(std::uint64_t page) {
        auto& slot = shadow[page];
        if (slot != nullptr) return *slot;
        slot = std::make_unique<Page>();
        slot->fill(std::byte{0});
        for (const auto& run : runs) {
            if (run.end <= page || run.begin >= page + PageBytes) continue;
            const auto from = std::max(run.begin, page);
            const auto to = std::min(run.end, page + PageBytes);
            if (!Seek(memory, run.offset + (from - run.begin)) || std::fread(slot->data() + (from - page), 1, static_cast<std::size_t>(to - from), memory) != to - from) throw std::runtime_error("frame capture: cannot read back memory.bin");
        }
        return *slot;
    }

private:
    void appendRun(std::uint64_t begin, std::uint64_t end);
    void writeManifest(bool complete);
    void endSession();

    std::filesystem::path directory;
    std::FILE* events = nullptr;
    bool drained = false;
    std::vector<std::uint32_t> outputs;
    std::uint64_t eventCount = 0;
    std::uint64_t submissions = 0;
    std::uint64_t writeBytes = 0;
    std::uint64_t writeRuns = 0;
    std::uint64_t comparedPages = 0;
    std::uint64_t unwatchedBytes = 0;
    std::chrono::steady_clock::time_point started;
};

Capture& State() {
    static auto* capture = new Capture();
    return *capture;
}

void ObserveDirty(std::uint64_t address, std::size_t bytes) {
    auto& capture = State();
    if (!capture.observing.load(std::memory_order_acquire) || bytes == 0) return;
    std::lock_guard lock(capture.dirtyMutex);
    for (auto page = address & ~(PageBytes - 1); page < address + bytes; page += PageBytes) capture.dirty.push_back(page);
}

void ObserveStore(std::uint64_t address, std::size_t bytes) {
    auto& capture = State();
    if (!capture.shadowing.load(std::memory_order_acquire) || bytes == 0) return;
    std::lock_guard lock(capture.shadowMutex);
    for (auto page = address & ~(PageBytes - 1); page < address + bytes; page += PageBytes) {
        if (!capture.InMappings(page)) continue;
        auto& shadow = capture.ShadowPage(page);
        const auto from = std::max(page, address);
        const auto to = std::min(page + PageBytes, address + bytes);
        std::memcpy(shadow.data() + (from - page), reinterpret_cast<const void*>(from), static_cast<std::size_t>(to - from));
    }
}

void Capture::appendRun(std::uint64_t begin, std::uint64_t end) {
    constexpr std::uint64_t Chunk = 4u << 20u;
    std::vector<std::byte> buffer(static_cast<std::size_t>(std::min(Chunk, end - begin)));
    if (std::fseek(memory, 0, SEEK_END) != 0) throw std::runtime_error("frame capture: cannot seek memory.bin");
    for (auto cursor = begin; cursor < end;) {
        const auto size = static_cast<std::size_t>(std::min(Chunk, end - cursor));
        const auto bytes = std::span(buffer).first(size);
        if (GuestMemory::CopyMapped(cursor, bytes) != GuestMemory::Compare::Equal) {
            std::fill(bytes.begin(), bytes.end(), std::byte{0});
            std::fprintf(stderr, "[capture] 0x%llx+0x%zx became unreadable during the snapshot; stored as zeros\n", static_cast<unsigned long long>(cursor), size);
        }
        if (std::fwrite(bytes.data(), 1, size, memory) != size) throw std::runtime_error("frame capture: cannot write memory.bin (disk full?)");
        cursor += size;
    }
    runs.push_back({begin, end, memoryBytes});
    memoryBytes += end - begin;
}

void Capture::Arm() {
    if (trigger == nullptr || active.load(std::memory_order_acquire) || target.load(std::memory_order_acquire) != Disarmed) return;
    std::error_code error;
    if (!std::filesystem::exists(trigger, error)) return;
    std::filesystem::remove(trigger, error);
    target.store(flips.load(std::memory_order_acquire), std::memory_order_release);
    std::fprintf(stderr, "[capture] %s appeared: capturing the next frame\n", trigger);
}

void Capture::Start(Driver& driver) {
    started = std::chrono::steady_clock::now();
    frame = flips.load(std::memory_order_acquire);
    const char* configured = std::getenv("APS5_CAPTURE_DIR");
    const bool named = configured != nullptr && *configured != '\0';
    if (trigger != nullptr) directory = (named ? std::filesystem::path(configured) : std::filesystem::path(".")) / ("frame-capture-" + std::to_string(frame));
    else directory = named ? std::filesystem::path(configured) : std::filesystem::path("frame-capture-" + std::to_string(frame));
    std::filesystem::create_directories(directory);
    std::fprintf(stderr, "[capture] frame %llu: draining the GPU, then writing %s\n", static_cast<unsigned long long>(frame), directory.string().c_str());
    drained = driver.Settle(std::chrono::milliseconds(EnvNumber("APS5_CAPTURE_DRAIN_MS", 10000)));
    if (!drained) std::fprintf(stderr, "[capture] the GPU did not drain in time; capturing with work in flight\n");

    std::vector<Mapping> found;
    if (EnvNumber("APS5_CAPTURE_ALL_MEMORY", 0) != 0) {
        for (const auto& range : GuestAllocations::GuestAllocationsAcquire_nid_postfix()) {
            if (range->bytes != 0) found.push_back({range->address, range->address + range->bytes, (range->readable ? 1 : 0) | (range->writable ? 2 : 0), false});
        }
    } else {
        KernelProtectedRanges_nid_postfix([](void* context, std::uintptr_t begin, std::uintptr_t end, int prot) {
            if ((prot & 0x30) != 0 && end > begin) static_cast<std::vector<Mapping>*>(context)->push_back({begin, end, prot, false});
        }, &found);
    }
    for (auto& mapping : found) {
        mapping.begin &= ~(PageBytes - 1);
        mapping.end = (mapping.end + PageBytes - 1) & ~(PageBytes - 1);
    }
    std::sort(found.begin(), found.end(), [](const Mapping& a, const Mapping& b) { return a.begin < b.begin; });
    for (const auto& mapping : found) {
        if (!mappings.empty() && mapping.begin <= mappings.back().end) {
            mappings.back().end = std::max(mappings.back().end, mapping.end);
            mappings.back().prot |= mapping.prot;
        } else {
            mappings.push_back(mapping);
        }
    }

    for (auto alias : FindAliases()) {
        const bool first = InMappings(alias.first);
        const bool second = InMappings(alias.second);
        if (!first && !second) continue;
        if (!first) std::swap(alias.first, alias.second);
        if (!first || !second) partners.push_back({alias.second, alias.second + alias.bytes, 0, false});
        aliases.push_back(alias);
    }
    if (!aliases.empty()) {
        std::uint64_t aliased = 0;
        for (const auto& alias : aliases) aliased += alias.bytes;
        std::fprintf(stderr, "[capture] %zu captured ranges alias direct memory mapped elsewhere (%.1f MiB); the replay keeps them as separate copies\n", aliases.size(), static_cast<double>(aliased) / 1048576.0);
    }

    GuestMemory::SetCaptureObservers(&ObserveDirty, &ObserveStore);
    observing.store(true, std::memory_order_release);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> unwatched;
    for (const auto& mapping : mappings) GuestMemory::CollectForCapture(mapping.begin, static_cast<std::size_t>(mapping.end - mapping.begin), unwatched);
    {
        std::lock_guard lock(dirtyMutex);
        dirty.clear();
    }
    for (const auto& [begin, end] : unwatched) unwatchedBytes += end - begin;

    memory = std::fopen((directory / "memory.bin").string().c_str(), "w+b");
    if (memory == nullptr) throw std::runtime_error("frame capture: cannot create memory.bin");
    for (const auto& mapping : mappings) {
        for (const auto& [begin, end] : GuestMemory::CommittedRanges(mapping.begin, static_cast<std::size_t>(mapping.end - mapping.begin))) appendRun(begin & ~(PageBytes - 1), (end + PageBytes - 1) & ~(PageBytes - 1));
    }
    if (std::fflush(memory) != 0) throw std::runtime_error("frame capture: cannot write memory.bin");

    const auto state = driver.SaveCaptureState();
    if (auto* file = std::fopen((directory / "state.bin").string().c_str(), "wb")) {
        const bool written = std::fwrite(state.data(), 1, state.size(), file) == state.size();
        std::fclose(file);
        if (!written) throw std::runtime_error("frame capture: cannot write state.bin");
    } else {
        throw std::runtime_error("frame capture: cannot create state.bin");
    }
    outputs = driver.VideoOutputHandles();

    events = std::fopen((directory / "events.bin").string().c_str(), "wb");
    if (events == nullptr) throw std::runtime_error("frame capture: cannot create events.bin");
    CaptureFormat::Writer header;
    header.U32(CaptureFormat::EventsMagic);
    header.U32(CaptureFormat::Version);
    if (std::fwrite(header.bytes.data(), 1, header.bytes.size(), events) != header.bytes.size()) throw std::runtime_error("frame capture: cannot write events.bin");

    writeManifest(false);
    shadowing.store(true, std::memory_order_release);
    active.store(true, std::memory_order_release);
    std::fprintf(stderr, "[capture] snapshot: %zu mappings, %zu committed runs, %.1f MiB (%.1f MiB unwatched) in %.1f s\n", mappings.size(), runs.size(), static_cast<double>(memoryBytes) / 1048576.0, static_cast<double>(unwatchedBytes) / 1048576.0, std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

void Capture::AddLateRanges(std::span<const std::pair<std::uint64_t, std::size_t>> segments) {
    bool added = false;
    std::lock_guard lock(shadowMutex);
    for (const auto& [address, bytes] : segments) {
        if (bytes == 0) continue;
        const auto begin = address & ~(PageBytes - 1);
        const auto end = (address + bytes + PageBytes - 1) & ~(PageBytes - 1);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> missing;
        for (auto page = begin; page < end; page += PageBytes) {
            if (InMappings(page)) continue;
            if (!missing.empty() && missing.back().second == page) missing.back().second += PageBytes;
            else missing.emplace_back(page, page + PageBytes);
        }
        if (!missing.empty()) added = true;
        for (const auto& [from, to] : missing) {
            std::fprintf(stderr, "[capture] command memory 0x%llx+0x%llx is outside the captured mappings; adding it\n", static_cast<unsigned long long>(from), static_cast<unsigned long long>(to - from));
            mappings.push_back({from, to, 0, true});
            std::sort(mappings.begin(), mappings.end(), [](const Mapping& a, const Mapping& b) { return a.begin < b.begin; });
            for (const auto& [runBegin, runEnd] : GuestMemory::CommittedRanges(from, static_cast<std::size_t>(to - from))) appendRun(runBegin & ~(PageBytes - 1), (runEnd + PageBytes - 1) & ~(PageBytes - 1));
        }
    }
    std::fflush(memory);
    if (added) writeManifest(false);
}

void Capture::Event(CaptureFormat::EventKind kind, const CaptureFormat::Writer& payload) {
    std::vector<Mapping> walk;
    {
        std::lock_guard lock(shadowMutex);
        walk = mappings;
    }
    walk.insert(walk.end(), partners.begin(), partners.end());
    std::vector<std::pair<std::uint64_t, std::uint64_t>> unwatched;
    for (const auto& mapping : walk) GuestMemory::CollectForCapture(mapping.begin, static_cast<std::size_t>(mapping.end - mapping.begin), unwatched);
    std::vector<std::uint64_t> pages;
    {
        std::lock_guard lock(dirtyMutex);
        pages.swap(dirty);
    }
    for (const auto& [begin, end] : unwatched) {
        for (auto page = begin; page < end; page += PageBytes) pages.push_back(page);
    }
    if (!aliases.empty()) {
        const auto written = pages.size();
        for (std::size_t i = 0; i < written; ++i) {
            const auto page = pages[i];
            for (const auto& alias : aliases) {
                if (page >= alias.first && page - alias.first < alias.bytes) pages.push_back(alias.second + (page - alias.first));
                if (page >= alias.second && page - alias.second < alias.bytes) pages.push_back(alias.first + (page - alias.second));
            }
        }
    }
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());

    CaptureFormat::Writer writes;
    std::uint32_t count = 0;
    {
        std::lock_guard lock(shadowMutex);
        Page current;
        for (const auto page : pages) {
            if (!InMappings(page)) continue;
            if (GuestMemory::CopyMapped(page, current) != GuestMemory::Compare::Equal) continue;
            ++comparedPages;
            auto& old = ShadowPage(page);
            for (std::size_t at = 0; at < PageBytes;) {
                if (current[at] == old[at]) {
                    ++at;
                    continue;
                }
                auto last = at;
                for (auto probe = at + 1; probe < PageBytes && probe - last <= 16; ++probe) {
                    if (current[probe] != old[probe]) last = probe;
                }
                const auto length = static_cast<std::uint32_t>(last + 1 - at);
                writes.U64(page + at);
                writes.U32(length);
                writes.Raw(current.data() + at, length);
                ++count;
                writeBytes += length;
                at = last + 1;
            }
            old = current;
        }
    }
    writeRuns += count;
    CaptureFormat::Writer record;
    record.U32(static_cast<std::uint32_t>(kind));
    record.U32(count);
    record.U64(writes.bytes.size() + payload.bytes.size());
    if (std::fwrite(record.bytes.data(), 1, record.bytes.size(), events) != record.bytes.size() || std::fwrite(writes.bytes.data(), 1, writes.bytes.size(), events) != writes.bytes.size() || std::fwrite(payload.bytes.data(), 1, payload.bytes.size(), events) != payload.bytes.size()) throw std::runtime_error("frame capture: cannot write events.bin (disk full?)");
    std::fflush(events);
    ++eventCount;
    if (kind == CaptureFormat::EventKind::Submit) ++submissions;
}

void Capture::writeManifest(bool complete) {
    auto* file = std::fopen((directory / "manifest.txt").string().c_str(), "w");
    if (file == nullptr) throw std::runtime_error("frame capture: cannot create manifest.txt");
    std::fprintf(file, "aps5-frame-capture %u\n", CaptureFormat::Version);
    std::fprintf(file, "frame %llu\n", static_cast<unsigned long long>(frame));
    std::fprintf(file, "drained %d\n", drained ? 1 : 0);
    std::fprintf(file, "null-pixel-program 0x%llx\n", static_cast<unsigned long long>(NullPixelProgramAddress()));
    std::fprintf(file, "events %llu\n", static_cast<unsigned long long>(eventCount));
    std::fprintf(file, "submissions %llu\n", static_cast<unsigned long long>(submissions));
    std::fprintf(file, "cpu-write-bytes %llu\n", static_cast<unsigned long long>(writeBytes));
    std::fprintf(file, "memory-bytes %llu\n", static_cast<unsigned long long>(memoryBytes));
    for (const auto handle : outputs) std::fprintf(file, "output %u\n", handle);
    for (const auto& mapping : mappings) std::fprintf(file, "mapping 0x%llx 0x%llx 0x%x %d\n", static_cast<unsigned long long>(mapping.begin), static_cast<unsigned long long>(mapping.end), static_cast<unsigned>(mapping.prot), mapping.late ? 1 : 0);
    for (const auto& run : runs) std::fprintf(file, "run 0x%llx 0x%llx 0x%llx\n", static_cast<unsigned long long>(run.begin), static_cast<unsigned long long>(run.end), static_cast<unsigned long long>(run.offset));
    for (const auto& alias : aliases) std::fprintf(file, "alias 0x%llx 0x%llx 0x%llx\n", static_cast<unsigned long long>(alias.first), static_cast<unsigned long long>(alias.second), static_cast<unsigned long long>(alias.bytes));
    std::fprintf(file, "complete %d\n", complete ? 1 : 0);
    std::fclose(file);
}

void Capture::Finish(Driver& driver) {
    Event(CaptureFormat::EventKind::End, {});
    shadowing.store(false, std::memory_order_release);
    observing.store(false, std::memory_order_release);
    GuestMemory::SetCaptureObservers(nullptr, nullptr);
    std::fclose(events);
    events = nullptr;
    {
        std::lock_guard lock(shadowMutex);
        std::fclose(memory);
        memory = nullptr;
        shadow.clear();
    }
    writeManifest(true);
    std::fprintf(stderr, "[capture] frame %llu written to %s: %llu events (%llu submissions), %llu CPU write runs (%.1f MiB) over %llu compared pages, %.1f s\n", static_cast<unsigned long long>(frame), directory.string().c_str(), static_cast<unsigned long long>(eventCount), static_cast<unsigned long long>(submissions), static_cast<unsigned long long>(writeRuns), static_cast<double>(writeBytes) / 1048576.0, static_cast<unsigned long long>(comparedPages), std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    if (const char* reference = std::getenv("APS5_CAPTURE_REFERENCE"); reference != nullptr && *reference != '\0') {
        if (!driver.Settle(std::chrono::milliseconds(EnvNumber("APS5_CAPTURE_DRAIN_MS", 10000)))) std::fprintf(stderr, "[capture] the frame did not finish in time; the reference images may be incomplete\n");
        Graphics::DumpCachedStorageImages(VK_NULL_HANDLE, (directory / "reference").string(), {}, static_cast<std::uint32_t>(std::strtoul(reference, nullptr, 0)));
    }
    endSession();
}

void Capture::Abort(const char* reason) {
    std::fprintf(stderr, "[capture] frame %llu abandoned: %s\n", static_cast<unsigned long long>(frame), reason);
    shadowing.store(false, std::memory_order_release);
    observing.store(false, std::memory_order_release);
    GuestMemory::SetCaptureObservers(nullptr, nullptr);
    if (events != nullptr) std::fclose(events);
    events = nullptr;
    {
        std::lock_guard lock(shadowMutex);
        if (memory != nullptr) std::fclose(memory);
        memory = nullptr;
    }
    endSession();
}

void Capture::endSession() {
    {
        std::lock_guard lock(dirtyMutex);
        dirty.clear();
    }
    std::lock_guard lock(shadowMutex);
    mappings.clear();
    runs.clear();
    aliases.clear();
    partners.clear();
    shadow.clear();
    memoryBytes = 0;
    outputs.clear();
    eventCount = 0;
    submissions = 0;
    writeBytes = 0;
    writeRuns = 0;
    comparedPages = 0;
    unwatchedBytes = 0;
    active.store(false, std::memory_order_release);
    if (trigger != nullptr) target.store(Disarmed, std::memory_order_release);
    else done.store(true, std::memory_order_release);
}

template <typename TRecord>
void Guarded(TRecord&& record) {
    auto& capture = State();
    try {
        record(capture);
    } catch (const std::exception& error) {
        capture.Abort(error.what());
    }
}

}

namespace FrameCapture {

bool Enabled() {
    static const bool enabled = std::getenv("APS5_CAPTURE_FRAME") != nullptr || std::getenv("APS5_CAPTURE_TRIGGER") != nullptr;
    return enabled;
}

std::unique_lock<std::mutex> Enter(Driver& driver) {
    if (!Enabled()) return {};
    auto& capture = State();
    if (capture.done.load(std::memory_order_acquire)) return {};
    if (!capture.active.load(std::memory_order_acquire) && capture.flips.load(std::memory_order_acquire) < capture.target.load(std::memory_order_acquire)) return {};
    std::unique_lock lock(capture.mutex);
    if (capture.done.load(std::memory_order_acquire)) return {};
    if (!capture.active.load(std::memory_order_acquire) && capture.flips.load(std::memory_order_acquire) < capture.target.load(std::memory_order_acquire)) return {};
    if (!capture.active.load(std::memory_order_acquire)) {
        try {
            capture.Start(driver);
        } catch (const std::exception& error) {
            capture.Abort(error.what());
            return {};
        }
    }
    return lock;
}

std::vector<std::pair<std::uint64_t, std::size_t>>*& Segments() {
    static thread_local std::vector<std::pair<std::uint64_t, std::size_t>>* segments = nullptr;
    return segments;
}

void RecordSubmit(std::uint32_t queue, std::uint64_t address, std::uint32_t words, std::span<const std::uint32_t> commands, std::span<const std::pair<std::uint64_t, std::size_t>> segments) {
    Guarded([&](Capture& capture) {
        if (!capture.active.load(std::memory_order_acquire)) return;
        capture.AddLateRanges(segments);
        CaptureFormat::Writer payload;
        payload.U32(queue);
        payload.U64(address);
        payload.U32(words);
        payload.Span(commands);
        capture.Event(CaptureFormat::EventKind::Submit, payload);
    });
}

void RecordSuspend() {
    Guarded([&](Capture& capture) {
        if (capture.active.load(std::memory_order_acquire)) capture.Event(CaptureFormat::EventKind::Suspend, {});
    });
}

void RecordShader(std::uint64_t codeAddress, std::uint64_t headerAddress, std::uint8_t type, std::span<const std::uint32_t> code, std::span<const std::byte> header) {
    Guarded([&](Capture& capture) {
        if (!capture.active.load(std::memory_order_acquire)) return;
        CaptureFormat::Writer payload;
        payload.U64(codeAddress);
        payload.U64(headerAddress);
        payload.U8(type);
        payload.Span(code);
        payload.Span(header);
        capture.Event(CaptureFormat::EventKind::Shader, payload);
    });
}

void Submitted(Driver& driver, bool flipped, std::unique_lock<std::mutex>& entered) {
    auto& capture = State();
    if (flipped) {
        capture.flips.fetch_add(1, std::memory_order_acq_rel);
        capture.Arm();
    }
    if (!flipped || !entered.owns_lock() || !capture.active.load(std::memory_order_acquire)) return;
    Guarded([&](Capture& state) { state.Finish(driver); });
}

}

bool Driver::Settle(std::chrono::milliseconds limit) {
    require(!onWorkerThread(), "worker cannot settle the GPU");
    bool drained = false;
    {
        std::unique_lock lock(mutex);
        const auto target = accepted;
        ++idleWaiters;
        drained = changed.wait_for(lock, limit, [&] { return failure != nullptr || stopping || completed >= target; });
        --idleWaiters;
        rethrowFailure();
        checkStopping();
    }
    std::lock_guard gpuLock(GuestMemory::GpuMutex());
    if (const auto localDevice = device.Load()) {
        localDevice->WaitIdle();
        Graphics::StorageTexture::FlushAllPending("frame capture");
        localDevice->WaitIdle();
    }
    return drained;
}

std::vector<std::byte> Driver::SaveCaptureState() {
    CaptureFormat::Writer out;
    out.U32(CaptureFormat::Version);
    std::shared_ptr<const ShaderRegistry> registry;
    {
        std::lock_guard lock(mutex);
        out.U32(static_cast<std::uint32_t>(queues.size()));
        for (const auto& [id, queue] : queues) {
            out.U32(id);
            CaptureFormat::WriteQueue(out, queue);
        }
        out.U8(resetGraphics ? 1 : 0);
        registry = shaders;
    }
    out.U32(registry != nullptr ? static_cast<std::uint32_t>(registry->size()) : 0u);
    if (registry != nullptr) {
        for (const auto& [address, snapshot] : *registry) {
            out.U64(snapshot->codeAddress);
            out.U64(snapshot->headerAddress);
            out.U8(snapshot->type);
            out.Span(std::span<const std::uint32_t>(snapshot->code));
            out.Span(std::span<const std::byte>(snapshot->header));
        }
    }
    return std::move(out.bytes);
}

std::vector<std::uint32_t> Driver::VideoOutputHandles() {
    std::lock_guard lock(mutex);
    std::vector<std::uint32_t> handles;
    for (const auto& [handle, output] : outputs) handles.push_back(handle);
    return handles;
}

}
