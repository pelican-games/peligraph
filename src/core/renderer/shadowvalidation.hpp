#pragma once
#include "../userpublic/userdataerror.hpp"
#include "../renderingpass/renderfeatureparameteraccess.hpp"
namespace Pelican {
inline void requireShadowDistance(float near_distance, float far_distance) {
    if (far_distance <= near_distance + 1.0e-4f)
        throw UserDataError("directional shadow max_distance does not reach beyond the main-view near plane");
}
inline void requireCameraShadowDistance(float near_distance, const CompiledRenderPipeline &pipeline) {
    for (const auto &feature : pipeline.feature_instances)
        if (feature.feature == "shadow_directional" &&
            renderFeatureUnsignedParameter(feature, "cascade_count", 1) > 1)
            requireShadowDistance(near_distance, renderFeatureFloatParameter(feature, "max_distance", 100.0f));
}
}
