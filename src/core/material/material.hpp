#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../shader/shader.hpp"
#include <span>
#include <vulkan/vulkan.hpp>

namespace Pelican {

PELICAN_DEFINE_HANDLE(GlobalMaterialId, int);
PELICAN_DEFINE_HANDLE(GlobalTextureId, int);

struct MaterialInfo {
    GlobalShaderId vert_shader, frag_shader;
    GlobalTextureId base_color_texture;
    GlobalTextureId metallic_roughness_texture;
    GlobalTextureId normal_texture;
};

} // namespace Pelican