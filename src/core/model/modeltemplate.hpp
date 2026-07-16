#pragma once

#include "../handle.hpp"
#include "../material/material.hpp"
#include "skeletalanimation.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace Pelican {

using ModelTemplateId = int;

PELICAN_DEFINE_HANDLE(ModelAssetId, std::uint64_t)

inline constexpr bool isValidModelAssetId(ModelAssetId id) noexcept { return id.value != 0; }

struct ModelPrimitiveRefInfo {
    uint32_t index_count;
    uint32_t index_offset;
    int32_t vert_offset;
    bool skinned = false;
};

// Exact suballocations owned by one model generation. Keeping this metadata
// next to the template lets a rejected reload return its candidate ranges and
// lets a successful reload retire only the previous generation.
struct ModelGeometryAllocation {
    ModelPrimitiveRefInfo primitive;
    uint32_t vertex_count = 0;
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
    };

    std::vector<MaterialPrimitives> material_primitives;
    std::shared_ptr<SkeletalModelData> skeletal;
    std::shared_ptr<ModelGpuResources> gpu_resources;
    ModelAssetId asset_id{};
    std::uint64_t content_revision = 1;
    std::uint64_t compatibility_revision = 0;
};

} // namespace Pelican
