#include "ControlFlow/GraphBuilder.hpp"
#include "ControlFlow/Structurizer.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
        const auto bytes = input.tellg();
        if (bytes <= 0 || bytes % 4 != 0) throw std::runtime_error("invalid code file");
        std::vector<std::uint32_t> code(static_cast<std::size_t>(bytes) / 4);
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(code.data()), bytes)) throw std::runtime_error("short code read");
        const auto start = std::chrono::steady_clock::now();
        const auto decoded = ShaderRecompiler::RdnaInstructionDecoder{}.Decode(code);
        auto graph = ShaderRecompiler::GraphBuilder{}.Build(decoded);
        const auto built = std::chrono::steady_clock::now();
        ShaderRecompiler::Structurizer{}.Structurize(graph);
        const auto ended = std::chrono::steady_clock::now();
        std::ofstream output(argv[2]);
        output << ShaderRecompiler::GraphToString(graph);
        for (const auto& block : graph.blocks) {
            output << "\nblock " << block.id << " dominators:";
            for (auto id : block.dominators) output << ' ' << id;
            output << " postDominators:";
            for (auto id : block.postDominators) output << ' ' << id;
        }
        std::cout << "words=" << code.size() << " blocks=" << graph.blocks.size()
                  << " decodeBuildMs=" << std::chrono::duration<double, std::milli>(built-start).count()
                  << " structurizeMs=" << std::chrono::duration<double, std::milli>(ended-built).count() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
