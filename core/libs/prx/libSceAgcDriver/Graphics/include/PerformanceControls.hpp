#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PERFORMANCECONTROLS_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PERFORMANCECONTROLS_HPP

#include <atomic>
#include <cstdint>
#include <string_view>

namespace AgcDriver::Graphics {

class PerformanceControls {
public:
    enum Flag : std::uint32_t { MergeDrawBarriers = 1, BdaTableDeviceLocal = 2, InPlaceDrawInputs = 4 };
    explicit PerformanceControls(std::uint32_t initial) : flags(initial) {}
    std::uint32_t Get() const { return flags.load(std::memory_order_relaxed); }
    void Set(std::uint32_t value) { flags.store(value, std::memory_order_relaxed); }

    // A partial edit must never change live behavior: both fields are required,
    // unknown/duplicate fields are rejected, and the entire update is atomic.
    bool Apply(std::string_view text) {
        if (text.size() > 256) return false;
        std::uint32_t seen = 0, next = Get() & InPlaceDrawInputs;
        while (!text.empty()) {
            const auto end = text.find('\n');
            auto line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty()) continue;
            const auto split = line.find('=');
            if (split == std::string_view::npos) return false;
            const auto key = line.substr(0, split), value = line.substr(split + 1);
            const std::uint32_t bit = key == "merge_draw_barriers" ? MergeDrawBarriers :
                key == "bda_table_device_local" ? BdaTableDeviceLocal :
                key == "in_place_draw_inputs" ? InPlaceDrawInputs : 0;
            if (bit == 0 || (seen & bit) != 0 || (value != "0" && value != "1")) return false;
            seen |= bit;
            next &= ~bit;
            if (value == "1") next |= bit;
        }
        if ((seen & (MergeDrawBarriers | BdaTableDeviceLocal)) != (MergeDrawBarriers | BdaTableDeviceLocal)) return false;
        Set(next);
        return true;
    }

private:
    std::atomic<std::uint32_t> flags;
};

PerformanceControls& LivePerformanceControls();

}

#endif
