#pragma once

#include "shaderreference.hpp"

#include <cstdint>
#include <compare>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class RayTracingCanonicalTypeKind {
    boolean,
    signed_integer,
    unsigned_integer,
    floating,
    vector,
    matrix,
    fixed_array,
    structure,
};

// Structural identity deliberately contains neither SPIR-V ids nor source
// names. Equality of this tree is the RT interface contract; fingerprint is
// only a diagnostic/index acceleration and is never an equality substitute.
struct RayTracingCanonicalType {
    RayTracingCanonicalTypeKind kind =
        RayTracingCanonicalTypeKind::structure;
    std::uint32_t bit_width = 0;
    std::uint32_t element_count = 0;
    std::vector<RayTracingCanonicalType> elements;

    bool operator==(const RayTracingCanonicalType &) const = default;
};

std::string describeRayTracingCanonicalType(
    const RayTracingCanonicalType &type);
std::string rayTracingCanonicalTypeFingerprint(
    const RayTracingCanonicalType &type);
std::uint64_t rayTracingCanonicalTypeByteSize(
    const RayTracingCanonicalType &type);

enum class RayTracingInterfaceStorage {
    ray_payload,
    incoming_ray_payload,
    hit_attribute,
};

struct RayTracingInterfaceVariable {
    RayTracingInterfaceStorage storage =
        RayTracingInterfaceStorage::ray_payload;
    std::optional<std::uint32_t> location;
    RayTracingCanonicalType type;
    std::string fingerprint;
    std::uint64_t byte_size = 0;

    bool operator==(const RayTracingInterfaceVariable &) const = default;
};

struct SpirvEntryPointKey {
    std::uint32_t execution_model = 0;
    std::string name;

    auto operator<=>(const SpirvEntryPointKey &) const = default;
};

struct RayTracingTraceCall {
    SpirvEntryPointKey caller_entry;
    std::uint32_t instruction_number = 0;
    RayTracingCanonicalType payload_type;
    std::uint32_t miss_index = 0;
    std::uint32_t sbt_record_offset = 0;
    std::uint32_t sbt_record_stride = 0;
    // nullopt means the Ray Flags operand was not a direct 32-bit
    // OpConstant/OpConstantNull. Unlike SBT routing operands this is accepted
    // by general RT inspection and rejected only by profile-active tasks.
    std::optional<std::uint32_t> ray_flags;

    bool operator==(const RayTracingTraceCall &) const = default;
};

struct RayTracingShaderRecordMemberReflection {
    RayTracingCanonicalType type;
    std::uint32_t offset = 0;

    bool operator==(
        const RayTracingShaderRecordMemberReflection &) const = default;
};

struct RayTracingShaderRecordReflection {
    std::vector<RayTracingShaderRecordMemberReflection> members;

    bool operator==(
        const RayTracingShaderRecordReflection &) const = default;
};

enum class ProceduralOracleClassification { eligible, sequence_mismatch };

std::string_view proceduralOracleClassificationName(ProceduralOracleClassification classification);
std::string_view proceduralOracleClassificationCode(ProceduralOracleClassification classification);

struct RayTracingEntryPointReflection {
    SpirvEntryPointKey key;
    std::string entry_digest;
    // Intersection-only golden classification; it never rejects shader loading.
    std::optional<ProceduralOracleClassification> procedural_oracle;
    std::vector<std::uint32_t> entry_body_opcodes;
    std::vector<RayTracingInterfaceVariable> interfaces;
    std::vector<RayTracingTraceCall> trace_calls;
    std::optional<RayTracingShaderRecordReflection> shader_record;

    bool operator==(const RayTracingEntryPointReflection &) const = default;
};

struct RayTracingModuleReflection {
    std::map<SpirvEntryPointKey, RayTracingEntryPointReflection>
        entries;
};

// This parser is intentionally independent of SPIRV-Tools. It remains in the
// shipping graph when PELICAN_WITH_SPIRV_LINK=OFF.
RayTracingModuleReflection inspectRayTracingSpirv(
    std::span<const std::uint32_t> spirv,
    std::string_view diagnostic_uri);

std::uint32_t spirvExecutionModelForShaderStage(ShaderStage stage);
const RayTracingEntryPointReflection &selectRayTracingEntryPoint(
    const RayTracingModuleReflection &reflection,
    ShaderStage expected_stage, std::string_view entry_name,
    std::string_view diagnostic_uri);

} // namespace Pelican
