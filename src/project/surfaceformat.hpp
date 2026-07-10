#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class SurfaceLanguage {
    glsl,
    hlsl,
    slang,
};

enum class SurfaceParamType {
    floating,
    vec2,
    vec3,
    vec4,
    integer,
};

struct SurfaceParamValue {
    SurfaceParamType type = SurfaceParamType::floating;
    std::array<double, 4> values{};
    std::int64_t integer_value = 0;
};

struct SurfaceParamDefinition {
    std::string name;
    SurfaceParamType type = SurfaceParamType::floating;
    SurfaceParamValue default_value;
    std::optional<double> min;
    std::optional<double> max;
    std::optional<std::string> hint;
};

struct SurfaceTextureDefinition {
    std::string name;
    std::string default_reference;
    std::string color_space;
};

struct SurfaceFormatDocument {
    SurfaceLanguage language = SurfaceLanguage::glsl;
    std::vector<SurfaceParamDefinition> params;
    std::vector<SurfaceTextureDefinition> textures;
    std::vector<std::string> screen_inputs;
    std::size_t code_offset = 0;
    std::string code;
    std::vector<std::string> warnings;
};

SurfaceFormatDocument parseSurfaceFormat(std::string_view source,
                                         std::string_view source_name = "<surface>");

std::string_view surfaceParamTypeName(SurfaceParamType type);

} // namespace Pelican
