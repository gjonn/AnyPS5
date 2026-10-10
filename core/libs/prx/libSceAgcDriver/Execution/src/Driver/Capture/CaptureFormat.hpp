#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_SRC_DRIVER_CAPTURE_CAPTUREFORMAT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_SRC_DRIVER_CAPTURE_CAPTUREFORMAT_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// The files of a frame capture (profiling/FRAME-REPLAY.md). Integers are little-endian.
namespace AgcDriver::DriverDetail::CaptureFormat {

constexpr std::uint32_t Version = 1;
constexpr std::uint32_t EventsMagic = 0x56454641u;
constexpr std::uint64_t PageBytes = 4096;

enum class EventKind : std::uint32_t { Submit = 1, Suspend = 2, Shader = 3, End = 4 };

class Writer {
public:
    void U8(std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
    void U32(std::uint32_t value) { Raw(&value, sizeof(value)); }
    void U64(std::uint64_t value) { Raw(&value, sizeof(value)); }
    void Raw(const void* data, std::size_t size) {
        const auto* first = static_cast<const std::byte*>(data);
        bytes.insert(bytes.end(), first, first + size);
    }
    template <typename T>
    void Span(std::span<const T> values) {
        U32(static_cast<std::uint32_t>(values.size()));
        Raw(values.data(), values.size_bytes());
    }
    std::vector<std::byte> bytes;
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes(bytes) {}
    std::uint8_t U8() { return static_cast<std::uint8_t>(Raw(1)[0]); }
    std::uint32_t U32() { return Value<std::uint32_t>(); }
    std::uint64_t U64() { return Value<std::uint64_t>(); }
    std::span<const std::byte> Raw(std::size_t size) {
        if (size > bytes.size() - offset) throw std::runtime_error("frame capture: truncated record");
        const auto result = bytes.subspan(offset, size);
        offset += size;
        return result;
    }
    template <typename T>
    std::vector<T> Span() {
        const auto count = U32();
        const auto raw = Raw(static_cast<std::size_t>(count) * sizeof(T));
        std::vector<T> values(count);
        std::memcpy(values.data(), raw.data(), raw.size());
        return values;
    }
    bool Done() const { return offset == bytes.size(); }

private:
    template <typename T>
    T Value() {
        T value;
        std::memcpy(&value, Raw(sizeof(T)).data(), sizeof(T));
        return value;
    }
    std::span<const std::byte> bytes;
    std::size_t offset = 0;
};

inline void WriteRegisters(Writer& out, const Registers& registers) {
    out.U32(static_cast<std::uint32_t>(registers.size()));
    for (const auto [offset, value] : registers) {
        out.U32(offset);
        out.U32(value);
    }
}

inline Registers ReadRegisters(Reader& in) {
    Registers registers;
    const auto count = in.U32();
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto offset = in.U32();
        registers.insert_or_assign(offset, in.U32());
    }
    return registers;
}

inline void WriteQueue(Writer& out, const QueueState& queue) {
    WriteRegisters(out, queue.shader);
    WriteRegisters(out, queue.context);
    WriteRegisters(out, queue.userConfig);
    out.U8(queue.savedContext.has_value() ? 1 : 0);
    if (queue.savedContext.has_value()) WriteRegisters(out, *queue.savedContext);
    out.Raw(queue.constantRam.data(), sizeof(queue.constantRam));
    out.U64(queue.indexBase);
    out.U64(queue.drawIndirectBase);
    out.U64(queue.dispatchIndirectBase);
    out.U32(queue.indexBufferSize);
    out.U32(queue.indexType);
    out.U32(queue.instanceCount);
    out.U64(queue.predication.address);
    out.U32(queue.predication.operation);
    out.U8(queue.predication.executeWhenSet ? 1 : 0);
}

inline QueueState ReadQueue(Reader& in) {
    QueueState queue;
    queue.shader = ReadRegisters(in);
    queue.context = ReadRegisters(in);
    queue.userConfig = ReadRegisters(in);
    if (in.U8() != 0) queue.savedContext = ReadRegisters(in);
    std::memcpy(queue.constantRam.data(), in.Raw(sizeof(queue.constantRam)).data(), sizeof(queue.constantRam));
    queue.indexBase = in.U64();
    queue.drawIndirectBase = in.U64();
    queue.dispatchIndirectBase = in.U64();
    queue.indexBufferSize = in.U32();
    queue.indexType = in.U32();
    queue.instanceCount = in.U32();
    queue.predication.address = in.U64();
    queue.predication.operation = in.U32();
    queue.predication.executeWhenSet = in.U8() != 0;
    return queue;
}

}

#endif
