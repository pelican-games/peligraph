#pragma once

#include "../container.hpp"
#include "material.hpp"
namespace Pelican {

DECLARE_MODULE(StandardMaterialResource) {
    GlobalShaderId std_vert, std_frag;
    GlobalTextureId tex_transparent, tex_white, tex_black;
    GlobalTextureId tex_metallic_roughness_default, tex_normal_default;
    GlobalMaterialId mat_transparent;

  public:
    StandardMaterialResource();

    GlobalShaderId standardVertShader() const { return std_vert; };
    GlobalShaderId standardFragShader() const { return std_frag; };
    GlobalTextureId transparentTexture() const { return tex_transparent; };
    GlobalTextureId whiteTexture() const { return tex_white; };
    GlobalTextureId blackTexture() const { return tex_black; };
    GlobalTextureId metallicRoughnessDefaultTexture() const { return tex_metallic_roughness_default; };
    GlobalTextureId normalDefaultTexture() const { return tex_normal_default; };
    GlobalMaterialId standardTransparentMaterial() const { return mat_transparent; };
};

} // namespace Pelican
