#pragma once

#include <cstdint>
#include <string>
#include "../details/schema/structfieldschema.hpp"

namespace Pelican {

struct AnimationParameterOverride {
    std::string parameter;
    double value{};
    static constexpr auto schema = structFields(
        componentPolicy("animation_parameter"),
        required(field<&AnimationParameterOverride::parameter>("parameter")),
        required(field<&AnimationParameterOverride::value>("value")));
    bool operator==(const AnimationParameterOverride &) const = default;
};

struct AnimationComponent {
    std::string clip;
    double speed = 1.0;
    std::uint8_t loop = 1;
    double start_time = 0.0;
    // Select one source. Graph states own their playback clocks.
    std::string graph;
    std::optional<List<AnimationParameterOverride>> parameters;

};

} // namespace Pelican
