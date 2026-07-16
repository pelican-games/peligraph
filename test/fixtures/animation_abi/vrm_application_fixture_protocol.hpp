#pragma once

#include <cstdint>

struct VrmApplicationFixtureResult {
    std::uint32_t status;
    std::uint32_t expression_count;
    std::uint32_t morph_weight_count;
    std::uint32_t material_override_count;
    std::uint32_t diagnostic_count;
    std::uint32_t diagnostic_code;
    float happy_weight;
    char expression_name[32];
    char material_color_type[32];
};
