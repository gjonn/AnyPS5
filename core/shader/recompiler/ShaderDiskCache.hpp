#ifndef CORE_SHADER_RECOMPILER_SHADERDISKCACHE_HPP
#define CORE_SHADER_RECOMPILER_SHADERDISKCACHE_HPP

#include "CompiledVariant.hpp"
#include "PipelineSpecialization.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ShaderRecompiler::ShaderDiskCache {

inline constexpr std::uint32_t FormatVersion = 20;

enum class LoadStatus {
    Loaded,
    Absent,
    KeyMismatch,
    Rejected,
};

struct Counters {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t writes = 0;
    std::uint64_t loadFailures = 0;
    std::uint64_t writeFailures = 0;
    std::uint64_t bytesRead = 0;
    std::uint64_t bytesWritten = 0;
    std::uint64_t moduleHits = 0;
    std::uint64_t moduleMisses = 0;
    std::uint64_t moduleWrites = 0;
};

struct SpecializedModuleEntry {
    std::vector<std::uint32_t> spirv;
    std::vector<std::uint32_t> bindings;
    bool pushData = false;
};

struct SpecializationTarget {
    std::uint32_t vulkanVersion = 0;
    std::uint32_t spirvVersion = 0;
    bool nonConstantImageOffsets = false;
};

[[nodiscard]] std::uint64_t SourceVersion();

void BuildKey(const RecompileRequest& request, std::uint32_t hostSubgroupSize, std::vector<std::byte>& key);

[[nodiscard]] std::string EntryName(std::span<const std::byte> key);

void EncodeResult(const RecompileResult& result, std::vector<std::byte>& out);
[[nodiscard]] bool DecodeResult(std::span<const std::byte> bytes, RecompileResult& result);

[[nodiscard]] std::vector<std::byte> EncodeEntry(std::span<const std::byte> key, const CompiledVariant& variant);
[[nodiscard]] LoadStatus DecodeEntry(std::span<const std::byte> file, std::span<const std::byte> key, CompiledVariant& variant);

void BuildModuleKey(std::span<const std::uint32_t> spirv, std::span<const VertexInputPatch> patches, std::span<const std::uint32_t> classes, std::span<const PipelineSpecializationConstant> constants, const SpecializationTarget& target, std::vector<std::byte>& key);
[[nodiscard]] std::vector<std::byte> EncodeModuleEntry(std::span<const std::byte> key, const SpecializedModuleEntry& module);
[[nodiscard]] LoadStatus DecodeModuleEntry(std::span<const std::byte> file, std::span<const std::byte> key, SpecializedModuleEntry& module);

[[nodiscard]] bool Enabled();
[[nodiscard]] std::filesystem::path EntryDirectory();

[[nodiscard]] bool Load(std::span<const std::byte> key, CompiledVariant& variant);
void Store(std::vector<std::byte> key, std::shared_ptr<const CompiledVariant> variant);
[[nodiscard]] bool LoadModule(std::span<const std::byte> key, SpecializedModuleEntry& module);
void StoreModule(std::vector<std::byte> key, SpecializedModuleEntry module);
void Flush();
[[nodiscard]] Counters Totals();

}

#endif
