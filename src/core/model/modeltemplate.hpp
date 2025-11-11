#pragma once

namespace Pelican {

using ModelTemplateId = int;

struct ModelTemplate {
    struct PrimitiveRefInfo {
        uint32_t index_count;
        uint32_t index_offset;
        uint32_t vert_offset;
    };

    struct MaterialPrimitives {
        int material;
        std::vector<PrimitiveRefInfo> primitives;
    };

    std::vector<MaterialPrimitives> material_primitives;
};

} // namespace Pelican