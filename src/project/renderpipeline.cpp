#include "renderpipeline.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_set>

namespace Pelican {
namespace {

constexpr std::string_view preset_schema = "pelican.render_pipeline";
constexpr int supported_preset_version = 1;

void requireOnlyKeys(const nlohmann::json &object,
                     std::initializer_list<std::string_view> allowed,
                     std::string_view context) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end()) {
            throw std::runtime_error(std::string{context} + " has unknown key '" +
                                     it.key() + "'");
        }
    }
}

std::string requireString(const nlohmann::json &object, std::string_view key,
                          std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() || found->get<std::string>().empty()) {
        throw std::runtime_error(std::string{context} + " requires non-empty string " +
                                 std::string{key});
    }
    return found->get<std::string>();
}

void appendUniqueArray(nlohmann::json &destination, const nlohmann::json &source,
                       std::string_view field, std::string_view context,
                       bool strings_only) {
    if (!source.contains(field)) return;
    const auto &values = source.at(field);
    if (!values.is_array()) {
        throw std::runtime_error(std::string{context} + " " + std::string{field} +
                                 " must be an array");
    }
    if (!destination.contains(field)) destination[field] = nlohmann::json::array();
    auto &output = destination.at(field);
    if (!output.is_array()) {
        throw std::runtime_error("render pipeline preset config " + std::string{field} +
                                 " must be an array");
    }
    for (const auto &value : values) {
        if (strings_only && !value.is_string()) {
            throw std::runtime_error(std::string{context} + " " + std::string{field} +
                                     " entries must be strings");
        }
        if (std::find(output.begin(), output.end(), value) == output.end()) {
            output.push_back(value);
        }
    }
}

MaterialPassContract expectedContract(MaterialRouteClass route) {
    switch (route) {
    case MaterialRouteClass::deferred_geometry:
        return MaterialPassContract::deferred_geometry_v1;
    case MaterialRouteClass::forward_opaque:
        return MaterialPassContract::forward_opaque_v1;
    case MaterialRouteClass::forward_transparent:
        return MaterialPassContract::forward_transparent_v1;
    }
    throw std::runtime_error("unknown material route");
}

const nlohmann::json *findPass(const nlohmann::json &pass_set,
                              std::string_view pass_name) {
    if (!pass_set.is_object() || !pass_set.contains("passes") ||
        !pass_set.at("passes").is_array()) {
        throw std::runtime_error("rendering pass requires passes array");
    }
    const nlohmann::json *result = nullptr;
    for (const auto &pass : pass_set.at("passes")) {
        if (pass.is_object() && pass.value("name", std::string{}) == pass_name) {
            if (result != nullptr) {
                throw std::runtime_error("material routing pass name is ambiguous: " +
                                         std::string{pass_name});
            }
            result = &pass;
        }
    }
    return result;
}

} // namespace

std::string_view materialRouteClassName(MaterialRouteClass route) {
    switch (route) {
    case MaterialRouteClass::deferred_geometry: return "deferred_geometry";
    case MaterialRouteClass::forward_opaque: return "forward_opaque";
    case MaterialRouteClass::forward_transparent: return "forward_transparent";
    }
    return "unknown";
}

std::optional<MaterialRouteClass> materialRouteClassFromName(std::string_view name) {
    if (name == "deferred_geometry") return MaterialRouteClass::deferred_geometry;
    if (name == "forward_opaque") return MaterialRouteClass::forward_opaque;
    if (name == "forward_transparent") return MaterialRouteClass::forward_transparent;
    return std::nullopt;
}

std::string_view materialShaderContractName(MaterialShaderContract contract) {
    switch (contract) {
    case MaterialShaderContract::legacy_gbuffer_v1: return "legacy_gbuffer_v1";
    case MaterialShaderContract::gbuffer_v1: return "gbuffer_v1";
    case MaterialShaderContract::forward_scene_color_v1: return "forward_scene_color_v1";
    }
    return "unknown";
}

std::string_view materialPhaseName(MaterialPhase phase) {
    switch (phase) {
    case MaterialPhase::opaque: return "opaque";
    case MaterialPhase::transparent: return "transparent";
    }
    return "unknown";
}

std::string_view materialPassContractName(MaterialPassContract contract) {
    switch (contract) {
    case MaterialPassContract::legacy_gbuffer_v1: return "legacy_gbuffer_v1";
    case MaterialPassContract::deferred_geometry_v1: return "deferred_geometry_v1";
    case MaterialPassContract::forward_opaque_v1: return "forward_opaque_v1";
    case MaterialPassContract::forward_transparent_v1: return "forward_transparent_v1";
    }
    return "unknown";
}

std::optional<MaterialPassContract> materialPassContractFromName(std::string_view name) {
    if (name == "legacy_gbuffer_v1") return MaterialPassContract::legacy_gbuffer_v1;
    if (name == "deferred_geometry_v1") return MaterialPassContract::deferred_geometry_v1;
    if (name == "forward_opaque_v1") return MaterialPassContract::forward_opaque_v1;
    if (name == "forward_transparent_v1") return MaterialPassContract::forward_transparent_v1;
    return std::nullopt;
}

std::optional<MaterialRouteClass> materialPassRoute(MaterialPassContract contract) {
    switch (contract) {
    case MaterialPassContract::legacy_gbuffer_v1: return std::nullopt;
    case MaterialPassContract::deferred_geometry_v1:
        return MaterialRouteClass::deferred_geometry;
    case MaterialPassContract::forward_opaque_v1:
        return MaterialRouteClass::forward_opaque;
    case MaterialPassContract::forward_transparent_v1:
        return MaterialRouteClass::forward_transparent;
    }
    return std::nullopt;
}

MaterialShaderContract materialPassShaderContract(MaterialPassContract contract) {
    switch (contract) {
    case MaterialPassContract::legacy_gbuffer_v1:
        return MaterialShaderContract::legacy_gbuffer_v1;
    case MaterialPassContract::deferred_geometry_v1:
        return MaterialShaderContract::gbuffer_v1;
    case MaterialPassContract::forward_opaque_v1:
    case MaterialPassContract::forward_transparent_v1:
        return MaterialShaderContract::forward_scene_color_v1;
    }
    throw std::runtime_error("unknown material pass contract");
}

MaterialPhase materialPassPhase(MaterialPassContract contract) {
    return contract == MaterialPassContract::forward_transparent_v1
               ? MaterialPhase::transparent
               : MaterialPhase::opaque;
}

std::string_view materialRouteReasonName(MaterialRouteReason reason) {
    switch (reason) {
    case MaterialRouteReason::automatic_deferred_compatible:
        return "automatic_deferred_compatible";
    case MaterialRouteReason::automatic_openpbr_base: return "automatic_openpbr_base";
    case MaterialRouteReason::automatic_blended: return "automatic_blended";
    case MaterialRouteReason::automatic_screen_input: return "automatic_screen_input";
    case MaterialRouteReason::automatic_custom_brdf: return "automatic_custom_brdf";
    case MaterialRouteReason::automatic_custom_ambient: return "automatic_custom_ambient";
    case MaterialRouteReason::automatic_custom_lighting: return "automatic_custom_lighting";
    case MaterialRouteReason::explicit_deferred: return "explicit_deferred";
    case MaterialRouteReason::explicit_forward: return "explicit_forward";
    }
    return "unknown";
}

std::string_view deferredMaterialModelName(DeferredMaterialModel model) {
    switch (model) {
    case DeferredMaterialModel::standard_pbr_v1: return "standard_pbr_v1";
    case DeferredMaterialModel::openpbr_base_v1: return "openpbr_base_v1";
    }
    return "unknown";
}

bool materialPassAcceptsMaterial(
    MaterialPassContract pass_contract, std::string_view pass_name,
    MaterialRouteClass material_route,
    MaterialShaderContract material_shader_contract,
    const std::optional<std::string> &exact_pass) {
    if (exact_pass && *exact_pass != pass_name) return false;
    if (pass_contract == MaterialPassContract::legacy_gbuffer_v1) {
        return material_shader_contract == MaterialShaderContract::legacy_gbuffer_v1 ||
               material_shader_contract == MaterialShaderContract::gbuffer_v1;
    }
    const auto route = materialPassRoute(pass_contract);
    return route && *route == material_route &&
           materialPassShaderContract(pass_contract) == material_shader_contract;
}

RenderPipelinePresetResolution resolveRenderPipelinePreset(
    const nlohmann::json &authored_config,
    const RenderPipelinePresetLoader &load_preset_json) {
    if (!authored_config.is_object()) {
        throw std::runtime_error("Rendering config must be an object");
    }
    if (!authored_config.contains("pipeline")) {
        return {authored_config, std::nullopt};
    }

    const auto &pipeline = authored_config.at("pipeline");
    if (!pipeline.is_object()) {
        throw std::runtime_error("rendering config pipeline must be an object");
    }
    requireOnlyKeys(pipeline, {"preset"}, "rendering config pipeline");
    const auto reference = requireString(pipeline, "preset", "rendering config pipeline");
    if (!load_preset_json) {
        throw std::runtime_error("render pipeline preset loader is unavailable: " + reference);
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(load_preset_json(reference));
    } catch (const std::exception &error) {
        throw std::runtime_error("failed to load render pipeline preset '" + reference +
                                 "': " + error.what());
    }
    if (!document.is_object()) {
        throw std::runtime_error("render pipeline preset must be an object: " + reference);
    }
    requireOnlyKeys(document, {"schema", "version", "name", "config"},
                    "render pipeline preset '" + reference + "'");
    if (document.value("schema", std::string{}) != preset_schema) {
        throw std::runtime_error("render pipeline preset schema must be '" +
                                 std::string{preset_schema} + "': " + reference);
    }
    if (!document.contains("version") || !document.at("version").is_number_integer() ||
        document.at("version").get<int>() != supported_preset_version) {
        throw std::runtime_error("render pipeline preset version must be exactly 1: " +
                                 reference);
    }
    const auto name = requireString(document, "name", "render pipeline preset '" + reference + "'");
    if (!document.contains("config") || !document.at("config").is_object()) {
        throw std::runtime_error("render pipeline preset requires object config: " + reference);
    }
    auto resolved = document.at("config");
    if (resolved.contains("pipeline")) {
        throw std::runtime_error("render pipeline presets cannot recursively select a preset: " +
                                 reference);
    }

    // Keep v1 deterministic.  Structural overrides use an explicit eject/copy
    // workflow so a preset update cannot silently reinterpret a deep merge.
    requireOnlyKeys(authored_config,
                    {"pipeline", "features", "snapshots", "shader_defines"},
                    "rendering config using pipeline preset");
    appendUniqueArray(resolved, authored_config, "features", "rendering config", false);
    appendUniqueArray(resolved, authored_config, "shader_defines", "rendering config", true);
    if (authored_config.contains("snapshots")) {
        if (resolved.contains("snapshots")) {
            throw std::runtime_error(
                "rendering config cannot override preset snapshots; copy/eject the preset first");
        }
        if (!authored_config.at("snapshots").is_array()) {
            throw std::runtime_error("rendering config snapshots must be an array");
        }
        resolved["snapshots"] = authored_config.at("snapshots");
    }
    return {std::move(resolved), RenderPipelinePresetInfo{
                                         reference, name, supported_preset_version}};
}

nlohmann::json resolveMaterialRoutingTable(const nlohmann::json &composed_config) {
    if (!composed_config.contains("material_routing")) return nullptr;
    const auto &routing = composed_config.at("material_routing");
    if (!routing.is_object()) {
        throw std::runtime_error("material_routing must be an object");
    }
    requireOnlyKeys(routing, {"policy", "routes"}, "material_routing");
    const auto policy = requireString(routing, "policy", "material_routing");
    if (policy != "hybrid_auto_v1") {
        throw std::runtime_error("unsupported material_routing policy: " + policy);
    }
    if (!routing.contains("routes") || !routing.at("routes").is_object()) {
        throw std::runtime_error("material_routing requires object routes");
    }
    const auto &routes = routing.at("routes");
    requireOnlyKeys(routes,
                    {"deferred_geometry", "forward_opaque", "forward_transparent"},
                    "material_routing routes");
    constexpr std::array route_values{
        MaterialRouteClass::deferred_geometry,
        MaterialRouteClass::forward_opaque,
        MaterialRouteClass::forward_transparent,
    };
    std::unordered_set<std::string> selected_passes;
    nlohmann::json resolved_routes = nlohmann::json::object();
    for (const auto route : route_values) {
        const auto route_name = materialRouteClassName(route);
        const auto pass_name = requireString(routes, route_name, "material_routing routes");
        if (!selected_passes.insert(pass_name).second) {
            throw std::runtime_error("material_routing routes must select distinct passes: " +
                                     pass_name);
        }
        const auto expected = expectedContract(route);
        if (!composed_config.contains("rendering_passes") ||
            !composed_config.at("rendering_passes").is_array() ||
            composed_config.at("rendering_passes").empty()) {
            throw std::runtime_error("material_routing requires rendering_passes");
        }
        for (const auto &pass_set : composed_config.at("rendering_passes")) {
            const auto *pass = findPass(pass_set, pass_name);
            const auto set_name = pass_set.value("name", std::string{"<unnamed>"});
            if (pass == nullptr) {
                throw std::runtime_error("material_routing route '" +
                                         std::string{route_name} + "' pass '" + pass_name +
                                         "' is missing from rendering pass '" + set_name + "'");
            }
            if (pass->value("type", std::string{}) != "material") {
                throw std::runtime_error("material_routing route '" +
                                         std::string{route_name} + "' selects non-material pass '" +
                                         pass_name + "'");
            }
            if (!pass->contains("material_contract") ||
                !pass->at("material_contract").is_string()) {
                throw std::runtime_error("material_routing pass '" + pass_name +
                                         "' requires explicit material_contract");
            }
            const auto parsed = materialPassContractFromName(
                pass->at("material_contract").get<std::string>());
            if (!parsed || *parsed != expected) {
                throw std::runtime_error("material_routing route '" +
                                         std::string{route_name} + "' pass '" + pass_name +
                                         "' requires contract '" +
                                         std::string{materialPassContractName(expected)} + "'");
            }
        }
        resolved_routes[route_name] = {
            {"pass", pass_name},
            {"contract", materialPassContractName(expected)},
            {"shader_contract", materialShaderContractName(
                                    materialPassShaderContract(expected))},
            {"phase", materialPhaseName(materialPassPhase(expected))},
        };
    }
    return {
        {"policy", policy},
        {"routes", std::move(resolved_routes)},
    };
}

} // namespace Pelican
