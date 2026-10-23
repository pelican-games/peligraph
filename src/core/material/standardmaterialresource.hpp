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
    // G-4: the fragment and route of the engine's built-in materials. With
    // `material_route_default: "forward"` these are a runtime-compiled forward
    // build of the standard fragment and forward_opaque; otherwise std_frag and
    // deferred_geometry, as before.
    ShaderBundleId builtin_frag;
    MaterialRouteClass builtin_route =
        MaterialRouteClass::deferred_geometry;
    std::vector<GltfFragmentShader>
        gltf_fragment_shaders;

  public:
    StandardMaterialResource();
    // Static so built-ins may use it while this module is still constructing.
    // The cache has the module lifetime, including on a subsequent engine start.
    static ShaderBundleId errorFragmentShader(
        MaterialRouteClass route, const std::optional<MaterialOutputSchema> &schema, bool compact_vertex);

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
    GlobalTextureId occlusionDefaultTexture() const { return tex_white; };
    GlobalTextureId defaultTexture(MaterialDummyTexture fallback) const;
    ShaderBundleId gltfFragmentShader(
        const SurfaceFormatDocument &surface,
        std::string_view surface_reference,
        const LoweredMaterial &lowered,
        std::vector<std::string> shader_defines);
    GlobalMaterialId standardTransparentMaterial() const { return mat_transparent; };
    // G-4: a built-in material (the transparent fallback, the sequence
    // player's sphere, a glTF primitive without a material) with the standard
    // vertex shader, the default textures, the route the project's
    // material_route_default selects, and a `pelican.builtin.<suffix>` name.
    MaterialInfo builtinMaterialInfo(std::string_view suffix,
                                     GlobalTextureId base_color_texture) const;
};

} // namespace Pelican
