#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::string_view rayGroupLayoutVirtualIncludeName =
    "__pelican_ray_group_layout.glsl";

enum class RayRecordType : std::uint8_t {
    i32,
    u32,
    f32,
    vec2,
    vec3,
    vec4,
};

struct RayRecordMemberLayout {
    std::string name;
    RayRecordType type = RayRecordType::u32;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t alignment = 0;

    bool operator==(const RayRecordMemberLayout &) const = default;
};

struct RayGroupLayout {
    std::string source_reference;
    std::string digest_sha256;
    std::vector<std::string> ray_types;
    std::string default_group;
    std::vector<std::string> groups;
    std::vector<std::string> misses;
    std::vector<RayRecordMemberLayout> record;
    std::uint32_t data_size = 0;

    std::uint32_t rayTypeIndex(std::string_view name) const;
    std::uint32_t groupIndex(std::string_view name) const;
    std::uint32_t missIndex(std::string_view name) const;
};

RayGroupLayout parseRayGroupLayout(
    std::string_view raw_bytes,
    std::string source_reference = {});

std::string_view rayRecordTypeName(RayRecordType type) noexcept;
nlohmann::json canonicalRayRecordAbiJson(
    const RayGroupLayout &layout);
std::string makeRayGroupLayoutVirtualInclude(
    const RayGroupLayout &layout);

struct RayBindingSelector {
    enum class Kind : std::uint8_t {
        document,
        asset_material,
    };

    Kind kind = Kind::document;
    std::string reference;
    std::uint32_t source_material_index = 0;

    bool operator==(const RayBindingSelector &) const = default;
};

struct RayMaterialBinding {
    RayBindingSelector selector;
    std::uint32_t group_index = 0;
    std::vector<std::byte> record_data;
};

struct RayBindings {
    std::string source_reference;
    std::string digest_sha256;
    std::vector<RayMaterialBinding> bindings;
};

RayBindings parseRayBindings(
    std::string_view raw_bytes,
    const RayGroupLayout &layout,
    std::string source_reference = {});

// Parsed exactly once from the authored root and shared by every requested
// logical variant (flat/xr/preview). Feature and preset composition never own
// or merge these references.
struct RayTracingProjectConfig {
    RayGroupLayout layout;
    std::optional<RayBindings> bindings;
};

std::shared_ptr<const RayTracingProjectConfig>
loadRayTracingProjectConfig(
    const nlohmann::json &authored_root,
    const std::function<std::string(std::string_view)> &load_document,
    bool runtime_shader_compiler_enabled);

struct RaySceneRowInput {
    // Persistent scene identity: object name followed by the merged-component
    // ordinal. It deliberately excludes draw-inventory publication order.
    std::string scene_identity;
    std::string geometry_identity;
    std::string material_document;
    std::string material_asset;
    std::uint32_t source_material_index = 0;
    std::size_t source_index = 0;
};

struct RaySceneRow {
    std::uint32_t q = 0;
    std::size_t source_index = 0;
    std::uint32_t group_index = 0;
    std::vector<std::byte> record_data;
    std::string scene_identity;
    std::string geometry_identity;
    std::string material_reference;
};

struct RaySceneRowSnapshot {
    std::string identity_digest_sha256;
    std::vector<RaySceneRow> rows;
};

RaySceneRowSnapshot resolveRaySceneRows(
    std::span<const RaySceneRowInput> inputs,
    const RayGroupLayout &layout,
    const RayBindings *bindings);

struct RayTracingRecordAddressing {
    std::uint32_t record_index_stride = 0;
    std::vector<std::uint32_t> traced_ray_type_indices;
};

void validateRayTracingRecordAddressing(
    std::uint32_t sbt_record_offset,
    std::uint32_t sbt_record_stride,
    std::uint32_t miss_index,
    const RayGroupLayout &layout,
    const RayTracingRecordAddressing &addressing);

std::uint32_t rayTracingInstanceRecordOffset(
    std::uint32_t q, std::uint32_t ray_type_count);

struct RayTracingHitTablePackRequest {
    std::uint32_t shader_group_handle_size = 0;
    std::uint32_t shader_group_handle_alignment = 0;
    std::uint32_t max_shader_group_stride = 0;
    // H[g,r], tightly packed, group-major then ray-type-major.
    std::span<const std::byte> group_handles;
    std::span<const RaySceneRow> rows;
    // Empty means every ray type is populated (useful for the independent
    // CPU oracle). Production passes the task's exact traced subset; record
    // data for all other valid no-op slots remains zero.
    std::span<const std::uint32_t> traced_ray_type_indices;
};

struct RayTracingPackedHitTable {
    std::uint32_t data_start = 0;
    std::uint32_t record_index_stride = 0;
    std::uint32_t record_byte_stride = 0;
    std::uint32_t record_count = 0;
    std::vector<std::byte> bytes;
};

RayTracingPackedHitTable packRayTracingHitTable(
    const RayGroupLayout &layout,
    const RayTracingHitTablePackRequest &request);

struct RayTracingSbtWriteObservation {
    int pipeline_handle = -1;
    std::string layout_digest_sha256;
    std::string row_digest_sha256;
    std::uint32_t record_index_stride = 0;
    std::uint32_t record_byte_stride = 0;
    std::uint32_t record_count = 0;
    std::vector<std::byte> hit_bytes;
};

using RayTracingSbtWriteObserver =
    std::function<void(const RayTracingSbtWriteObservation &)>;

class ScopedRayTracingSbtWriteObserver {
    RayTracingSbtWriteObserver previous_;

  public:
    explicit ScopedRayTracingSbtWriteObserver(
        RayTracingSbtWriteObserver observer);
    ~ScopedRayTracingSbtWriteObserver();
    ScopedRayTracingSbtWriteObserver(
        const ScopedRayTracingSbtWriteObserver &) = delete;
    ScopedRayTracingSbtWriteObserver &operator=(
        const ScopedRayTracingSbtWriteObserver &) = delete;
};

// Called only by the production GPU write branch, immediately before the
// bytes are copied to the addressed SBT buffer.
void observeRayTracingSbtWrite(
    const RayTracingSbtWriteObservation &observation);

using RayTracingTlasRecordOffsetOverrideForTesting =
    std::function<std::uint32_t(
        std::uint32_t q, std::uint32_t correct_offset)>;

// Test-only mutation seam around the value consumed by the production TLAS
// lowering path. The default is the validated q*R value; overriding it lets
// an end-to-end GPU sentinel prove that the TLAS base is not accidentally q.
class ScopedRayTracingTlasRecordOffsetOverrideForTesting {
    RayTracingTlasRecordOffsetOverrideForTesting previous_;

  public:
    explicit ScopedRayTracingTlasRecordOffsetOverrideForTesting(
        RayTracingTlasRecordOffsetOverrideForTesting override_offset);
    ~ScopedRayTracingTlasRecordOffsetOverrideForTesting();
    ScopedRayTracingTlasRecordOffsetOverrideForTesting(
        const ScopedRayTracingTlasRecordOffsetOverrideForTesting &) = delete;
    ScopedRayTracingTlasRecordOffsetOverrideForTesting &operator=(
        const ScopedRayTracingTlasRecordOffsetOverrideForTesting &) = delete;
};

std::uint32_t overrideRayTracingTlasRecordOffsetForTesting(
    std::uint32_t q, std::uint32_t correct_offset);

enum class RayTracingJointTransactionPoint : std::uint8_t {
    after_compile,
    after_pipeline,
    after_sbt,
    after_as_record,
    submit,
};

using RayTracingJointTransactionInjector =
    std::function<void(RayTracingJointTransactionPoint)>;

// Test delegate installed around real production registration/rendering.  It
// is deliberately invoked only at the five joint-transaction boundaries;
// throwing exercises the same rollback guards as a real failure there.
class ScopedRayTracingJointTransactionInjector {
    RayTracingJointTransactionInjector previous_;

  public:
    explicit ScopedRayTracingJointTransactionInjector(
        RayTracingJointTransactionInjector injector);
    ~ScopedRayTracingJointTransactionInjector();
    ScopedRayTracingJointTransactionInjector(
        const ScopedRayTracingJointTransactionInjector &) = delete;
    ScopedRayTracingJointTransactionInjector &operator=(
        const ScopedRayTracingJointTransactionInjector &) = delete;
};

void injectRayTracingJointTransactionPoint(
    RayTracingJointTransactionPoint point);

} // namespace Pelican
