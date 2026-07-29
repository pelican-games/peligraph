#pragma once

#include "../../project/renderpipeline.hpp"

#include <cstdint>
#include <string_view>

namespace Pelican {

const CompiledRenderFeatureParameter *
findRenderFeatureParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name) noexcept;

std::uint32_t renderFeatureUnsignedParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name,
    std::uint32_t fallback);

float renderFeatureFloatParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name,
    float fallback);

bool renderFeatureBoolParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name,
    bool fallback);

} // namespace Pelican
