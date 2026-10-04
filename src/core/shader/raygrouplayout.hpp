#pragma once

#include "../container.hpp"
#include "../userpublic/components/rayinstance.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <glm/mat4x4.hpp>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

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

// Callable records have their own std430 schema, without hit-group engine fields.
struct RayCallableRecordLayout {
    std::vector<RayRecordMemberLayout> members;
    std::uint32_t data_size = 0;
};

RayCallableRecordLayout parseRayCallableRecordLayout(
    const nlohmann::json &members, std::string_view diagnostic = {});
std::vector<std::byte> encodeRayCallableRecord(
    const nlohmann::json &values, const RayCallableRecordLayout &layout,
    std::string_view diagnostic = {});
std::vector<std::vector<std::byte>> encodeRayCallableRecords(
    const nlohmann::json &records, const RayCallableRecordLayout &layout,
    std::size_t callable_count, std::string_view diagnostic = {});

enum class RayEngineRecordField : std::uint8_t {
    material_index,
    geometry_index,
    geometry_kind,
    procedural_transform,
};

enum class RayPortableProfile : std::uint8_t {
    material_alpha_v1,
};

// A project either has no ray group layout or has one, and a layout is
// version 3: earlier versions are refused by name when the layout is parsed.
enum class RayLayoutSemanticMode : std::uint8_t {
    LegacyNoLayout,
    LayoutV3,
};

// The only versions each ray document is read in. Any other integer version
// is `<schema>.version_unsupported@1`.
inline constexpr std::uint32_t rayGroupLayoutVersion = 3;
inline constexpr std::uint32_t rayBindingsVersion = 2;

enum class RayGeometryKind : std::uint8_t {
    triangles,
    aabbs,
};

enum class RayGroupType : std::uint8_t {
    triangles,
    procedural,
};

struct RayTracingProjectConfig;

struct RayAsMaterialSelector {
    enum class Kind : std::uint8_t {
        document_material,
        asset_material,
    };

    Kind kind = Kind::document_material;
    std::string reference;
    std::string material_name;
    std::uint32_t source_material_index = 0;

    std::string canonicalKey() const;
    bool operator==(const RayAsMaterialSelector &) const = default;
};

struct RayAsGeometryRule {
    RayAsMaterialSelector material;
    std::uint32_t geometry_flags = 0;

    bool operator==(const RayAsGeometryRule &) const = default;
};

struct RayAccelerationStructureDeclaration {
    bool triangle_world_space = false;
    bool procedural_world_space = false;
    RayAccelerationStructureCanonicalDefaults defaults;
    RayAccelerationStructureCanonicalDefaults procedural_defaults{
        .geometry_flags = rayGeometryOpaque};
    std::vector<RayAsGeometryRule> rules;

    bool operator==(const RayAccelerationStructureDeclaration &) const =
        default;
};

struct RayGroupLayout {
    std::string source_reference;
    std::string digest_sha256;
    std::string acceleration_structure_digest_sha256;
    std::vector<std::string> ray_types;
    std::string default_group;
    std::vector<std::string> groups;
    std::vector<RayGroupType> group_types;
    std::vector<std::string> misses;
    // Authored members retain their authored order, but their offsets are the
    // offsets in the one physical std430 shader-record struct.
    std::vector<RayRecordMemberLayout> record;
    // Canonical physical order: requested engine fields first in
    // [material_index, geometry_index, geometry_kind] order, followed by
    // authored members.
    std::vector<RayEngineRecordField> engine_record;
    std::vector<RayRecordMemberLayout> physical_record;
    std::uint32_t data_size = 0;
    // Authored only at the layout document top level. Absence is the exact
    // pre-WP369 path and creates no generated shader/wire/cache input.
    std::optional<RayPortableProfile> portable_profile;
    std::optional<RayAccelerationStructureDeclaration> acceleration_structure;

    std::uint32_t rayTypeIndex(std::string_view name) const;
    std::uint32_t groupIndex(std::string_view name) const;
    std::uint32_t missIndex(std::string_view name) const;
    bool usesRayTables() const noexcept { return !engine_record.empty(); }
    bool usesMaterialAlphaProfile() const noexcept {
        return portable_profile ==
               RayPortableProfile::material_alpha_v1;
    }
    std::optional<std::uint32_t> engineRecordOffset(
        RayEngineRecordField field) const noexcept;
};

// Process-lifetime runtime gate, owned by FastModuleContainer.  Parsing stays
// pure; startup configures this before builtin ECS registration, and render
// graph reloads validate against the immutable mode.
DECLARE_MODULE(RayLayoutRuntimeState) {
    bool configured_ = false;
    RayLayoutSemanticMode mode_ = RayLayoutSemanticMode::LegacyNoLayout;

  public:
    void configure(RayLayoutSemanticMode mode);
    void validate(RayLayoutSemanticMode mode) const;
    bool configured() const noexcept { return configured_; }
    RayLayoutSemanticMode mode() const noexcept { return mode_; }
    bool builtinRayInstanceEnabled() const noexcept {
        return configured_ && mode_ == RayLayoutSemanticMode::LayoutV3;
    }
};

RayLayoutSemanticMode rayLayoutSemanticMode(
    const RayTracingProjectConfig *config) noexcept;

RayGroupLayout parseRayGroupLayout(
    std::string_view raw_bytes,
    std::string source_reference = {});

std::string_view rayRecordTypeName(RayRecordType type) noexcept;
nlohmann::json canonicalRayRecordAbiJson(
    const RayGroupLayout &layout);
std::string makeRayGroupLayoutVirtualInclude(
    const RayGroupLayout &layout);
std::string makeMaterialAlphaAnyHitSource(
    const RayGroupLayout &layout);

struct RayBindingSelector {
    enum class Kind : std::uint8_t {
        document,
        asset_material,
        procedural,
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
    RayGeometryKind geometry_kind = RayGeometryKind::triangles;
    // Persistent scene identity: object name followed by the merged-component
    // ordinal. It deliberately excludes draw-inventory publication order.
    std::string scene_identity;
    std::string geometry_identity;
    // Equality domain for the ambiguity guard.  Unlike the canonical q sort
    // key above, this contains only the physical GPU geometry range and the
    // canonical transform; authored asset/mesh/node aliases must not make
    // coincident geometry look distinct.
    std::string geometry_match_identity;
    std::string material_document;
    std::string material_asset;
    std::string material_name;
    // This is optional before material identity canonicalization.  In
    // particular, a missing source index must never silently become index 0.
    std::optional<std::uint32_t> source_material_index;
    int resolved_material_handle = -1;
    std::uint64_t resolved_material_generation = 0;
    std::shared_ptr<const void> resolved_material_lease;
    std::uint64_t geometry_allocation_id = 0;
    std::string procedural_asset;
    nlohmann::ordered_json procedural_record =
        nlohmann::ordered_json::object();
    std::uint64_t aabb_buffer_address = 0;
    glm::mat4 world_transform{1.0F};
    std::uint64_t authoring_object_id = 0;
    std::optional<RayInstanceComponent> ray_instance;
    std::size_t source_index = 0;
    // Absolute source overrides take priority. Relative UV transforms are
    // transported only when no absolute UV transform exists; relative base
    // color multipliers remain post-discard and deliberately stay out.
    std::uint32_t alpha_override_mask = 0;
    std::array<float, 4> alpha_override_base_color_factor{
        1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> alpha_override_uv_offset_scale{
        0.0F, 0.0F, 1.0F, 1.0F};
    float alpha_override_uv_rotation = 0.0F;
};

struct MaterialRowDocumentIdentity {
    std::string document;
    std::string material_name;
};

struct MaterialRowAssetIdentity {
    std::string asset;
    std::uint32_t source_material_index = 0;
};

struct MaterialRowIdentityUnavailable {};

// The only material identity seen by AS rule gating and selector lookup.
// Canonicalization gives a document (with an optional material name) priority,
// then asset+valid source index, and otherwise makes the row explicitly
// unavailable.
using MaterialRowIdentity =
    std::variant<MaterialRowDocumentIdentity, MaterialRowAssetIdentity,
                 MaterialRowIdentityUnavailable>;

MaterialRowIdentity materialRowIdentity(const RaySceneRowInput &row);

struct RaySceneSettingSources {
    std::string_view mask = "engine_default";
    std::string_view instance_flags = "engine_default";
    std::string_view custom_index = "engine_default";
    std::string_view geometry_flags = "engine_default";
    std::optional<std::size_t> geometry_rule_index;
    std::optional<std::uint32_t> override_row_id;
};

nlohmann::ordered_json rayRecordFieldsJson(
    std::span<const RayRecordMemberLayout> members, std::span<const std::byte> data);

struct RaySceneRow {
    RaySceneSettingSources setting_sources;
    std::optional<std::size_t> binding_index;
    RayGeometryKind geometry_kind = RayGeometryKind::triangles;
    std::uint32_t q = 0;
    std::size_t source_index = 0;
    std::uint32_t group_index = 0;
    std::vector<std::byte> record_data;
    std::string scene_identity;
    std::string geometry_identity;
    std::string geometry_match_identity;
    std::string material_reference;
    std::string material_name;
    int resolved_material_handle = -1;
    std::uint64_t resolved_material_generation = 0;
    std::shared_ptr<const void> resolved_material_lease;
    std::uint64_t geometry_allocation_id = 0;
    std::uint64_t authoring_object_id = 0;
    std::uint8_t instance_mask =
        rayAccelerationStructureCanonicalDefaults.mask;
    std::uint32_t instance_flags =
        rayAccelerationStructureCanonicalDefaults.instance_flags;
    std::uint32_t instance_custom_index =
        rayAccelerationStructureCanonicalDefaults.custom_index;
    std::uint32_t geometry_flags =
        rayAccelerationStructureCanonicalDefaults.geometry_flags;
    std::uint32_t alpha_override_mask = 0;
    std::array<float, 4> alpha_override_base_color_factor{
        1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> alpha_override_uv_offset_scale{
        0.0F, 0.0F, 1.0F, 1.0F};
    float alpha_override_uv_rotation = 0.0F;
    std::uint32_t material_index =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t geometry_index =
        std::numeric_limits<std::uint32_t>::max();
};

struct RayOccurrenceOverrideResolution {
    std::uint64_t authoring_object_id = 0;
    std::string component_occurrence;
    nlohmann::ordered_json ray_instance_component_witness =
        nlohmann::ordered_json::object();
    std::uint32_t row_id = 0;
    std::string occurrence_id;
    std::optional<std::uint32_t> q;
    std::uint8_t mask = 0;
    std::uint32_t instance_flags = 0;
    std::uint32_t custom_index = 0;
    // Empty for an applied row. Unresolved rows retain their authored address
    // while the geometry uses object/default values.
    std::string error;
};

struct RaySceneRowSnapshot {
    std::string layout_reference;
    std::string bindings_reference;
    // RTL-1a: the digest of the bindings bytes these rows were resolved from,
    // and the bindings (by index) that matched no scene row. Such a binding
    // is not an error: a binding may be authored before the object it binds
    // is placed, and stay after the last one is removed.
    std::string bindings_digest_sha256;
    std::vector<std::size_t> unresolved_binding_indices;
    std::vector<std::string> group_names;
    // Authored schema captured with these row bytes, excluding engine-owned fields.
    std::vector<RayRecordMemberLayout> record_members;
    std::uint64_t generation = 0;
    std::string identity_digest_sha256;
    std::string material_table_digest_sha256;
    std::string geometry_table_digest_sha256;
    std::vector<RaySceneRow> rows;
    std::vector<RayOccurrenceOverrideResolution> occurrence_overrides;
};

inline constexpr std::uint32_t rayGeometryAttributeNormal = 1u << 0u;
inline constexpr std::uint32_t rayGeometryAttributeUv0 = 1u << 1u;
inline constexpr std::uint32_t rayGeometryAttributeColor0 = 1u << 2u;
inline constexpr std::uint32_t rayMissingTextureIndex = 0xffffffffu;

// The ray tables occupy one descriptor set of their own on the ray tracing
// pipeline layout. These four names are the whole contract: the generated GLSL
// in makeRayGroupLayoutVirtualInclude declares the three resources from them,
// and the pipeline layout, the descriptor set lookup and the descriptor writes
// in pipelinefactory.cpp are built from the same values, so neither side can
// drift from the other.
inline constexpr std::uint32_t rayTableDescriptorSet = 3u;
inline constexpr std::uint32_t rayMaterialTableBinding = 0u;
inline constexpr std::uint32_t rayMaterialTextureBinding = 1u;
inline constexpr std::uint32_t rayGeometryTableBinding = 2u;
inline constexpr std::uint32_t rayBindlessTextureCapacity = 1024u;
inline constexpr std::uint32_t rayAlphaOverrideBaseFactor = 1u << 0u;
inline constexpr std::uint32_t rayAlphaOverrideUvTransform = 1u << 1u;

// C++ half of the generated set-3 ABI. The offset assertions in the
// implementation are mirrored by generated GLSL declarations and tests.
struct alignas(16) RayMaterialTableRow {
    std::array<float, 4> base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    float alpha_cutoff = 0.5F;
    std::uint32_t alpha_mode = 0;
    std::uint32_t base_color_texture_index = rayMissingTextureIndex;
    std::uint32_t reserved = 0;

    bool operator==(const RayMaterialTableRow &) const = default;
};

// Profile-active per-instance material row. Its byte layout is separate from
// RayMaterialTableRow so a profile-absent layout retains the exact WP368 ABI.
struct alignas(16) RayProfileMaterialTableRow {
    std::uint32_t alpha_mode = 0;
    float cutoff = 0.5F;
    float base_factor_a = 1.0F;
    std::uint32_t texture_index = rayMissingTextureIndex;
    std::array<float, 4> uv_offset_scale{
        0.0F, 0.0F, 1.0F, 1.0F};
    float uv_rotation = 0.0F;
    std::uint32_t uses_vertex_color = 0;
    std::array<std::uint32_t, 2> reserved{};

    bool operator==(const RayProfileMaterialTableRow &) const = default;
};

struct alignas(8) RayGeometryTableRow {
    std::uint64_t vertex_address = 0;
    std::uint64_t index_address = 0;
    std::uint32_t vertex_stride = 0;
    std::uint32_t index_type = 0;
    std::uint32_t attribute_presence = 0;
    std::uint32_t reserved = 0;

    bool operator==(const RayGeometryTableRow &) const = default;
};

struct RayTracingGeometrySnapshotRow {
    RayGeometryKind geometry_kind = RayGeometryKind::triangles;
    std::uint64_t geometry_allocation_id = 0;
    std::uint32_t index_count = 0;
    std::uint32_t index_offset = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t vertex_count = 0;
    RayGeometryTableRow gpu;

    bool operator==(const RayTracingGeometrySnapshotRow &) const = default;
};

// Sorted by geometry_allocation_id and consumed by both BLAS construction and
// the set-3 geometry table. Addresses already point at the subranges, so every
// Vulkan/GLSL offset is zero and double addition is structurally impossible.
struct RayTracingGeometrySnapshot {
    std::uint64_t generation = 0;
    std::string digest_sha256;
    std::vector<RayTracingGeometrySnapshotRow> rows;
};

struct RayMaterialTableSource {
    int material_handle = -1;
    std::uint64_t material_generation = 0;
    std::shared_ptr<const void> material_lease;
    std::string canonical_key;
    std::array<float, 4> base_color_factor{1.0F, 1.0F, 1.0F, 1.0F};
    float alpha_cutoff = 0.5F;
    std::uint32_t alpha_mode = 0;
    std::string base_color_texture_identity;
    vk::ImageView base_color_srgb_view;
    vk::Sampler default_sampler;
    bool has_normalized_alpha_ir = false;
    std::string alpha_source_kind;
    std::string alpha_material_identity;
    std::string alpha_texture_identity;
    std::uint32_t normalized_alpha_mode = 0;
    float normalized_alpha_cutoff = 0.5F;
    float normalized_base_factor_a = 1.0F;
    std::array<float, 4> normalized_uv_offset_scale{
        0.0F, 0.0F, 1.0F, 1.0F};
    float normalized_uv_rotation = 0.0F;
    std::uint32_t normalized_uv_transform_state = 0;
    std::string normalized_alpha_digest_sha256;
};

struct RayTextureTableEntry {
    std::string canonical_identity;
    vk::ImageView srgb_view;
    vk::Sampler sampler;
};

struct RayTracingTableSnapshot {
    std::string material_digest_sha256;
    std::string geometry_digest_sha256;
    std::vector<std::string> material_keys;
    std::vector<std::string> material_row_digests;
    std::vector<RayMaterialTableRow> material_rows;
    std::vector<RayProfileMaterialTableRow> profile_material_rows;
    std::vector<std::uint64_t> geometry_allocation_ids;
    std::vector<std::string> geometry_row_digests;
    std::vector<RayGeometryTableRow> geometry_rows;
    std::vector<RayTextureTableEntry> textures;
    std::vector<std::shared_ptr<const void>> material_leases;
    std::vector<std::byte> material_bytes;
    std::vector<std::byte> geometry_bytes;
};

struct RayTracingTableBuildResult {
    RaySceneRowSnapshot scene_rows;
    RayTracingTableSnapshot tables;
};

RayTracingTableBuildResult buildRayTracingTableSnapshot(
    const RaySceneRowSnapshot &rows,
    std::span<const RayMaterialTableSource> materials,
    const RayTracingGeometrySnapshot &geometry,
    const RayGroupLayout *layout = nullptr);

void validateRayProfileGeometryAttributes(
    const RayProfileMaterialTableRow &row,
    const RayGeometryTableRow &geometry,
    std::uint32_t q, std::string_view material_reference);

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
bool rayTracingTlasRecordOffsetOverrideActiveForTesting() noexcept;

enum class RayTracingJointTransactionPoint : std::uint8_t {
    after_compile,
    after_pipeline,
    after_sbt,
    after_as_record,
    submit,
    after_publication,
};

using RayTracingJointTransactionInjector =
    std::function<void(RayTracingJointTransactionPoint)>;

// Test delegate installed around real production registration/rendering.  It
// is deliberately invoked only at the six joint-transaction boundaries;
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
