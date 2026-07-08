#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace Pelican {

enum class MaterialParamKind {
    scalar,
    vec2,
    vec3,
    vec4,
};

struct MaterialParamValue {
    MaterialParamKind kind = MaterialParamKind::scalar;
    std::array<double, 4> values{};
};

struct MaterialParam {
    std::string name;
    MaterialParamValue value;
};

struct MaterialBase {
    std::array<double, 4> base_color_factor{1.0, 1.0, 1.0, 1.0};
    std::optional<std::string> base_color_texture;
    double metallic_factor = 1.0;
    double roughness_factor = 1.0;
    std::optional<std::string> metallic_roughness_texture;
    std::optional<std::string> normal_texture;
    std::optional<std::string> occlusion_texture;
    std::array<double, 3> emissive_factor{0.0, 0.0, 0.0};
    std::optional<std::string> emissive_texture;
};

struct MaterialDefinition {
    std::string name;
    MaterialBase base;
    std::optional<std::string> shader;
    std::vector<std::string> defines;
    std::vector<MaterialParam> params;
};

struct MaterialFormatDocument {
    std::vector<MaterialDefinition> materials;
    std::vector<std::string> warnings;
};

MaterialFormatDocument parseMaterialFormatJson(const nlohmann::json &document);

} // namespace Pelican
