#pragma once

#include "surfaceformat.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Pelican {

struct MaterialValue {
    std::string name;
    SurfaceParamValue value;
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
    std::optional<std::string> surface;
    std::vector<MaterialValue> values;
};

struct MaterialFormatDocument {
    std::vector<MaterialDefinition> materials;
    std::vector<std::string> warnings;
};

using MaterialSurfaceCatalog = std::unordered_map<std::string, SurfaceFormatDocument>;

MaterialFormatDocument parseMaterialFormatJson(const nlohmann::json &document);
MaterialFormatDocument parseMaterialFormatJson(const nlohmann::json &document,
                                               const MaterialSurfaceCatalog &surfaces);

} // namespace Pelican
