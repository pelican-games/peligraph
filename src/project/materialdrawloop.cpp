#include "materialdrawloop.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace Pelican {

void throwMaterialDrawLoopRefusal(std::string_view code,
                                  const nlohmann::json &fields) {
    throw std::runtime_error(std::string{code} + ": " + fields.dump());
}

std::string materialDrawSubjectKey(
    std::string_view name, std::string_view source_document,
    std::string_view source_asset,
    std::optional<std::uint32_t> source_material_index) {
    if (!source_document.empty()) {
        return std::string{source_document} + "#" + std::string{name};
    }
    if (source_material_index) {
        return std::string{source_asset} + "#material[" +
               std::to_string(*source_material_index) + "]";
    }
    return std::string{name};
}

bool materialRouteIsTransparent(MaterialRouteClass route) noexcept {
    return route == MaterialRouteClass::forward_transparent;
}

void validateMaterialDrawStep(const MaterialDrawStep &step,
                              const MaterialDrawSubject &subject,
                              std::string_view node,
                              bool runtime_shader_compiler) {
    switch (step.kind) {
    case MaterialDrawStepKind::material:
        return;
    case MaterialDrawStepKind::variant:
        if (!std::binary_search(subject.variants.begin(),
                                subject.variants.end(), step.variant)) {
            throwMaterialDrawLoopRefusal(
                materialDrawVariantMissingCode,
                {{"node", node},
                 {"material", subject.key},
                 {"variant", step.variant}});
        }
        return;
    case MaterialDrawStepKind::override_pipeline:
        break;
    }
    if (step.render_state.blend) {
        const bool blends =
            *step.render_state.blend != SurfaceBlendMode::opaque;
        if (blends != materialRouteIsTransparent(subject.route)) {
            throwMaterialDrawLoopRefusal(
                materialDrawPhaseCrossingCode,
                {{"node", node}, {"material", subject.key}});
        }
    }
    if (!step.defines.empty() && !runtime_shader_compiler) {
        throwMaterialDrawLoopRefusal(
            materialDrawOverrideNeedsCompilerCode, {{"node", node}});
    }
}

SurfaceRenderState applyMaterialDrawRenderStateOverride(
    SurfaceRenderState base,
    const MaterialDrawRenderStateOverride &override_state) {
    if (override_state.blend) base.blend = *override_state.blend;
    if (override_state.cull) base.cull = *override_state.cull;
    if (override_state.depth_test) {
        base.depth_test = *override_state.depth_test;
    }
    if (override_state.depth_write) {
        base.depth_write = *override_state.depth_write;
    }
    if (override_state.depth_compare) {
        base.depth_compare = *override_state.depth_compare;
    }
    return base;
}

std::string materialDrawOverrideKey(const MaterialDrawStep &step) {
    std::ostringstream key;
    const auto optional_int = [&](const auto &value) {
        if (value) {
            key << static_cast<int>(*value);
        } else {
            key << '-';
        }
        key << ';';
    };
    optional_int(step.render_state.blend);
    optional_int(step.render_state.cull);
    optional_int(step.render_state.depth_test);
    optional_int(step.render_state.depth_write);
    optional_int(step.render_state.depth_compare);
    auto defines = step.defines;
    std::sort(defines.begin(), defines.end());
    defines.erase(std::unique(defines.begin(), defines.end()),
                  defines.end());
    for (const auto &define : defines) {
        key << define.size() << ':' << define;
    }
    return key.str();
}

} // namespace Pelican
