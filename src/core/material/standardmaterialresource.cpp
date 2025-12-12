#include "standardmaterialresource.hpp"
#include "../shader/shadercontainer.hpp"
#include "battery/embed.hpp"
#include "materialcontainer.hpp"

namespace Pelican {

StandardMaterialResource::StandardMaterialResource() {
    auto &mat_con = GET_MODULE(MaterialContainer);
    auto &shader_con = GET_MODULE(ShaderContainer);

    const auto vert_shader = b::embed<"default.vert.spv">();
    std_vert = shader_con.registerShader(vert_shader.length(), vert_shader.data());

    const auto frag_shader = b::embed<"default.frag.spv">();
    std_frag = shader_con.registerShader(frag_shader.length(), frag_shader.data());

    uint8_t texdata_transparent[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_transparent[i * 4 + 0] = 0;
        texdata_transparent[i * 4 + 1] = 0;
        texdata_transparent[i * 4 + 2] = 0;
        texdata_transparent[i * 4 + 3] = 0;
    }
    tex_transparent = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_transparent);

    uint8_t texdata_white[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_white[i * 4 + 0] = 255;
        texdata_white[i * 4 + 1] = 255;
        texdata_white[i * 4 + 2] = 255;
        texdata_white[i * 4 + 3] = 255;
    }
    tex_white = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_white);

    uint8_t texdata_black[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_black[i * 4 + 0] = 0;
        texdata_black[i * 4 + 1] = 0;
        texdata_black[i * 4 + 2] = 0;
        texdata_black[i * 4 + 3] = 255;
    }
    tex_black = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_black);

    // Roughness=1.0 (G=255), Metallic=0.0 (B=0), AO=1.0 (R=255)
    uint8_t texdata_metallic_roughness[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_metallic_roughness[i * 4 + 0] = 255; // occlusion
        texdata_metallic_roughness[i * 4 + 1] = 255; // roughness
        texdata_metallic_roughness[i * 4 + 2] = 0;   // metallic
        texdata_metallic_roughness[i * 4 + 3] = 255;
    }
    tex_metallic_roughness_default = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_metallic_roughness);

    // Normal map default (0.5, 0.5, 1.0)
    uint8_t texdata_normal[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_normal[i * 4 + 0] = 128;
        texdata_normal[i * 4 + 1] = 128;
        texdata_normal[i * 4 + 2] = 255;
        texdata_normal[i * 4 + 3] = 255;
    }
    tex_normal_default = mat_con.registerTexture(vk::Extent3D(4, 4, 1), texdata_normal);

    mat_transparent = mat_con.registerMaterial(MaterialInfo{
        .vert_shader = std_vert,
        .frag_shader = std_frag,
        .base_color_texture = tex_transparent,
        .metallic_roughness_texture = tex_metallic_roughness_default,
        .normal_texture = tex_normal_default,
    });
}

} // namespace Pelican
