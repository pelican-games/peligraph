#version 450
#extension GL_GOOGLE_include_directive : enable

#include "pelican_frame.glsl"

#ifndef PELICAN_FEATURE_TAA
#error PELICAN_FEATURE_TAA must be defined for the TAA resolve shader
#endif
#ifndef PELICAN_FEATURE_TAA_ALPHA
#error PELICAN_FEATURE_TAA_ALPHA must be defined for the TAA resolve shader
#endif
#ifndef PELICAN_FEATURE_TAA_DISOCCLUSION_TAU
#error PELICAN_FEATURE_TAA_DISOCCLUSION_TAU must be defined for the TAA resolve shader
#endif
#ifndef PELICAN_FEATURE_TAA_DEPTH_EPSILON
#error PELICAN_FEATURE_TAA_DEPTH_EPSILON must be defined for the TAA resolve shader
#endif

layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D sceneColorSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D velocitySampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 2) uniform sampler2D depthSampler;
layout(set = PELICAN_SET_PASS_INPUT, binding = 3) uniform sampler2D historySampler;

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

float linearizeDepth(float depth, vec2 uv) {
    vec4 viewPosition = inverse(pelicanFrame.projection) * vec4(uv * 2.0 - 1.0, depth, 1.0);
    return -viewPosition.z / viewPosition.w;
}

void currentNeighborhood(vec2 uv, out vec3 minimumColor, out vec3 maximumColor) {
    vec2 textureExtent = vec2(textureSize(sceneColorSampler, 0));
    vec2 texelSize = 1.0 / textureExtent;
    vec2 firstCenter = texelSize * 0.5;
    vec2 lastCenter = vec2(1.0) - firstCenter;
    minimumColor = vec3(3.402823466e+38);
    maximumColor = vec3(-3.402823466e+38);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 sampleUV = clamp(uv + vec2(x, y) * texelSize, firstCenter, lastCenter);
            vec3 sampleColor = texture(sceneColorSampler, sampleUV).rgb;
            minimumColor = min(minimumColor, sampleColor);
            maximumColor = max(maximumColor, sampleColor);
        }
    }
}

void main() {
    vec4 current = texture(sceneColorSampler, inUV);
    bool valid = pelicanFrame.temporal_reset_epoch ==
                 pelicanFrame.previous_temporal_reset_epoch;

    vec2 velocityUV = texture(velocitySampler, inUV).xy;
    vec2 historyUV = inUV - velocityUV;
    valid = valid && all(greaterThanEqual(historyUV, vec2(0.0))) &&
            all(lessThanEqual(historyUV, vec2(1.0)));

    if (valid) {
        float currentDepth = linearizeDepth(texture(depthSampler, inUV).r, inUV);
        float reprojectedDepth = linearizeDepth(texture(depthSampler, historyUV).r, historyUV);
        float relativeDepthDifference =
            abs(reprojectedDepth - currentDepth) /
            max(abs(currentDepth), PELICAN_FEATURE_TAA_DEPTH_EPSILON);
        valid = relativeDepthDifference <= PELICAN_FEATURE_TAA_DISOCCLUSION_TAU;
    }

    if (!valid) {
        outColor = current;
        return;
    }

    vec3 minimumColor;
    vec3 maximumColor;
    currentNeighborhood(inUV, minimumColor, maximumColor);
    vec3 clampedHistory = clamp(texture(historySampler, historyUV).rgb,
                                minimumColor, maximumColor);
    outColor = vec4(mix(clampedHistory, current.rgb, PELICAN_FEATURE_TAA_ALPHA),
                    current.a);
}
