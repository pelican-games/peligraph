#pragma once

#include <charconv>
#include <cmath>
#include <limits>
#include <type_traits>
#include <nlohmann/json.hpp>

namespace Pelican::internal {
// JSON stores doubles. Round the decimal representation while the source's
// float type is still known, before putting the number into that storage.
template <class Float>
nlohmann::ordered_json authoredJsonNumber(Float value) {
    if (!std::isfinite(value)) return nullptr;
    if (value == Float{0}) return static_cast<double>(value);
    char buffer[128];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    const auto number = nlohmann::ordered_json::parse(buffer, result.ptr).get<double>();
    if constexpr (std::is_same_v<Float, float>) {
        constexpr auto limit = static_cast<double>(std::numeric_limits<float>::max());
        // The existing reader checks the exact F32 bound before conversion.
        // Its shortest decimal can lie just outside that bound.
        if (number < -limit || number > limit) return static_cast<double>(value);
    }
    return number;
}
} // namespace Pelican::internal
