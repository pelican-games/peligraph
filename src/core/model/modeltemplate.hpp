#pragma once

#include "../handle.hpp"
#include "../material/material.hpp"
#include "morphtarget.hpp"
#include "skeletalanimation.hpp"
#include "vrmsemantic.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

namespace Pelican {

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
    PrimitiveViewVisibility view_visibility = PrimitiveViewVisibility::both;
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

struct ModelTemplate {
    using PrimitiveRefInfo = ModelPrimitiveRefInfo;

    struct MaterialPrimitives {
        GlobalMaterialId material;
        std::vector<PrimitiveRefInfo> primitives;
        std::uint32_t source_material_index = noSourceMaterialIndex;
    };

    struct NamedMaterial {
        std::string name;
        GlobalMaterialId material;
    };

    std::vector<MaterialPrimitives> material_primitives;
    std::vector<NamedMaterial> named_materials;
    std::shared_ptr<SkeletalModelData> skeletal;
    std::shared_ptr<const MorphTargetLayout> morph_targets;
    std::shared_ptr<const SourceMaterialInitialValueTable> material_initial_values;
    std::shared_ptr<const VrmSemanticData> vrm_semantic;
    std::shared_ptr<ModelGpuResources> gpu_resources;
    ModelAssetId asset_id{};
    std::uint64_t content_revision = 1;
    std::uint64_t compatibility_revision = 0;
};

using PrimitiveMaterialResolver =
    std::function<std::optional<GlobalMaterialId>(const PrimitiveMaterialBinding &binding)>;

// A non-empty mapping is a whole-model contract: every loaded primitive must
// occur exactly once. Fragment loads cannot satisfy that contract and are
// rejected instead of silently applying a partial mapping.
void applyPrimitiveMaterialBindings(
    ModelTemplate &model, const PrimitiveMaterialBindingDocument &document,
    std::string_view model_name, std::optional<std::string_view> fragment = std::nullopt,
    PrimitiveMaterialResolver resolve_material = {});

} // namespace Pelican
