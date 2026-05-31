#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../shader/shader.hpp"
#include <span>
#include <vulkan/vulkan.hpp>

namespace Pelican {

PELICAN_DEFINE_HANDLE(GlobalMaterialId, int);
PELICAN_DEFINE_HANDLE(GlobalTextureId, int);

inline constexpr int invalidMaterialIdValue = -1;

inline constexpr GlobalMaterialId invalidMaterialId() {
    return GlobalMaterialId{invalidMaterialIdValue};
}

inline constexpr bool isValidMaterialId(GlobalMaterialId material_id) {
    return material_id.value >= 0;
}

struct MaterialInfo {
    GlobalShaderId vert_shader, frag_shader;
    GlobalTextureId base_color_texture;
    GlobalTextureId metallic_roughness_texture;
    GlobalTextureId normal_texture;
    GlobalTextureId emissive_texture;
};

} // namespace Pelican
