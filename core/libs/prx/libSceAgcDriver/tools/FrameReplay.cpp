// agc_frame_replay <capture directory> [options]: replays a frame captured with APS5_CAPTURE_FRAME
// or APS5_CAPTURE_TRIGGER through the driver, without the title (profiling/FRAME-REPLAY.md).
// agc_frame_replay --compare <reference dir> <replay dir>: compares two storage image dumps.
#include "prx/libSceAgcDriver/Execution/include/Driver/Capture/FrameCapture.hpp"
#include "prx/libc/include/Shutdown.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void Usage() {
    std::fputs("usage: agc_frame_replay <capture directory> [options]\n"
               "  --images <dir>          dump every cached storage image / render target after the frame\n"
               "  --image <address>       only this base address (repeatable)\n"
               "  --min-width <pixels>    skip images narrower than this\n"
               "  --memory <address>:<bytes>:<file>  dump guest memory after the frame (repeatable)\n"
               "  --after-draw <n>        dump the cached images right after the n-th draw packet\n"
               "  --after-dispatch <n>    dump the cached images right after the n-th dispatch packet\n"
               "  --after-dir <dir>       where --after-draw / --after-dispatch write (default <capture>/draw-<n>)\n"
               "  --prepare-shaders       prepare every registered shader up front, as the title did\n"
               "  --verbose               log every event\n"
               "       agc_frame_replay --compare <reference dir> <replay dir>\n"
               "  compares the storage_*.raw dumps of two directories (exit code 1 when any differs)\n", stderr);
}

std::uint64_t Number(const char* text) {
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 0);
    if (end == text || *end != '\0') throw std::runtime_error(std::string("not a number: ") + text);
    return value;
}

std::vector<char> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(file), {}};
}

int Compare(const std::filesystem::path& reference, const std::filesystem::path& replay) {
    constexpr std::size_t HeaderBytes = 20;
    std::size_t identical = 0;
    std::size_t different = 0;
    std::size_t missing = 0;
    for (const auto& entry : std::filesystem::directory_iterator(reference)) {
        const auto name = entry.path().filename();
        if (name.extension() != ".raw") continue;
        if (!std::filesystem::exists(replay / name)) {
            std::printf("missing   %s\n", name.string().c_str());
            ++missing;
            continue;
        }
        const auto a = ReadFile(entry.path());
        const auto b = ReadFile(replay / name);
        if (a == b) {
            ++identical;
            continue;
        }
        ++different;
        if (a.size() != b.size() || a.size() < HeaderBytes || std::memcmp(a.data(), b.data(), HeaderBytes) != 0) {
            std::printf("different %s: the image shape differs (%zu and %zu bytes)\n", name.string().c_str(), a.size(), b.size());
            continue;
        }
        std::uint32_t header[5];
        std::memcpy(header, a.data(), sizeof(header));
        const auto width = std::max<std::uint32_t>(header[1], 1);
        const auto pitch = std::max<std::uint32_t>(header[3], 1);
        const auto texelBytes = std::max<std::uint32_t>(pitch / width, 1);
        std::size_t bytes = 0;
        std::size_t texels = 0;
        int largest = 0;
        std::uint32_t firstX = 0, firstY = 0;
        for (std::size_t at = HeaderBytes; at < a.size(); at += texelBytes) {
            bool texel = false;
            for (std::size_t k = at; k < std::min(a.size(), at + texelBytes); ++k) {
                if (a[k] == b[k]) continue;
                ++bytes;
                texel = true;
                largest = std::max(largest, std::abs(static_cast<int>(static_cast<unsigned char>(a[k])) - static_cast<int>(static_cast<unsigned char>(b[k]))));
            }
            if (texel && texels++ == 0) {
                firstX = static_cast<std::uint32_t>(((at - HeaderBytes) % pitch) / texelBytes);
                firstY = static_cast<std::uint32_t>((at - HeaderBytes) / pitch);
            }
        }
        std::printf("different %s: %zu texels (%zu bytes, largest byte difference %d), first at (%u, %u)\n", name.string().c_str(), texels, bytes, largest, firstX, firstY);
    }
    std::printf("%zu identical, %zu different, %zu missing from %s\n", identical, different, missing, replay.string().c_str());
    return different == 0 && missing == 0 ? 0 : 1;
}

}

int main(int argc, char** argv) {
    AgcDriver::FrameReplay::Options options;
    try {
        if (argc == 4 && std::string(argv[1]) == "--compare") return Compare(argv[2], argv[3]);
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto next = [&]() -> const char* {
                if (i + 1 >= argc) throw std::runtime_error(argument + " needs a value");
                return argv[++i];
            };
            if (argument == "--images") options.imagesDirectory = next();
            else if (argument == "--image") options.imageAddresses.push_back(Number(next()));
            else if (argument == "--min-width") options.imagesMinimumWidth = static_cast<std::uint32_t>(Number(next()));
            else if (argument == "--memory") {
                const std::string value = next();
                const auto first = value.find(':');
                const auto second = first == std::string::npos ? std::string::npos : value.find(':', first + 1);
                if (second == std::string::npos) throw std::runtime_error("--memory takes <address>:<bytes>:<file>");
                options.memory.push_back({Number(value.substr(0, first).c_str()), Number(value.substr(first + 1, second - first - 1).c_str()), value.substr(second + 1)});
            } else if (argument == "--after-draw") options.afterDraw = Number(next());
            else if (argument == "--after-dispatch") options.afterDispatch = Number(next());
            else if (argument == "--after-dir") options.afterDirectory = next();
            else if (argument == "--prepare-shaders") options.prepareShaders = true;
            else if (argument == "--verbose") options.verbose = true;
            else if (argument == "--help" || argument == "-h") {
                Usage();
                return 0;
            } else if (!argument.empty() && argument[0] != '-' && options.directory.empty()) options.directory = argument;
            else throw std::runtime_error("unknown argument " + argument);
        }
        if (options.directory.empty()) {
            Usage();
            return 2;
        }
        const auto summary = AgcDriver::FrameReplay::Run(options);
        std::printf("replayed %llu events (%llu submissions, %llu draws, %llu dispatches), %s\n", static_cast<unsigned long long>(summary.events), static_cast<unsigned long long>(summary.submissions), static_cast<unsigned long long>(summary.draws), static_cast<unsigned long long>(summary.dispatches), summary.drained ? "finished" : "did not finish");
        LibcRunShutdown_nid_postfix();
        return summary.drained ? 0 : 3;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_frame_replay: %s\n", error.what());
        try { LibcRunShutdown_nid_postfix(); } catch (...) {}
        return 1;
    }
}
