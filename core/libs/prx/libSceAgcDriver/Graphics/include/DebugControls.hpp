#ifndef AGCDRIVER_GRAPHICS_DEBUGCONTROLS_HPP
#define AGCDRIVER_GRAPHICS_DEBUGCONTROLS_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

namespace AgcDriver::Graphics {

enum class DebugKey : std::uint32_t {
    NoClearedView,
    TraceTarget,
    TraceTargetNoDepth,
    TraceTargetDumpAt,
    TraceSampled,
    TraceDepth,
    DumpMinWidth,
    TraceStorage,
    Packed111110,
    TraceTargetSpan,
    SplitCubeStorage,
    DumpDepth,
    DumpExtra,
    ClearSampledDepth,
    SplitDepthSlices,
    ClearDepthPerFrame,
    Count
};

struct DebugKeyInfo {
    const char* name;
    const char* environment;
    bool hex;
};

inline constexpr std::array<DebugKeyInfo, static_cast<std::size_t>(DebugKey::Count)> kDebugKeys{{
    {"no_cleared_view", "APS5_NO_CLEARED_VIEW", false},
    {"trace_target", "APS5_TRACE_TARGET", true},
    {"trace_target_no_depth", "APS5_TRACE_TARGET_NO_DEPTH", false},
    {"trace_target_dump_at", "APS5_TRACE_TARGET_DUMP_AT", false},
    {"trace_sampled", "APS5_TRACE_SAMPLED", true},
    {"trace_depth", "APS5_TRACE_DEPTH", true},
    {"dump_min_width", "APS5_DUMP_MIN_WIDTH", false},
    {"trace_storage", "APS5_TRACE_STORAGE", true},
    {"packed_11_11_10", "APS5_PACKED_11_11_10", false},
    {"trace_target_span", "APS5_TRACE_TARGET_SPAN", true},
    {"split_cube_storage", "APS5_SPLIT_CUBE_STORAGE", false},
    {"dump_depth", "APS5_DUMP_DEPTH", true},
    {"dump_extra", "APS5_DUMP_EXTRA", true},
    {"clear_sampled_depth", "APS5_CLEAR_SAMPLED_DEPTH", true},
    {"split_depth_slices", "APS5_SPLIT_DEPTH_SLICES", false},
    {"clear_depth_per_frame", "APS5_CLEAR_DEPTH_PER_FRAME", true},
}};

struct DebugControlState {
    std::array<std::atomic<std::uint64_t>, static_cast<std::size_t>(DebugKey::Count)> values{};
    std::mutex mutex;
    std::string dumpLabel;
    std::string lastText;
    std::chrono::steady_clock::time_point nextPoll{};

    DebugControlState() {
        for (std::size_t i = 0; i < kDebugKeys.size(); ++i) {
            const char* text = std::getenv(kDebugKeys[i].environment);
            std::uint64_t value = i == static_cast<std::size_t>(DebugKey::ClearDepthPerFrame) ? 0x1210c10000ull : 0;
            if (text != nullptr) value = *text == '\0' ? 1ull : std::strtoull(text, nullptr, kDebugKeys[i].hex ? 16 : 0);
            if (text != nullptr && value == 0 && !kDebugKeys[i].hex && std::string_view(text) != "0") value = 1;
            values[i].store(value, std::memory_order_relaxed);
        }
    }
};

inline DebugControlState& DebugControls() {
    static DebugControlState state;
    return state;
}

inline std::uint64_t DebugValue(DebugKey key) {
    return DebugControls().values[static_cast<std::size_t>(key)].load(std::memory_order_relaxed);
}

inline void PollDebugControls() {
    static const char* path = std::getenv("APS5_DEBUG_CONTROL");
    if (path == nullptr) return;
    auto& state = DebugControls();
    std::unique_lock lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < state.nextPoll) return;
    state.nextPoll = now + std::chrono::seconds(1);
    std::ifstream input(path, std::ios::binary);
    if (!input) return;
    std::ostringstream buffer;
    buffer << input.rdbuf();
    const auto text = buffer.str();
    if (text == state.lastText) return;
    state.lastText = text;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        const auto name = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        if (name == "dump") {
            state.dumpLabel = value;
            std::fprintf(stderr, "[debug-control] dump requested: %s\n", value.c_str());
            continue;
        }
        for (std::size_t i = 0; i < kDebugKeys.size(); ++i) {
            if (name != kDebugKeys[i].name) continue;
            const auto parsed = std::strtoull(value.c_str(), nullptr, kDebugKeys[i].hex ? 16 : 0);
            if (state.values[i].exchange(parsed, std::memory_order_relaxed) != parsed) std::fprintf(stderr, "[debug-control] %s=%s\n", kDebugKeys[i].name, value.c_str());
        }
    }
}

inline std::uint64_t TakeDebugValue(DebugKey key) {
    return DebugControls().values[static_cast<std::size_t>(key)].exchange(0, std::memory_order_relaxed);
}

inline bool TakeDumpRequest(std::string& label) {
    auto& state = DebugControls();
    std::lock_guard lock(state.mutex);
    if (state.dumpLabel.empty()) return false;
    label = std::move(state.dumpLabel);
    state.dumpLabel.clear();
    return true;
}

}

#endif
