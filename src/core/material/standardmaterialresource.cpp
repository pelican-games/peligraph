#include "standardmaterialresource.hpp"
#include "../../project/materiallowering.hpp"
#include "../../project/surfaceformat.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../shader/shaderlibrary.hpp"
#include "battery/embed.hpp"
#include "materialcontainer.hpp"
#include <algorithm>
#include <stdexcept>

namespace Pelican {

StandardMaterialResource::StandardMaterialResource() {
    auto &mat_con = GET_MODULE(MaterialContainer);
    auto &shader_library = GET_MODULE(ShaderLibrary);

    const auto vert_shader = b::embed<"default.vert.spv">();
    std_vert = shader_library.loadFromBytes(vert_shader.length(), vert_shader.data(), "default.vert.spv");

    const auto skinned_vert_shader = b::embed<"skinned.vert.spv">();
    skinned_vert = shader_library.loadFromBytes(skinned_vert_shader.length(), skinned_vert_shader.data(),
                                                "skinned.vert.spv");

#if PELICAN_WITH_VAT
    const auto vat_vert_shader = b::embed<"vat.vert.spv">();
    vat_vert = shader_library.loadFromBytes(vat_vert_shader.length(), vat_vert_shader.data(), "vat.vert.spv");
#endif

    const auto frag_shader = b::embed<"default.frag.spv">();
    std_frag = shader_library.loadFromBytes(frag_shader.length(), frag_shader.data(), "default.frag.spv");

    const auto output_schema =
        GET_MODULE(RenderingPassContainer)
            .materialOutputSchema(
                MaterialRouteClass::
                    deferred_geometry,
                std::nullopt);
    if (output_schema) {
#if PELICAN_RUNTIME_SHADER_COMPILER
        constexpr std::string_view source_name =
            "generated://standard_material.surface";
        const auto surface = parseSurfaceFormat(
            R"surface(//! pelican.surface v1
//! language: glsl

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
}
)surface",
            source_name);
        const auto lowered =
            lowerSurfaceDefaults(
                surface, source_name);
        const auto generated =
            shader_library
                .loadFromSurfaceForMaterial(
                    surface, source_name,
                    lowered,
                    {"PELICAN_COMPACT_STANDARD_VERTEX_ABI"});
        // The built-in standard/skinned/VAT vertex shaders intentionally use
        // the compact six-varying ABI. The generated fragment retains that
        // input ABI while adopting the project-selected output schema.
        std_frag = generated.fragment;
#else
        throw std::runtime_error(
            "project material_outputs requires the runtime "
            "shader compiler for the standard material");
#endif
    }

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

    uint8_t texdata_gray[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_gray[i * 4 + 0] = 128;
        texdata_gray[i * 4 + 1] = 128;
        texdata_gray[i * 4 + 2] = 128;
        texdata_gray[i * 4 + 3] = 255;
    }
    tex_gray = mat_con.registerTexture(
        vk::Extent3D(4, 4, 1), texdata_gray);

    // Roughness=1.0 (G=255), Metallic=0.0 (B=0). glTF leaves R undefined.
    uint8_t texdata_metallic_roughness[4 * 16];
    for (int i = 0; i < 16; i++) {
        texdata_metallic_roughness[i * 4 + 0] = 255; // unused
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

    tex_emissive_default = tex_black;

    mat_transparent = mat_con.registerMaterial(MaterialInfo{
        .vert_shader = std_vert,
        .frag_shader = std_frag,
        .base_color_texture = tex_transparent,
        .metallic_roughness_texture = tex_metallic_roughness_default,
        .normal_texture = tex_normal_default,
        .emissive_texture = tex_emissive_default,
        .occlusion_texture = tex_white,
    });
}

GlobalTextureId StandardMaterialResource::defaultTexture(MaterialDummyTexture fallback) const {
    switch (fallback) {
    case MaterialDummyTexture::white:
        return tex_white;
    case MaterialDummyTexture::flat_normal:
        return tex_normal_default;
    case MaterialDummyTexture::black:
        return tex_black;
    }
    throw std::runtime_error("unknown material dummy texture");
}

ShaderBundleId
StandardMaterialResource::gltfFragmentShader(
    const SurfaceFormatDocument &surface,
    std::string_view surface_reference,
    const LoweredMaterial &lowered,
    std::vector<std::string> shader_defines) {
    shader_defines.emplace_back(
        "PELICAN_COMPACT_STANDARD_VERTEX_ABI");
    shader_defines.insert(
        shader_defines.end(),
        lowered.defines.begin(),
        lowered.defines.end());
    std::ranges::sort(shader_defines);
    shader_defines.erase(
        std::unique(
            shader_defines.begin(),
            shader_defines.end()),
        shader_defines.end());

    const auto cached = std::find_if(
        gltf_fragment_shaders.begin(),
        gltf_fragment_shaders.end(),
        [&](const auto &entry) {
            return entry.surface ==
                       surface_reference &&
                   entry.route == lowered.route &&
                   entry.defines == shader_defines;
        });
    if (cached != gltf_fragment_shaders.end()) {
        return cached->fragment;
    }

    auto additional_defines = shader_defines;
    for (const auto &define : lowered.defines) {
        const auto found = std::find(
            additional_defines.begin(),
            additional_defines.end(),
            define);
        if (found != additional_defines.end()) {
            additional_defines.erase(found);
        }
    }
    const auto shaders =
        GET_MODULE(ShaderLibrary)
            .loadFromSurfaceForMaterial(
                surface, surface_reference,
                lowered,
                std::move(additional_defines));
    gltf_fragment_shaders.push_back(
        GltfFragmentShader{
            .surface =
                std::string{surface_reference},
            .route = lowered.route,
            .defines = std::move(shader_defines),
            .fragment = shaders.fragment,
        });
    return shaders.fragment;
}

} // namespace Pelican
