#include "engineresources.hpp"

#include <array>
#include <sstream>
#include <stdexcept>

#include "battery/embed.hpp"

namespace Pelican {

namespace {

#if PELICAN_WITH_VAT
constexpr size_t registeredResourceCount = 30;
#else
constexpr size_t registeredResourceCount = 29;
#endif

constexpr std::array<std::string_view, registeredResourceCount> registered_ids{
    "bloom_blur_h.frag.spv",
    "bloom_blur_v.frag.spv",
    "bloom_composite.frag.spv",
    "bloom_highpass.frag.spv",
    "debug_texture.frag.spv",
    "default_config.json",
    "default.frag.spv",
    "default.vert.spv",
    "debug_draw.frag.spv",
    "debug_draw.vert.spv",
    "features/debug_draw.json",
    "features/gpu_timing.json",
    "features/hdr.json",
    "fullscreen.frag.spv",
    "fullscreen.vert.spv",
    "shader_lab_base_lit.frag.spv",
    "shader_lab_bloom_composite.frag.spv",
    "shader_lab_bloom_threshold.frag.spv",
    "shader_lab_blur_h.frag.spv",
    "shader_lab_blur_v.frag.spv",
    "shader_lab_gltf_lighting.frag.spv",
    "shader_lab_hello.frag.spv",
    "shader_lab_present.frag.spv",
    "ssao.frag.spv",
    "ssao_blur.frag.spv",
    "tonemap.frag",
    "tonemap.vert",
    "ui.frag.spv",
    "ui.vert.spv",
#if PELICAN_WITH_VAT
    "vat.vert.spv",
#endif
};

} // namespace

std::optional<std::string_view> engineResource(std::string_view id) {
#define PELICAN_ENGINE_RESOURCE(resource_id)                     \
    if (id == resource_id) {                                     \
        static const auto embedded = b::embed<resource_id>();    \
        static const std::string resource{embedded.data(), embedded.length()}; \
        return std::string_view{resource};                       \
    }

    PELICAN_ENGINE_RESOURCE("bloom_blur_h.frag.spv")
    PELICAN_ENGINE_RESOURCE("bloom_blur_v.frag.spv")
    PELICAN_ENGINE_RESOURCE("bloom_composite.frag.spv")
    PELICAN_ENGINE_RESOURCE("bloom_highpass.frag.spv")
    PELICAN_ENGINE_RESOURCE("debug_texture.frag.spv")
    if (id == "default_config.json") {
        static const std::string default_config = b::embed<"default_config.json">().str();
        return std::string_view{default_config};
    }
    PELICAN_ENGINE_RESOURCE("default.frag.spv")
    PELICAN_ENGINE_RESOURCE("default.vert.spv")
    PELICAN_ENGINE_RESOURCE("debug_draw.frag.spv")
    PELICAN_ENGINE_RESOURCE("debug_draw.vert.spv")
    if (id == "features/debug_draw.json") {
        static const std::string feature = b::embed<"features/debug_draw.json">().str();
        return std::string_view{feature};
    }
    if (id == "features/gpu_timing.json") {
        static const std::string feature = b::embed<"features/gpu_timing.json">().str();
        return std::string_view{feature};
    }
    PELICAN_ENGINE_RESOURCE("features/hdr.json")
    PELICAN_ENGINE_RESOURCE("fullscreen.frag.spv")
    PELICAN_ENGINE_RESOURCE("fullscreen.vert.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_base_lit.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_bloom_composite.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_bloom_threshold.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_blur_h.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_blur_v.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_gltf_lighting.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_hello.frag.spv")
    PELICAN_ENGINE_RESOURCE("shader_lab_present.frag.spv")
    PELICAN_ENGINE_RESOURCE("ssao.frag.spv")
    PELICAN_ENGINE_RESOURCE("ssao_blur.frag.spv")
    PELICAN_ENGINE_RESOURCE("tonemap.frag")
    PELICAN_ENGINE_RESOURCE("tonemap.vert")
    PELICAN_ENGINE_RESOURCE("ui.frag.spv")
    PELICAN_ENGINE_RESOURCE("ui.vert.spv")
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
