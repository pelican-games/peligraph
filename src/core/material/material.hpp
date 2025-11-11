#pragma once

#include "../container.hpp"
#include <map>
#include <span>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct GlobalMaterialId {
    int value;
    bool operator<(GlobalMaterialId o) const { return value < o.value; }
};

struct GlobalShaderId {
    int value;
    bool operator<(GlobalShaderId o) const { return value < o.value; }
};

struct GlobalTextuerId {
    int value;
    bool operator<(GlobalTextuerId o) const { return value < o.value; }
};

struct MaterialInfo {
    GlobalShaderId vert_shader, frag_shader;
    GlobalTextuerId base_color_texture;
};

} // namespace Pelican