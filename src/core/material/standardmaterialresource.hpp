#pragma once

#include "../build_features.hpp"
#include "../container.hpp"
#include "material.hpp"
namespace Pelican {

DECLARE_MODULE(StandardMaterialResource) {
    ShaderBundleId std_vert, vat_vert, std_frag;
    GlobalTextureId tex_transparent, tex_white, tex_black;
    GlobalTextureId tex_metallic_roughness_default, tex_normal_default;
    GlobalTextureId tex_emissive_default;
    GlobalMaterialId mat_transparent;

  public:
    StandardMaterialResource();

    ShaderBundleId standardVertShader() const { return std_vert; };
    ShaderBundleId vatVertShader() const {
#if PELICAN_WITH_VAT
        return vat_vert;
#else
        throwBuildFeatureDisabled("PELICAN_WITH_VAT", "vat vertex shader is unavailable");
#endif
    };
    ShaderBundleId standardFragShader() const { return std_frag; };
    GlobalTextureId transparentTexture() const { return tex_transparent; };
    GlobalTextureId whiteTexture() const { return tex_white; };
    GlobalTextureId blackTexture() const { return tex_black; };
    GlobalTextureId metallicRoughnessDefaultTexture() const { return tex_metallic_roughness_default; };
    GlobalTextureId normalDefaultTexture() const { return tex_normal_default; };
    GlobalTextureId emissiveDefaultTexture() const { return tex_emissive_default; };
    GlobalMaterialId standardTransparentMaterial() const { return mat_transparent; };
};

} // namespace Pelican
