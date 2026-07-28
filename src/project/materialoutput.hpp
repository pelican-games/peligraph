#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// Logical fragment-output types. The physical render-target format may use a
// different bit width (for example vec4 -> R8G8B8A8_UNORM), but its numeric
// class must agree with this declaration.
enum class MaterialOutputType {
    floating,
    vec2,
    vec3,
    vec4,
    integer,
    ivec2,
    ivec3,
    ivec4,
    unsigned_integer,
    uvec2,
    uvec3,
    uvec4,
};

enum class MaterialOutputNumericClass {
    floating,
    signed_integer,
    unsigned_integer,
};

// Built-in sources are conveniences, not engine-owned G-buffer slots. A
// render strategy can choose any ordered set and use custom for packed or
// project-specific encodings.
enum class MaterialOutputSource {
    custom,
    surface_base_color,
    surface_normal,
    surface_normal_encoded,
    surface_material,
    input_world_position,
    surface_emissive,
    lighting_scene_color,
};

struct MaterialOutputField {
    std::string name;
    MaterialOutputType type = MaterialOutputType::vec4;
    MaterialOutputSource source = MaterialOutputSource::custom;

    bool operator==(const MaterialOutputField &) const = default;
};

struct MaterialOutputSchema {
    std::string name;
    std::vector<MaterialOutputField> outputs;

    bool operator==(const MaterialOutputSchema &) const = default;
};

std::string_view materialOutputTypeName(MaterialOutputType type);
std::optional<MaterialOutputType>
materialOutputTypeFromName(std::string_view name);
MaterialOutputNumericClass
materialOutputNumericClass(MaterialOutputType type);
std::string_view materialOutputSourceName(MaterialOutputSource source);
std::optional<MaterialOutputSource>
materialOutputSourceFromName(std::string_view name);

// Parses the public pelican.material_outputs v1 declaration. The order of
// outputs is the shader location order and is therefore ABI-significant.
MaterialOutputSchema parseMaterialOutputSchema(
    const nlohmann::json &declaration,
    std::string_view context = "material_outputs");
void validateMaterialOutputSchema(
    const MaterialOutputSchema &schema,
    std::string_view context = "material_outputs");

// Stable, human-readable identity used across route compilation, shader
// caching, hot reload, and pipeline compatibility checks.
std::string materialOutputSchemaFingerprint(
    const MaterialOutputSchema &schema);
nlohmann::json materialOutputSchemaToJson(
    const MaterialOutputSchema &schema);

} // namespace Pelican
