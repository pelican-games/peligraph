#include "passfieldownership.hpp"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

using namespace std::literals;

constexpr std::array material_fields{
    "material_range"sv,
    "material_filter"sv,
    "material_variant"sv,
    "material_contract"sv,
    "gpu_draw_source"sv,
    "material_outputs"sv,
    "material_output_states"sv,
    "screen_inputs"sv,
    "surface_resources"sv,
    "material_resources"sv,
};
constexpr std::array fullscreen_fields{
    "input_sampling"sv,
    "resource_ports"sv,
    "shader"sv,
    "push_constants"sv,
    "uses_light_data"sv,
    "implementation"sv,
};
constexpr std::array raster_fields{
    "resource_ports"sv,
    "draw"sv,
    "raster_state"sv,
    "shader"sv,
};
constexpr std::array shader_field{"shader"sv};
constexpr std::array canonical_anchor_fields{"anchor"sv};
constexpr std::array snapshot_copy_fields{
    "source"sv,
    "destination"sv,
    "snapshot"sv,
    "snapshot_after"sv,
};

constexpr std::array fullscreen_authoring_projection_fields{
    "name"sv,
    "type"sv,
    "input"sv,
    "output"sv,
    "shader"sv,
};

constexpr std::array ownership_table{
    PassFieldOwnershipEntry{RenderPassType::material, "material"sv,
                            material_fields},
    PassFieldOwnershipEntry{RenderPassType::fullscreen, "fullscreen"sv,
                            fullscreen_fields},
    PassFieldOwnershipEntry{RenderPassType::raster, "raster"sv,
                            raster_fields},
    PassFieldOwnershipEntry{RenderPassType::output_transform,
                            "output_transform"sv, fullscreen_fields},
    PassFieldOwnershipEntry{RenderPassType::debug_draw, "debug_draw"sv,
                            shader_field},
    PassFieldOwnershipEntry{RenderPassType::gizmo, "gizmo"sv,
                            shader_field},
    PassFieldOwnershipEntry{RenderPassType::debug_text, "debug_text"sv,
                            shader_field},
    PassFieldOwnershipEntry{RenderPassType::shadow_depth, "shadow_depth"sv,
                            shader_field},
    PassFieldOwnershipEntry{RenderPassType::velocity, "velocity"sv,
                            shader_field},
    PassFieldOwnershipEntry{RenderPassType::picking, "picking"sv,
                            shader_field},
    PassFieldOwnershipEntry{RenderPassType::ui, "ui"sv, {}},
    PassFieldOwnershipEntry{RenderPassType::imgui, "imgui"sv, {}, true},
    PassFieldOwnershipEntry{RenderPassType::canonical_anchor,
                            "canonical_anchor"sv,
                            canonical_anchor_fields},
    PassFieldOwnershipEntry{RenderPassType::snapshot_copy,
                            "snapshot_copy"sv,
                            snapshot_copy_fields},
};

std::string passName(const nlohmann::json &pass) {
    const auto found = pass.find("name");
    return found != pass.end() && found->is_string()
               ? found->get<std::string>()
               : "<unnamed>";
}

bool ownsField(const PassFieldOwnershipEntry &entry,
               std::string_view field) {
    return std::find(entry.fields.begin(), entry.fields.end(), field) !=
           entry.fields.end();
}

} // namespace

std::span<const PassFieldOwnershipEntry> passFieldOwnershipTable() {
    return ownership_table;
}

std::span<const std::string_view> passAuthoringProjectionFields(
    RenderPassType type) {
    if (type == RenderPassType::fullscreen) {
        return fullscreen_authoring_projection_fields;
    }
    throw std::runtime_error(
        "No pass authoring projection schema for type '" +
        std::string{renderPassTypeName(type)} + "'");
}

RenderPassType validatePassFieldOwnership(
    const nlohmann::json &pass,
    PassFieldOwnershipCapabilities capabilities,
    std::string_view context) {
    if (!pass.is_object()) {
        throw std::runtime_error(std::string{context} +
                                 " entry must be an object");
    }
    const auto name = passName(pass);
    const auto type_value = pass.find("type");
    if (type_value == pass.end() || !type_value->is_string()) {
        throw std::runtime_error("Pass '" + name +
                                 "' field 'type' must be a string");
    }
    const auto type_name = type_value->get<std::string>();
    const auto found = std::find_if(
        ownership_table.begin(), ownership_table.end(),
        [&](const auto &entry) {
            return entry.type_name == type_name &&
                   (!entry.requires_imgui || capabilities.imgui_enabled);
        });
    if (found == ownership_table.end()) {
        throw std::runtime_error("Pass '" + name +
                                 "' has unknown type '" + type_name + "'");
    }

    if (pass.contains("needs_projection_matrix")) {
        throw std::runtime_error(
            "Pass field needs_projection_matrix is deprecated; use push_constants: projection_view: " +
            name);
    }

    for (const auto &owner : ownership_table) {
        for (const auto field : owner.fields) {
            if (pass.contains(field) && !ownsField(*found, field)) {
                throw std::runtime_error(
                    "Pass '" + name + "' type '" + type_name +
                    "' does not own field '" + std::string{field} + "'");
            }
        }
    }
    return found->type;
}

std::string_view renderPassTypeName(RenderPassType type) {
    const auto found = std::find_if(
        ownership_table.begin(), ownership_table.end(),
        [type](const auto &entry) { return entry.type == type; });
    if (found == ownership_table.end()) {
        throw std::runtime_error("Unknown render pass type enum");
    }
    return found->type_name;
}

} // namespace Pelican
