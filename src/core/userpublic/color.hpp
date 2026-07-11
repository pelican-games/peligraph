#pragma once

#include <glm/glm.hpp>

namespace Pelican {

constexpr float colorClamp01(float value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

constexpr float fifthRoot(float value) {
    if (value <= 0.0f) {
        return 0.0f;
    }
    float root = 1.0f;
    for (int i = 0; i < 32; ++i) {
        const float root2 = root * root;
        const float root4 = root2 * root2;
        root = (4.0f * root + value / root4) / 5.0f;
    }
    return root;
}

constexpr float colorPow24(float value) {
    const float value2 = value * value;
    const float value4 = value2 * value2;
    const float value8 = value4 * value4;
    return fifthRoot(value8 * value4);
}

constexpr float srgbToLinear(float value) {
    const float c = colorClamp01(value);
    return c <= 0.04045f ? c / 12.92f : colorPow24((c + 0.055f) / 1.055f);
}

constexpr glm::vec4 srgb(float red, float green, float blue, float alpha = 1.0f) {
    return glm::vec4{srgbToLinear(red), srgbToLinear(green), srgbToLinear(blue), alpha};
}

} // namespace Pelican
