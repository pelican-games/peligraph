#pragma once

#include "../build_features.hpp"
#include "../container.hpp"
#include "material.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

DECLARE_MODULE(StandardMaterialResource) {
    struct GltfFragmentShader {
        std::string surface;
        MaterialRouteClass route =
            MaterialRouteClass::deferred_geometry;
        std::vector<std::string> defines;
        ShaderBundleId fragment;
    };

    ShaderBundleId std_vert, skinned_vert, vat_vert, std_frag;
    GlobalTextureId tex_transparent, tex_white, tex_black,
        tex_gray;
    GlobalTextureId tex_metallic_roughness_default, tex_normal_default;
    GlobalTextureId tex_emissive_default;
    GlobalMaterialId mat_transparent;
    std::vector<GltfFragmentShader>
        gltf_fragment_shaders;

  public:
    StandardMaterialResource();

    ShaderBundleId standardVertShader() const { return std_vert; };
    ShaderBundleId skinnedVertShader() const { return skinned_vert; };
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
    GlobalTextureId grayTexture() const { return tex_gray; };
    GlobalTextureId metallicRoughnessDefaultTexture() const { return tex_metallic_roughness_default; };
    GlobalTextureId normalDefaultTexture() const { return tex_normal_default; };
    GlobalTextureId emissiveDefaultTexture() const { return tex_emissive_default; };
    GlobalTextureId defaultTexture(MaterialDummyTexture fallback) const;
    ShaderBundleId gltfFragmentShader(
        const SurfaceFormatDocument &surface,
        std::string_view surface_reference,
        const LoweredMaterial &lowered,
        std::vector<std::string> shader_defines);
    GlobalMaterialId standardTransparentMaterial() const { return mat_transparent; };
};

} // namespace Pelican
