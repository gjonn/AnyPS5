// Captures a synthetic two-queue frame with APS5_CAPTURE_FRAME, replays it with agc_frame_replay
// (the path is the first argument) and requires the replay to produce the same bytes. With
// --direct the frame lives in GPU-mapped direct memory (the default capture filter, section-backed
// pages) and one input is written through a CPU-only alias of its pages. With --trigger the capture
// is armed by APS5_CAPTURE_TRIGGER instead of a frame number.
#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/FrameCapture.hpp"
#include "prx/libSceAgcDriver/Execution/include/VideoOutput.hpp"
#include "prx/libSceAgcDriver/Submit/include/Acb.hpp"
#include "prx/libSceAgcDriver/Submit/include/Dcb.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "SceShaders.hpp"
#include "execution/VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
}

namespace {

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Width = 32;
constexpr std::uint32_t Height = 8;
constexpr std::uint32_t Format32x4Sint = 76;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t IdentitySwizzle = 0xfacu;
constexpr std::size_t ImageBytes = 8192;
constexpr std::size_t BufferBytes = Threads * 16;
constexpr std::size_t BlockBytes = 0x80000;
constexpr std::uint32_t Handle = 7;

constexpr std::array<std::uint32_t, 12> StoreCode{
    0x34020084, 0xe0381000, 0x80001401, 0xbf8c3f70, 0x7e3c0300, 0x7e3e0280, 0xf0201f08, 0x0001141e,
    0x7e3e0281, 0xf0201508, 0x0001141e, 0xbf810000,
};

constexpr std::array<std::uint32_t, 9> LoadCode{
    0x34020084, 0x7e3c0300, 0x7e3e0280, 0xf0001f08, 0x0001141e, 0xbf8c3f70, 0xe0781000, 0x80001401,
    0xbf810000,
};

void Require(bool condition, const std::string& what) {
    if (!condition) throw std::runtime_error("frame replay: " + what);
}

struct Layout {
    static constexpr std::size_t Store = 0x0;
    static constexpr std::size_t Load = 0x100;
    static constexpr std::size_t Label = 0x400;
    static constexpr std::size_t ComputeLabel = 0x440;
    static constexpr std::size_t LoadHeader = 0x500;
    static constexpr std::size_t StoreHeader = 0x600;
    static constexpr std::size_t StoreCopy = 0x800;
    static constexpr std::size_t InputA = 0x1000;
    static constexpr std::size_t InputB = 0x8000;
    static constexpr std::size_t Output = 0x3000;
    static constexpr std::size_t ImageA = 0x10000;
    static constexpr std::size_t ImageB = 0x20000;
    static constexpr std::size_t Commands = 0x40000;
    static constexpr std::size_t CommandStride = 0x4000;
};

class GuestBlock {
public:
    explicit GuestBlock(bool direct) {
        if (direct) {
            Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, BlockBytes, 0x10000, 0, &phys) == 0, "cannot allocate direct memory");
            void* mapped = nullptr;
            Require(sceKernelMapDirectMemory(&mapped, BlockBytes, 0x33, 0, phys, 0x10000) == 0, "cannot map direct memory for the CPU and the GPU");
            block = static_cast<std::byte*>(mapped);
        } else {
            block = static_cast<std::byte*>(GuestArena::GuestArenaAllocate_nid_postfix(BlockBytes, 0x10000));
#ifdef _WIN32
            GuestArena::GuestArenaCommit_nid_postfix(block, BlockBytes, PAGE_READWRITE, 0x4000);
#endif
            GuestAllocations::Mutation().Add(block, BlockBytes, true, true);
        }
        std::memset(block, 0, BlockBytes);
    }
    std::byte* At(std::size_t offset) const { return block + offset; }
    std::uint64_t Address(std::size_t offset) const { return reinterpret_cast<std::uintptr_t>(block + offset); }
    // A CPU-only mapping of the 16 KiB page at `offset` (direct memory only), or the page itself.
    std::byte* Alias(std::size_t offset) const {
        if (phys < 0) return At(offset);
        void* mapped = nullptr;
        Require(sceKernelMapDirectMemory(&mapped, 0x4000, 3, 0, phys + static_cast<std::int64_t>(offset), 0x4000) == 0, "cannot map a CPU alias of direct memory");
        return static_cast<std::byte*>(mapped);
    }

private:
    std::byte* block = nullptr;
    std::int64_t phys = -1;
};

class Ready final : public AgcDriver::IFlipRequest {
public:
    void GpuReady(const std::shared_ptr<AgcDriver::FrameTiming>&) override {}
    void Fail(std::exception_ptr) noexcept override {}
};

class Output final : public AgcDriver::IVideoOutput {
public:
    std::shared_ptr<AgcDriver::IFlipRequest> Reserve(const AgcDriver::FlipInfo&) override { return std::make_shared<Ready>(); }
    void Fail(std::exception_ptr) noexcept override {}
};

class Commands {
public:
    void SetSh(std::uint32_t offset, std::initializer_list<std::uint32_t> values) {
        words.push_back((3u << 30u) | (static_cast<std::uint32_t>(values.size()) << 16u) | (0x76u << 8u));
        words.push_back(offset);
        words.insert(words.end(), values);
    }
    void ComputeSetup() {
        SetSh(0x207, {Threads, 1, 1});
        SetSh(0x212, {0, 12u << 1u});
    }
    void Program(std::uint64_t address) { SetSh(0x20c, {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u)}); }
    void UserData(std::uint64_t buffer, std::uint64_t image) {
        const std::array<std::uint32_t, 12> data{
            static_cast<std::uint32_t>(buffer), static_cast<std::uint32_t>((buffer >> 32u) & 0xffffu), static_cast<std::uint32_t>(BufferBytes), 0x31016facu,
            static_cast<std::uint32_t>(image >> 8u), static_cast<std::uint32_t>((image >> 40u) & 0xffu) | (Format32x4Sint << 20u) | (((Width - 1u) & 3u) << 30u), ((Width - 1u) >> 2u) | ((Height - 1u) << 14u), IdentitySwizzle | (Type2D << 28u),
            0, 0, 0, 0,
        };
        words.push_back((3u << 30u) | (12u << 16u) | (0x76u << 8u));
        words.push_back(0x240);
        words.insert(words.end(), data.begin(), data.end());
    }
    void Dispatch() { words.insert(words.end(), {0xc0031500u, 1u, 1u, 1u, 0x8041u}); }
    void WriteLabel(std::uint64_t address, std::uint32_t value) { words.insert(words.end(), {0xc0033700u, 0x00100200u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value}); }
    void WaitLabel(std::uint64_t address, std::uint32_t value) { words.insert(words.end(), {0xc0053c00u, 0x13u, static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u), value, 0xffffffffu, 0x19u}); }
    void Flip() { words.insert(words.end(), {AgcDriver::FlipPacketHeader, Handle, 0xfffffffeu, 1u, 0x76543211u, 0xfedcba98u}); }

    // Writes the stream into guest memory (a CPU write the capture has to see) and submits it.
    void Submit(const GuestBlock& block, std::size_t slot, std::uint32_t queue) const {
        auto* target = block.At(Layout::Commands + slot * Layout::CommandStride);
        Require(words.size() * 4 <= Layout::CommandStride, "command stream too long");
        std::memcpy(target, words.data(), words.size() * 4);
        const Packet packet{reinterpret_cast<std::uint32_t*>(target), static_cast<std::uint32_t>(words.size()), 0, {}};
        Require((queue == 0 ? sceAgcDriverSubmitDcb(&packet) : sceAgcDriverSubmitAcb(queue, &packet)) == 0, "submission failed");
    }

private:
    std::vector<std::uint32_t> words;
};

// Registers the compute program at `code` from a header at `header`, then overwrites the program
// in guest memory: dispatches of it must take the registered code, so the replay has to restore
// the registry (or replay the registration) to produce the same result.
template <std::size_t N>
void RegisterThenClobber(const GuestBlock& block, std::size_t header, std::size_t code, const std::array<std::uint32_t, N>& words) {
    std::memcpy(block.At(code), words.data(), sizeof(words));
    Shader shader{};
    shader.file_header = 0x34333231u;
    shader.version = 0x18u;
    shader.code = block.At(code);
    shader.header_size = sizeof(Shader);
    shader.shader_size = static_cast<std::uint32_t>(sizeof(words));
    shader.type = 0;
    std::memcpy(block.At(header), &shader, sizeof(shader));
    AgcDriverRegisterShader_nid_postfix(reinterpret_cast<const Shader*>(block.At(header)));
    const std::uint32_t end = 0xbf810000u;
    for (std::size_t i = 0; i < N; ++i) std::memcpy(block.At(code + i * 4), &end, 4);
}

void FillInput(std::byte* target, std::uint32_t seed) {
    std::array<std::uint32_t, Threads * 4> values{};
    for (std::uint32_t i = 0; i < values.size(); ++i) values[i] = (i + 1u) * 0x9e3779b1u ^ seed;
    std::memcpy(target, values.data(), sizeof(values));
}

std::vector<std::byte> Bytes(const GuestBlock& block, std::size_t offset, std::size_t size) {
    return {block.At(offset), block.At(offset) + size};
}

std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(static_cast<bool>(file), "cannot open " + path.string());
    file.seekg(0, std::ios::end);
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void RunFrames(const GuestBlock& block, const std::filesystem::path& trigger) {
    std::memcpy(block.At(Layout::Store), StoreCode.data(), sizeof(StoreCode));
    RegisterThenClobber(block, Layout::LoadHeader, Layout::Load, LoadCode);
    for (std::size_t i = 0; i < ImageBytes; ++i) {
        *block.At(Layout::ImageA + i) = static_cast<std::byte>(0x80u + 37u * i);
        *block.At(Layout::ImageB + i) = static_cast<std::byte>(0x11u + 53u * i);
    }
    FillInput(block.At(Layout::InputA), 0x1000u);
    FillInput(block.At(Layout::InputB), 0x2000u);
    auto* aliasB = block.Alias(Layout::InputB);
    AgcDriverRegisterVideoOutput_nid_postfix(Handle, std::make_shared<Output>());

    Commands warmGraphics;
    warmGraphics.ComputeSetup();
    warmGraphics.Program(block.Address(Layout::Store));
    warmGraphics.UserData(block.Address(Layout::InputA), block.Address(Layout::ImageA));
    warmGraphics.Dispatch();
    warmGraphics.Submit(block, 0, 0);
    Commands warmCompute;
    warmCompute.ComputeSetup();
    warmCompute.Program(block.Address(Layout::Store));
    warmCompute.UserData(block.Address(Layout::InputB), block.Address(Layout::ImageB));
    warmCompute.Dispatch();
    warmCompute.Submit(block, 1, 0x20);
    if (!trigger.empty()) std::ofstream(trigger).put('1');
    Commands warmFlip;
    warmFlip.Flip();
    warmFlip.Submit(block, 2, 0);

    FillInput(block.At(Layout::InputA), 0x3000u);
    Commands first;
    first.Program(block.Address(Layout::Store));
    first.UserData(block.Address(Layout::InputA), block.Address(Layout::ImageA));
    first.Dispatch();
    first.WriteLabel(block.Address(Layout::Label), 1);
    first.Submit(block, 3, 0);

    FillInput(aliasB, 0x4000u);
    RegisterThenClobber(block, Layout::StoreHeader, Layout::StoreCopy, StoreCode);
    Commands second;
    second.WaitLabel(block.Address(Layout::Label), 1);
    second.Program(block.Address(Layout::StoreCopy));
    second.UserData(block.Address(Layout::InputB), block.Address(Layout::ImageB));
    second.Dispatch();
    second.WriteLabel(block.Address(Layout::ComputeLabel), 1);
    second.Submit(block, 4, 0x20);

    Commands third;
    third.WaitLabel(block.Address(Layout::ComputeLabel), 1);
    third.Program(block.Address(Layout::Load));
    third.UserData(block.Address(Layout::Output), block.Address(Layout::ImageA));
    third.Dispatch();
    third.Flip();
    third.Submit(block, 5, 0);
}

}

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            std::fputs("usage: agc_driver_frame_replay_tests <agc_frame_replay executable> [--direct | --trigger]\n", stderr);
            return 2;
        }
        const std::string mode = argc > 2 ? argv[2] : "";
        const bool direct = mode == "--direct";
        const bool triggered = mode == "--trigger";
        {
            const auto probe = OpenVulkanTestDevice();
            if (!probe) return VulkanTestSkipped;
        }
        const auto directory = std::filesystem::temp_directory_path() / ("aps5-frame-replay-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        const auto trigger = triggered ? directory / "capture-now" : std::filesystem::path{};
        if (triggered) SetEnvironment("APS5_CAPTURE_TRIGGER", trigger.string());
        else SetEnvironment("APS5_CAPTURE_FRAME", "1");
        if (!direct) SetEnvironment("APS5_CAPTURE_ALL_MEMORY", "1");
        SetEnvironment("APS5_CAPTURE_DIR", directory.string());
        const auto capture = triggered ? directory / "frame-capture-1" : directory;
        SetEnvironment("APS5_CAPTURE_REFERENCE", "0");

        const GuestBlock block(direct);
        RunFrames(block, trigger);
        AgcDriverWaitIdle_nid_postfix();
        Require(AgcDriver::FrameReplay::Settle(10000), "the captured frame did not finish");
        const auto output = Bytes(block, Layout::Output, BufferBytes);
        const auto imageA = Bytes(block, Layout::ImageA, ImageBytes);
        const auto imageB = Bytes(block, Layout::ImageB, ImageBytes);
        Require(std::memcmp(output.data(), block.At(Layout::InputA), BufferBytes) == 0, "the captured frame itself is wrong: the load did not return the stored texels");
        Require(std::filesystem::exists(capture / "manifest.txt"), "no capture was written to " + capture.string());
        Require(!triggered || !std::filesystem::exists(trigger), "the capture trigger file was not consumed");

        const auto hex = [](std::uint64_t value) {
            char text[32];
            std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
            return std::string(text);
        };
        const auto replayImages = directory / "replay";
        const auto afterSecond = directory / "after-second";
        std::string command = "\"\"" + std::string(argv[1]) + "\" \"" + capture.string() + "\" --verbose --images \"" + replayImages.string() + "\" --after-dispatch 2 --after-dir \"" + afterSecond.string() + "\"";
        command += " --memory " + hex(block.Address(Layout::Output)) + ":" + std::to_string(BufferBytes) + ":\"" + (directory / "output.bin").string() + "\"";
        command += " --memory " + hex(block.Address(Layout::ImageA)) + ":" + std::to_string(ImageBytes) + ":\"" + (directory / "image-a.bin").string() + "\"";
        command += " --memory " + hex(block.Address(Layout::ImageB)) + ":" + std::to_string(ImageBytes) + ":\"" + (directory / "image-b.bin").string() + "\"\"";
        std::fflush(stdout);
        std::fflush(stderr);
        const int status = std::system(command.c_str());
        Require(status == 0, "agc_frame_replay exited with " + std::to_string(status));

        Require(ReadFile(directory / "output.bin") == output, "the replayed load returned different bytes");
        Require(ReadFile(directory / "image-a.bin") == imageA, "the replayed first image differs");
        Require(ReadFile(directory / "image-b.bin") == imageB, "the replayed second image (written on the compute queue after a CPU write) differs");
        std::size_t compared = 0;
        for (const auto& entry : std::filesystem::directory_iterator(capture / "reference")) {
            const auto replayed = replayImages / entry.path().filename();
            Require(std::filesystem::exists(replayed), "the replay has no image " + entry.path().filename().string());
            Require(ReadFile(entry.path()) == ReadFile(replayed), "replayed image " + entry.path().filename().string() + " differs from the capture's reference");
            ++compared;
        }
        Require(compared >= 2, "the capture wrote " + std::to_string(compared) + " reference images, expected both storage images");
        const auto compare = "\"\"" + std::string(argv[1]) + "\" --compare \"" + (capture / "reference").string() + "\" \"" + replayImages.string() + "\"\"";
        Require(std::system(compare.c_str()) == 0, "agc_frame_replay --compare reported a difference");
        const auto second = [&](const std::filesystem::path& folder) {
            char name[96];
            std::snprintf(name, sizeof(name), "storage_%llx_%ux%u_t0_f", static_cast<unsigned long long>(block.Address(Layout::ImageB)), Width, Height);
            for (const auto& entry : std::filesystem::directory_iterator(folder)) {
                if (entry.path().filename().string().starts_with(name)) return entry.path();
            }
            throw std::runtime_error("frame replay: no dump of the second image in " + folder.string());
        };
        Require(ReadFile(second(afterSecond)) == ReadFile(second(capture / "reference")), "the image dumped right after the second dispatch differs from the frame's final second image");
        LibcRunShutdown_nid_postfix();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        std::printf("frame replay tests passed (%zu images compared)\n", compared);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
