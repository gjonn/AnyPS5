#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_FRAMECAPTURE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_FRAMECAPTURE_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Whole-frame capture and replay (profiling/FRAME-REPLAY.md).
namespace AgcDriver::DriverDetail {

class Driver;

namespace FrameCapture {

// APS5_CAPTURE_FRAME=<n>: capture the frame after the n-th flip submission (0: the first frame).
bool Enabled();
// Taken by every recorded driver entry point (Submit, SuspendPoint, RegisterShader) for the whole
// call: owns the capture mutex while a capture runs (starting it when its frame begins), so the
// calls are recorded in the order the driver accepts them; empty otherwise.
std::unique_lock<std::mutex> Enter(Driver& driver);
// While a recorded Submit copies its command stream: every guest segment the copy reads.
std::vector<std::pair<std::uint64_t, std::size_t>>*& Segments();
void RecordSubmit(std::uint32_t queue, std::uint64_t address, std::uint32_t words, std::span<const std::uint32_t> commands, std::span<const std::pair<std::uint64_t, std::size_t>> segments);
void RecordSuspend();
void RecordShader(std::uint64_t codeAddress, std::uint64_t headerAddress, std::uint8_t type, std::span<const std::uint32_t> code, std::span<const std::byte> header);
// After a Submit was accepted: counts flips and ends a running capture at its flip.
void Submitted(Driver& driver, bool flipped, std::unique_lock<std::mutex>& entered);

}

}

namespace AgcDriver::FrameReplay {

struct MemoryDump {
    std::uint64_t address = 0;
    std::uint64_t bytes = 0;
    std::string path;
};

struct Options {
    std::string directory;
    std::string imagesDirectory;
    std::vector<std::uint64_t> imageAddresses;
    std::uint32_t imagesMinimumWidth = 0;
    std::vector<MemoryDump> memory;
    // Dump the cached storage images right after the n-th draw or dispatch packet (1-based, in the
    // order the queue workers executed them) into afterDirectory (default <capture>/draw-<n> or
    // dispatch-<n>).
    std::uint64_t afterDraw = 0;
    std::uint64_t afterDispatch = 0;
    std::string afterDirectory;
    // Registered shaders are prepared at registration like in the title; by default the replay
    // leaves them to be prepared at first use, which compiles only what the frame draws.
    bool prepareShaders = false;
    bool verbose = false;
};

struct Summary {
    std::uint64_t events = 0;
    std::uint64_t submissions = 0;
    std::uint64_t cpuWriteBytes = 0;
    std::uint64_t skippedWrites = 0;
    std::uint64_t draws = 0;
    std::uint64_t dispatches = 0;
    bool drained = false;
};

// Maps the capture's memory, restores the driver state and plays the events through the driver
// (one call per process: the driver is a process singleton). Throws on a malformed capture.
Summary Run(const Options& options);
// Waits up to `limitMs` for every accepted submission, then drains the device and stores every
// pending storage image into guest memory; false when the submissions did not finish in time.
bool Settle(std::uint32_t limitMs);
// Called after every draw or dispatch packet a queue worker executed, with the packet's 1-based
// count of its kind in the process.
using WorkObserver = void (*)(bool draw, std::uint64_t index);
void SetWorkObserver(WorkObserver observer);
bool WorkObserved();
void NoteWork(bool draw);

}

#endif
