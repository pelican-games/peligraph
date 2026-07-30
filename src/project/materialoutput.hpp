#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
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

// Fixed-function names deliberately live above Vulkan. Render strategies can
// author portable attachment state while the device compiler remains free to
// lower it to another graphics API.
enum class MaterialOutputBlendFactor {
    zero,
    one,
    source_color,
    one_minus_source_color,
    destination_color,
    one_minus_destination_color,
    source_alpha,
    one_minus_source_alpha,
    destination_alpha,
    one_minus_destination_alpha,
    source_alpha_saturate,
};

enum class MaterialOutputBlendOperation {
    add,
    subtract,
    reverse_subtract,
    minimum,
    maximum,
};

struct MaterialOutputBlendEquation {
    MaterialOutputBlendFactor source =
        MaterialOutputBlendFactor::one;
    MaterialOutputBlendFactor destination =
        MaterialOutputBlendFactor::zero;
    MaterialOutputBlendOperation operation =
        MaterialOutputBlendOperation::add;

    bool operator==(
        const MaterialOutputBlendEquation &) const = default;
};

struct MaterialOutputBlendState {
    bool enabled = false;
    MaterialOutputBlendEquation color;
    MaterialOutputBlendEquation alpha;

    bool operator==(
        const MaterialOutputBlendState &) const = default;
};

inline constexpr std::uint8_t materialOutputWriteRed = 1u << 0;
inline constexpr std::uint8_t materialOutputWriteGreen = 1u << 1;
inline constexpr std::uint8_t materialOutputWriteBlue = 1u << 2;
inline constexpr std::uint8_t materialOutputWriteAlpha = 1u << 3;
inline constexpr std::uint8_t materialOutputWriteRgba =
    materialOutputWriteRed | materialOutputWriteGreen |
    materialOutputWriteBlue | materialOutputWriteAlpha;

// Sparse pass-owned overrides keyed by the logical output field name. Missing
// state inherits the material's render_state; missing members inherit only
// that member. The vector is canonicalized to schema order by the parser.
struct MaterialOutputAttachmentState {
    std::string output;
    std::optional<MaterialOutputBlendState> blend;
    std::optional<std::uint8_t> write_mask;

    bool operator==(
        const MaterialOutputAttachmentState &) const = default;
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

std::string_view materialOutputBlendFactorName(
    MaterialOutputBlendFactor factor);
std::optional<MaterialOutputBlendFactor>
materialOutputBlendFactorFromName(std::string_view name);
std::string_view materialOutputBlendOperationName(
    MaterialOutputBlendOperation operation);
std::optional<MaterialOutputBlendOperation>
materialOutputBlendOperationFromName(std::string_view name);

// Shared raster fixed-function helpers. Material output declarations use
// these through their named attachment-state syntax, while generic raster
// passes use the same portable values through an ordered attachment array.
MaterialOutputBlendState parseMaterialOutputBlendState(
    const nlohmann::json &declaration,
    std::string_view context = "blend");
nlohmann::json materialOutputBlendStateToJson(
    const MaterialOutputBlendState &state);
std::uint8_t parseMaterialOutputWriteMask(
    const nlohmann::json &declaration,
    std::string_view context = "write_mask");
std::string materialOutputWriteMaskName(
    std::uint8_t write_mask);

// Parses a material_output_states object. Keys are output field names from the
// supplied schema. Blend accepts the convenience presets opaque/blend/
// additive or an explicit {color, alpha} equation. write_mask is any
// channel subset such as "rgba", "rgb", "r", or "none".
std::vector<MaterialOutputAttachmentState>
parseMaterialOutputAttachmentStates(
    const nlohmann::json &declaration,
    const MaterialOutputSchema &schema,
    std::string_view context = "material_output_states");
void validateMaterialOutputAttachmentStates(
    const std::vector<MaterialOutputAttachmentState> &states,
    const MaterialOutputSchema &schema,
    std::string_view context = "material_output_states");
std::string materialOutputAttachmentStatesFingerprint(
    const std::vector<MaterialOutputAttachmentState> &states,
    const MaterialOutputSchema &schema);
nlohmann::json materialOutputAttachmentStatesToJson(
    const std::vector<MaterialOutputAttachmentState> &states,
    const MaterialOutputSchema &schema);

} // namespace Pelican
