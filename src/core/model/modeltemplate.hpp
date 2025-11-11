#pragma once

#include "../material/material.hpp"

namespace Pelican {

using ModelTemplateId = int;

struct ModelTemplate {
    struct PrimitiveRefInfo {
        uint32_t index_count;
        uint32_t index_offset;
        int32_t vert_offset;
    };

    struct MaterialPrimitives {
        GlobalMaterialId material;
        std::vector<PrimitiveRefInfo> primitives;
    };

    std::vector<MaterialPrimitives> material_primitives;
};

} // namespace Pelican