#include "engineresources.hpp"

#include <array>
#include <sstream>
#include <stdexcept>

#include "battery/embed.hpp"

namespace Pelican {

namespace {

constexpr auto registered_ids =
    std::to_array<std::string_view>({
    "bloom_blur_h.frag",
    "bloom_blur_h.frag.spv",
    "bloom_blur_v.frag",
    "bloom_blur_v.frag.spv",
    "bloom_composite.frag",
    "bloom_composite.frag.spv",
    "bloom_highpass.frag",
    "bloom_highpass.frag.spv",
    "debug_texture.frag.spv",
    "default_config.json",
    "default.frag.spv",
    "default.vert.spv",
    "render_pipelines/hybrid_v1.json",
    "debug_draw.frag.spv",
    "debug_draw.vert.spv",
    "debug_text.frag.spv",
    "debug_text.vert.spv",
    "debug_text.frag",
    "debug_text.vert",
    "debug_text_font.json",
    "debug_text_font.png",
    "features/debug_draw.json",
    "features/debug_text.json",
    "features/ui.json",
    "features/sprite.json",
    "features/gpu_timing.json",
    "features/clustered_lighting.json",
    "features/cube_capture.json",
    "features/hdr.json",
    "features/planar_reflection.json",
    "features/shadow_directional.json",
    "features/sky_ambient.json",
    "features/taa.json",
    "features/velocity.json",
    "fullscreen.frag",
    "fullscreen.frag.spv",
    "fullscreen.vert",
    "fullscreen.vert.spv",
    "scene_present.frag",
    "scene_present.frag.spv",
    "sky_ambient.frag",
    "sky_ambient.frag.spv",
    "output_transform.frag",
    "output_transform.frag.spv",
    "shaders/include/pelican_features.glsl",
    "shaders/include/pelican_frame.glsl",
    "shaders/include/pelican_view.glsl",
    "shaders/include/pelican_material.glsl",
    "shaders/include/pelican_material_instance.glsl",
    "shaders/include/pelican_sets.glsl",
    "shaders/include/pelican_surface_v1.glsl",
    "shaders/include/pelican_lighting_v1.glsl",
    "shaders/include/pelican_lighting_v2.glsl",
    "shaders/include/pelican_shadow_cascade.glsl",
    "shaders/include/pelican_skinning.glsl",
    "shaders/include/pelican_morph.glsl",
    "shaders/material/standard_lighting.glsl",
    "shaders/material/toon_lighting.glsl",
    "shaders/material/openpbr_lighting.glsl",
    "shaders/material/surface_v1.vert",
    "shaders/material/surface_v1.frag",
    "shaders/compute/clustered_light_select.comp",
#define PELICAN_OPTIONAL_ENGINE_RESOURCE(resource_id) resource_id,
#include "pelican_optional_engine_resources.inc"
#undef PELICAN_OPTIONAL_ENGINE_RESOURCE
    "surfaces/openpbr/manifest.json",
    "surfaces/openpbr/opaque_single.surface",
    "surfaces/openpbr/opaque_double.surface",
    "surfaces/openpbr/mask_single.surface",
    "surfaces/openpbr/mask_double.surface",
    "surfaces/openpbr/blend_single.surface",
    "surfaces/openpbr/blend_double.surface",
    "shadow_depth.vert",
    "shadow_depth.vert.spv",
    "skinned.vert.spv",
    "skinned_shadow_depth.vert.spv",
    "shader_lab_base_lit.frag.spv",
    "shader_lab_bloom_composite.frag.spv",
    "shader_lab_bloom_threshold.frag.spv",
    "shader_lab_blur_h.frag.spv",
    "shader_lab_blur_v.frag.spv",
    "shader_lab_gltf_lighting.frag.spv",
    "shader_lab_hello.frag.spv",
    "shader_lab_present.frag.spv",
    "ssao.frag",
    "ssao.frag.spv",
    "ssao_blur.frag",
    "ssao_blur.frag.spv",
    "taa_composite.frag",
    "taa_resolve.frag",
    "sprite.frag.spv",
    "sprite.vert.spv",
    "tonemap.frag",
    "tonemap.vert",
    "ui.frag.spv",
    "ui.vert.spv",
    "velocity.frag",
    "velocity.vert",
    "velocity_skinned.vert",
#if PELICAN_WITH_VAT
    "vat.vert.spv",
#endif
});

} // namespace

std::optional<std::string_view> engineResource(std::string_view id) {
#define PELICAN_ENGINE_RESOURCE(resource_id)                     \
    if (id == resource_id) {                                     \
        static const auto embedded = b::embed<resource_id>();    \
        static const std::string resource{embedded.data(), embedded.length()}; \
        return std::string_view{resource};                       \
    }

    PELICAN_ENGINE_RESOURCE("bloom_blur_h.frag")
    PELICAN_ENGINE_RESOURCE("bloom_blur_h.frag.spv")
    PELICAN_ENGINE_RESOURCE("bloom_blur_v.frag")
    PELICAN_ENGINE_RESOURCE("bloom_blur_v.frag.spv")
    PELICAN_ENGINE_RESOURCE("bloom_composite.frag")
    PELICAN_ENGINE_RESOURCE("bloom_composite.frag.spv")
    PELICAN_ENGINE_RESOURCE("bloom_highpass.frag")
    PELICAN_ENGINE_RESOURCE("bloom_highpass.frag.spv")
    PELICAN_ENGINE_RESOURCE("debug_texture.frag.spv")
    if (id == "default_config.json") {
        static const std::string default_config = b::embed<"default_config.json">().str();
        return std::string_view{default_config};
    }
    PELICAN_ENGINE_RESOURCE("default.frag.spv")
    PELICAN_ENGINE_RESOURCE("default.vert.spv")
    PELICAN_ENGINE_RESOURCE("render_pipelines/hybrid_v1.json")
    PELICAN_ENGINE_RESOURCE("debug_draw.frag.spv")
    PELICAN_ENGINE_RESOURCE("debug_draw.vert.spv")
    PELICAN_ENGINE_RESOURCE("debug_text.frag.spv")
    PELICAN_ENGINE_RESOURCE("debug_text.vert.spv")
    PELICAN_ENGINE_RESOURCE("debug_text.frag")
    PELICAN_ENGINE_RESOURCE("debug_text.vert")
    if (id == "debug_text_font.json") {
        static const std::string font = b::embed<"debug_text_font.json">().str();
        return std::string_view{font};
    }
    PELICAN_ENGINE_RESOURCE("debug_text_font.png")
    if (id == "features/debug_draw.json") {
        static const std::string feature = b::embed<"features/debug_draw.json">().str();
        return std::string_view{feature};
    }
    if (id == "features/debug_text.json") {
        static const std::string feature = b::embed<"features/debug_text.json">().str();
        return std::string_view{feature};
    }
    if (id == "features/ui.json") {
        static const std::string feature = b::embed<"features/ui.json">().str();
        return std::string_view{feature};
    }
    if (id == "features/sprite.json") {
        static const std::string feature = b::embed<"features/sprite.json">().str();
        return std::string_view{feature};
    }
    if (id == "features/gpu_timing.json") {
        static const std::string feature = b::embed<"features/gpu_timing.json">().str();
        return std::string_view{feature};
    }
    PELICAN_ENGINE_RESOURCE("features/clustered_lighting.json")
    PELICAN_ENGINE_RESOURCE("features/cube_capture.json")
    PELICAN_ENGINE_RESOURCE("features/hdr.json")
    PELICAN_ENGINE_RESOURCE("features/planar_reflection.json")
    PELICAN_ENGINE_RESOURCE("features/shadow_directional.json")
    PELICAN_ENGINE_RESOURCE("features/sky_ambient.json")
    PELICAN_ENGINE_RESOURCE("features/taa.json")
    PELICAN_ENGINE_RESOURCE("features/velocity.json")
    PELICAN_ENGINE_RESOURCE("fullscreen.frag")
    PELICAN_ENGINE_RESOURCE("fullscreen.frag.spv")
    PELICAN_ENGINE_RESOURCE("fullscreen.vert")
    PELICAN_ENGINE_RESOURCE("fullscreen.vert.spv")
    PELICAN_ENGINE_RESOURCE("scene_present.frag")
    PELICAN_ENGINE_RESOURCE("scene_present.frag.spv")
    PELICAN_ENGINE_RESOURCE("sky_ambient.frag")
    PELICAN_ENGINE_RESOURCE("sky_ambient.frag.spv")
    PELICAN_ENGINE_RESOURCE("output_transform.frag")
    PELICAN_ENGINE_RESOURCE("output_transform.frag.spv")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_features.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_frame.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_view.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_material.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_material_instance.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_sets.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_surface_v1.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_lighting_v1.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_lighting_v2.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_shadow_cascade.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_skinning.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/include/pelican_morph.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/material/standard_lighting.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/material/toon_lighting.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/material/openpbr_lighting.glsl")
    PELICAN_ENGINE_RESOURCE("shaders/material/surface_v1.vert")
    PELICAN_ENGINE_RESOURCE("shaders/material/surface_v1.frag")
    PELICAN_ENGINE_RESOURCE("shaders/compute/clustered_light_select.comp")
#define PELICAN_OPTIONAL_ENGINE_RESOURCE(resource_id) \
    PELICAN_ENGINE_RESOURCE(resource_id)
#include "pelican_optional_engine_resources.inc"
#undef PELICAN_OPTIONAL_ENGINE_RESOURCE
    PELICAN_ENGINE_RESOURCE("surfaces/openpbr/manifest.json")
    PELICAN_ENGINE_RESOURCE("surfaces/openpbr/opaque_single.surface")
    PELICAN_ENGINE_RESOURCE("surfaces/openpbr/opaque_double.surface")
    PELICAN_ENGINE_RESOURCE("surfaces/openpbr/mask_single.surface")
    PELICAN_ENGINE_RESOURCE("surfaces/openpbr/mask_double.surface")
    PELICAN_ENGINE_RESOURCE("surfaces/openpbr/blend_single.surface")
    PELICAN_ENGINE_RESOURCE("surfaces/openpbr/blend_double.surface")
    PELICAN_ENGINE_RESOURCE("shadow_depth.vert")
    PELICAN_ENGINE_RESOURCE("shadow_depth.vert.spv")
    PELICAN_ENGINE_RESOURCE("skinned.vert.spv")
    PELICAN_ENGINE_RESOURCE("skinned_shadow_depth.vert.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_base_lit.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_bloom_composite.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_bloom_threshold.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_blur_h.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_blur_v.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_gltf_lighting.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_hello.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_present.frag.spv")
    PELICAN_ENGINE_RESOURCE("ssao.frag")
    PELICAN_ENGINE_RESOURCE("ssao.frag.spv")
    PELICAN_ENGINE_RESOURCE("ssao_blur.frag")
    PELICAN_ENGINE_RESOURCE("ssao_blur.frag.spv")
    PELICAN_ENGINE_RESOURCE("taa_composite.frag")
    PELICAN_ENGINE_RESOURCE("taa_resolve.frag")
    PELICAN_ENGINE_RESOURCE("sprite.frag.spv")
    PELICAN_ENGINE_RESOURCE("sprite.vert.spv")
    PELICAN_ENGINE_RESOURCE("tonemap.frag")
    PELICAN_ENGINE_RESOURCE("tonemap.vert")
    PELICAN_ENGINE_RESOURCE("ui.frag.spv")
    PELICAN_ENGINE_RESOURCE("ui.vert.spv")
    PELICAN_ENGINE_RESOURCE("velocity.frag")
    PELICAN_ENGINE_RESOURCE("velocity.vert")
    PELICAN_ENGINE_RESOURCE("velocity_skinned.vert")
#if PELICAN_WITH_VAT
    PELICAN_ENGINE_RESOURCE("vat.vert.spv")
#endif
#undef PELICAN_ENGINE_RESOURCE

    return std::nullopt;
}

std::span<const std::string_view> registeredEngineResourceIds() {
    return std::span<const std::string_view>{registered_ids.data(), registered_ids.size()};
}

std::string registeredEngineResourceIdsMessage() {
    std::ostringstream stream;
    bool first = true;
    for (const auto id : registeredEngineResourceIds()) {
        if (!first) {
            stream << ", ";
        }
        stream << id;
        first = false;
    }
    return stream.str();
}

std::string engineResourceOrThrow(std::string_view id) {
    if (const auto resource = engineResource(id)) {
        return std::string{*resource};
    }

    throw std::runtime_error("Unknown engine resource id: " + std::string{id} +
                             ". Registered ids: " + registeredEngineResourceIdsMessage());
}

} // namespace Pelican
