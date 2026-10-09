#pragma once

#include "../handle.hpp"
#include "../material/material.hpp"
#include "morphtarget.hpp"
#include "skeletalanimation.hpp"
#include "vrmsemantic.hpp"
#include <cstdint>
#include <memory>
#include <limits>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

namespace Pelican {

struct ModelOccurrenceIdentityInfo;

inline constexpr std::uint32_t modelPrimitiveAttributeNormal = 1u << 0u;
inline constexpr std::uint32_t modelPrimitiveAttributeUv0 = 1u << 1u;
inline constexpr std::uint32_t modelPrimitiveAttributeColor0 = 1u << 2u;

using ModelTemplateId = int;

PELICAN_DEFINE_HANDLE(ModelAssetId, std::uint64_t)

inline constexpr bool isValidModelAssetId(ModelAssetId id) noexcept { return id.value != 0; }

inline constexpr std::uint32_t noSourceMaterialIndex =
    std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t noSourceNodeIndex =
    std::numeric_limits<std::uint32_t>::max();

enum class PrimitiveViewVisibility : std::uint8_t {
    third_person_only = 0,
    both = 1,
    first_person_only = 2,
};

struct SourceMaterialInitialValues {
    std::uint32_t source_material_index = noSourceMaterialIndex;
    glm::vec4 base_color_factor{1.0f};
    glm::vec4 emissive_factor{0.0f, 0.0f, 0.0f, 1.0f};
    glm::vec2 uv_offset{0.0f};
    glm::vec2 uv_scale{1.0f};
    float uv_rotation = 0.0f;
};

// Indexed by the source glTF material index. A shared const table keeps the
// source values tied to one immutable model generation instead of the mutable
// global material records used by rendering.
struct SourceMaterialInitialValueTable {
    std::vector<SourceMaterialInitialValues> values;
};

struct ModelPrimitiveBounds {
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};

    bool operator==(const ModelPrimitiveBounds &) const = default;
};

// CPU-side source for the per-frame world bounds used by draw-sort providers.
// The base and morph delta envelopes contain only vertices referenced by this
// indexed primitive. Skinning is applied later from the current instance
// palette, after the current morph weights have expanded this local envelope.
struct ModelPrimitiveBoundsSource {
    ModelPrimitiveBounds base;
    std::vector<ModelPrimitiveBounds> morph_position_deltas;
    std::uint32_t morph_weight_offset = 0;

    bool operator==(const ModelPrimitiveBoundsSource &) const = default;
};

struct ModelPrimitiveRefInfo {
    uint32_t index_count = 0;
    uint32_t index_offset = 0;
    int32_t vert_offset = 0;
    bool skinned = false;
    // Stable coordinates in the source GLB. These are intentionally separate
    // from GPU allocation offsets so an importer mapping survives regrouping.
    std::uint32_t mesh_index = 0;
    std::uint32_t primitive_index = 0;
    std::uint32_t node_index = noSourceNodeIndex;
    // Stable authored occurrence identity and its source metadata. The legacy
    // indices above remain diagnostic coordinates only.
    std::shared_ptr<ModelOccurrenceIdentityInfo> occurrence;
    PrimitiveViewVisibility view_visibility = PrimitiveViewVisibility::both;
    std::shared_ptr<const ModelPrimitiveBoundsSource> bounds_source;
    // Ray-query BLAS input metadata. The allocation identity is never reused,
    // so a released range cannot alias a cached BLAS for a later model.
    std::uint32_t vertex_count = 0;
    bool morph_deformed = false;
    bool vat_deformed = false;
    std::uint64_t geometry_allocation_id = 0;
    // Original importer presence, retained even though upload writes ABI
    // defaults for absent streams.
    std::uint32_t ray_attribute_presence = 0;
};

// Exact suballocations owned by one model generation. Keeping this metadata
// next to the template lets a rejected reload return its candidate ranges and
// lets a successful reload retire only the previous generation.
struct ModelGeometryAllocation {
    ModelPrimitiveRefInfo primitive;
    uint32_t vertex_count = 0;
    uint32_t morph_delta_offset = 0;
    uint32_t morph_delta_count = 0;
};

struct ModelGpuResources {
    std::vector<GlobalTextureId> textures;
    std::vector<GlobalMaterialId> materials;
    std::vector<ModelGeometryAllocation> geometry;
    bool released = false;
};

// Immutable CPU triangles in model space, before per-instance transforms.
// Kept independently of render variants and GPU allocations.
struct ModelCollisionMesh {
    std::uint32_t node_index = 0;
    std::vector<std::string> node_name_path;
    std::vector<std::uint32_t> parent_index_path;
    std::vector<glm::vec3> positions;
    std::vector<std::uint32_t> indices;
};

// Diagnostics belong to a model generation, including an unpublished reload candidate.
struct MaterialBindingIssue {
    std::optional<std::size_t> row; // zero-based bindings array index; absent for an unbound primitive
    std::uint32_t mesh = 0;
    std::uint32_t primitive = 0;
    std::string usd_path;
    std::string material; // non-empty only for an unresolved material name
    std::string reason;
};

struct ModelTemplate {
    using PrimitiveRefInfo = ModelPrimitiveRefInfo;

    struct MaterialPrimitives {
        GlobalMaterialId material;
        std::vector<PrimitiveRefInfo> primitives;
        std::optional<std::uint32_t> source_material_index;
    };

    struct NamedMaterial {
        std::string name;
        GlobalMaterialId material;
        std::optional<MaterialVariantRouting> routing;
        // Retained for CPU-only binding validation as well as live loads.
        bool vat_playback = false;
    };

    std::vector<MaterialPrimitives> material_primitives;
    std::vector<MaterialBindingIssue> binding_issues;
    std::shared_ptr<const std::vector<ModelCollisionMesh>> collision_meshes;
    std::vector<NamedMaterial> named_materials;
    std::shared_ptr<SkeletalModelData> skeletal;
    std::shared_ptr<const MorphTargetLayout> morph_targets;
    std::shared_ptr<const SourceMaterialInitialValueTable> material_initial_values;
    std::shared_ptr<const VrmSemanticData> vrm_semantic;
    std::shared_ptr<ModelGpuResources> gpu_resources;
    ModelAssetId asset_id{};
    // The project-authored model reference, preserved across physical-path
    // resolution and content reload. Ray material asset selectors use this
    // value rather than a transient path or numeric GPU identity.
    std::string authored_asset_reference;
    // Logical project name, retained for scene override diagnostics.
    std::string authored_model_name;
    bool hasPrimitive(std::uint32_t mesh, std::uint32_t primitive) const noexcept;
    std::uint64_t content_revision = 1;
    std::uint64_t compatibility_revision = 0;
};

// Project-authored names are an additive resolution domain. Validate it
// separately so an ambiguity fails even when a binding document does not
// happen to reference the colliding name.
void validateNamedMaterialResolutionDomains(
    const ModelTemplate &model,
    std::span<const ModelTemplate::NamedMaterial>
        project_materials,
    std::string_view model_name);

// Bindings address whole-model mesh/primitive coordinates. Stale targets,
// uncovered primitives and missing material names recover with diagnostics;
// structural errors, ambiguous names, routing and VAT violations still fail.
// keep_own_material is a legacy notification for unresolved material names.
void applyPrimitiveMaterialBindings(
    ModelTemplate &model, const PrimitiveMaterialBindingDocument &document,
    std::string_view model_name, std::optional<std::string_view> fragment = std::nullopt,
    std::span<const ModelTemplate::NamedMaterial>
        project_materials = {},
    const std::function<bool(const PrimitiveMaterialBinding &)>
        &keep_own_material = {});

} // namespace Pelican
