#include "renderfeatureparameteraccess.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>

namespace Pelican {

const CompiledRenderFeatureParameter *
findRenderFeatureParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name) noexcept {
    const auto found = std::find_if(
        feature.parameters.begin(),
        feature.parameters.end(),
        [name](
            const CompiledRenderFeatureParameter
                &parameter) {
            return parameter.name == name;
        });
    return found != feature.parameters.end()
               ? &*found
               : nullptr;
}

std::uint32_t renderFeatureUnsignedParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name,
    std::uint32_t fallback) {
    const auto *parameter =
        findRenderFeatureParameter(
            feature, name);
    if (parameter == nullptr) {
        return fallback;
    }
    if (const auto *value =
            std::get_if<std::uint64_t>(
                &parameter->value)) {
        if (*value <=
            std::numeric_limits<
                std::uint32_t>::max()) {
            return static_cast<
                std::uint32_t>(*value);
        }
    }
    if (const auto *value =
            std::get_if<std::int64_t>(
                &parameter->value)) {
        if (*value >= 0 &&
            static_cast<std::uint64_t>(
                *value) <=
                std::numeric_limits<
                    std::uint32_t>::max()) {
            return static_cast<
                std::uint32_t>(*value);
        }
    }
    throw std::runtime_error(
        "render feature '" + feature.feature +
        "' parameter '" + std::string{name} +
        "' is not an unsigned 32-bit integer");
}

float renderFeatureFloatParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name,
    float fallback) {
    const auto *parameter =
        findRenderFeatureParameter(
            feature, name);
    if (parameter == nullptr) {
        return fallback;
    }
    double value = 0.0;
    if (const auto *floating =
            std::get_if<double>(
                &parameter->value)) {
        value = *floating;
    } else if (const auto *integer =
                   std::get_if<std::int64_t>(
                       &parameter->value)) {
        value =
            static_cast<double>(*integer);
    } else if (const auto *integer =
                   std::get_if<std::uint64_t>(
                       &parameter->value)) {
        value =
            static_cast<double>(*integer);
    } else {
        throw std::runtime_error(
            "render feature '" +
            feature.feature +
            "' parameter '" +
            std::string{name} +
            "' is not numeric");
    }
    if (!std::isfinite(value) ||
        value <
            -static_cast<double>(
                std::numeric_limits<
                    float>::max()) ||
        value >
            static_cast<double>(
                std::numeric_limits<
                    float>::max())) {
        throw std::runtime_error(
            "render feature '" +
            feature.feature +
            "' parameter '" +
            std::string{name} +
            "' is outside the finite float range");
    }
    return static_cast<float>(value);
}

bool renderFeatureBoolParameter(
    const CompiledRenderFeatureInstance
        &feature,
    std::string_view name,
    bool fallback) {
    const auto *parameter =
        findRenderFeatureParameter(
            feature, name);
    if (parameter == nullptr) {
        return fallback;
    }
    if (const auto *value =
            std::get_if<bool>(
                &parameter->value)) {
        return *value;
    }
    throw std::runtime_error(
        "render feature '" + feature.feature +
        "' parameter '" + std::string{name} +
        "' is not boolean");
}

} // namespace Pelican
