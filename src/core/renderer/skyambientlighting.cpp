#include "skyambientlighting.hpp"

#include "../renderingpass/renderfeatureparameteraccess.hpp"
#include "../../project/renderpipeline.hpp"

#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

float requiredParameter(
    const CompiledRenderFeatureInstance &feature,
    std::string_view name) {
    if (findRenderFeatureParameter(
            feature, name) == nullptr) {
        throw std::runtime_error(
            "render feature '" +
            feature.feature +
            "' is missing required runtime parameter '" +
            std::string{name} + "'");
    }
    return renderFeatureFloatParameter(
        feature, name, 0.0f);
}

} // namespace

SkyAmbientLighting resolveSkyAmbientLighting(
    const CompiledRenderPipeline
        &pipeline) {
    const CompiledRenderFeatureInstance
        *selected = nullptr;
    for (const auto &feature :
         pipeline.feature_instances) {
        if (feature.feature !=
            skyAmbientRenderFeatureName) {
            continue;
        }
        if (selected != nullptr) {
            throw std::runtime_error(
                "render pipeline contains more than one '" +
                std::string{
                    skyAmbientRenderFeatureName} +
                "' feature instance");
        }
        selected = &feature;
    }
    if (selected == nullptr) {
        return {};
    }

    SkyAmbientLighting result{
        .color =
            {
                requiredParameter(
                    *selected, "color_r"),
                requiredParameter(
                    *selected, "color_g"),
                requiredParameter(
                    *selected, "color_b"),
            },
        .ambient_intensity =
            requiredParameter(
                *selected,
                "ambient_intensity"),
        .sky_intensity =
            requiredParameter(
                *selected,
                "sky_intensity"),
    };
    if (result.color.r < 0.0f ||
        result.color.g < 0.0f ||
        result.color.b < 0.0f ||
        result.ambient_intensity < 0.0f ||
        result.sky_intensity < 0.0f) {
        throw std::runtime_error(
            "render feature '" +
            selected->feature +
            "' sky/ambient values must be non-negative");
    }
    return result;
}

} // namespace Pelican
