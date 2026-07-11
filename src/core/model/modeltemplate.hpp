#pragma once

#include "../material/material.hpp"
#include "skeletalanimation.hpp"
#include <memory>

namespace Pelican {

using ModelTemplateId = int;

struct ModelTemplate {
    struct PrimitiveRefInfo {
        uint32_t index_count;
        uint32_t index_offset;
        int32_t vert_offset;
        bool skinned = false;
    };

    struct MaterialPrimitives {
        GlobalMaterialId material;
        std::vector<PrimitiveRefInfo> primitives;
    };

    std::vector<MaterialPrimitives> material_primitives;
    std::shared_ptr<SkeletalModelData> skeletal;
};

} // namespace Pelican
