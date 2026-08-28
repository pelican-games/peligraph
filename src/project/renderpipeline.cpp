#include "renderpipeline.hpp"
#include "featurecompose.hpp"
#include "passfieldownership.hpp"
#include "rasterpass.hpp"
#include "renderresourcename.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {
namespace {

constexpr std::string_view preset_schema = "pelican.render_pipeline";
constexpr int supported_preset_version = 1;

void validatePassFieldOwnershipInConfig(
    const nlohmann::json &config,
    PassFieldOwnershipCapabilities capabilities,
    std::string_view context) {
    const auto pass_sets = config.find("rendering_passes");
    if (pass_sets == config.end() || !pass_sets->is_array()) {
        return;
    }
    for (const auto &pass_set : *pass_sets) {
        if (!pass_set.is_object()) {
            continue;
        }
        const auto passes = pass_set.find("passes");
        if (passes == pass_set.end() || !passes->is_array()) {
            continue;
        }
        for (const auto &pass : *passes) {
            const auto type =
                validatePassFieldOwnership(pass, capabilities, context);
            if ((type == RenderPassType::fullscreen ||
                 type == RenderPassType::output_transform) &&
                pass.contains("raster_state")) {
                const auto name =
                    pass.contains("name") && pass.at("name").is_string()
                        ? pass.at("name").get<std::string>()
                        : std::string{"<unnamed>"};
                (void)parseFullscreenRasterFixedFunctionState(
                    pass, "Fullscreen pass '" + name + "'");
            }
        }
    }
}

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

void suffixRenderingPassNames(nlohmann::json &config,
                              std::string_view suffix) {
    if (suffix.empty()) return;
    for (auto &pass : config.at("rendering_passes")) {
        pass["name"] =
            pass.at("name").get<std::string>() + std::string{suffix};
    }
}

bool optionalBoolean(const nlohmann::json &object,
                     std::string_view key,
                     std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end()) return false;
    if (!found->is_boolean()) {
        throw std::runtime_error(
            std::string{context} + " " + std::string{key} +
            " must be a boolean");
    }
    return found->get<bool>();
}

std::uint64_t optionalUnsignedInteger(
    const nlohmann::json &object, std::string_view key,
    std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end()) return 0;
    if (found->is_number_unsigned()) {
        return found->get<std::uint64_t>();
    }
    if (found->is_number_integer()) {
        const auto value = found->get<std::int64_t>();
        if (value >= 0) return static_cast<std::uint64_t>(value);
    }
    throw std::runtime_error(
        std::string{context} + " " + std::string{key} +
        " must be an unsigned integer");
}

TargetPlanningPolicy compileTargetPlanningPolicy(
    const nlohmann::json &config,
    std::string_view graph_name_suffix) {
    TargetPlanningPolicy result;
    const auto declaration = config.find("target_planning");
    if (declaration == config.end()) return result;
    if (!declaration->is_object()) {
        throw std::runtime_error(
            "target_planning must be an object");
    }
    result.authored = true;
    requireOnlyKeys(
        *declaration, {"profile", "graphs", "diagnostics"},
        "target_planning");

    if (const auto profile = declaration->find("profile");
        profile != declaration->end()) {
        if (!profile->is_object()) {
            throw std::runtime_error(
                "target_planning profile must be an object");
        }
        requireOnlyKeys(
            *profile, {"kind", "seed"},
            "target_planning profile");
        const auto kind = requireString(
            *profile, "kind", "target_planning profile");
        if (kind == "optimized") {
            result.profile.kind =
                PlanningProfileKind::optimized;
        } else if (kind == "conservative_debug") {
            result.profile.kind =
                PlanningProfileKind::conservative_debug;
        } else if (kind == "hazard_stress") {
            result.profile.kind =
                PlanningProfileKind::hazard_stress;
        } else {
            throw std::runtime_error(
                "target_planning profile has unknown kind: " +
                kind);
        }
        if (profile->contains("seed") &&
            result.profile.kind !=
                PlanningProfileKind::hazard_stress) {
            throw std::runtime_error(
                "target_planning profile seed is only valid for "
                "hazard_stress");
        }
        result.profile.seed = optionalUnsignedInteger(
            *profile, "seed", "target_planning profile");
    }

    if (const auto diagnostics =
            declaration->find("diagnostics");
        diagnostics != declaration->end()) {
        if (!diagnostics->is_object()) {
            throw std::runtime_error(
                "target_planning diagnostics must be an object");
        }
        requireOnlyKeys(
            *diagnostics, {"strict_warnings"},
            "target_planning diagnostics");
        if (const auto warnings =
                diagnostics->find("strict_warnings");
            warnings != diagnostics->end()) {
            if (!warnings->is_array()) {
                throw std::runtime_error(
                    "target_planning diagnostics strict_warnings "
                    "must be an array");
            }
            for (const auto &warning : *warnings) {
                if (!warning.is_string() ||
                    warning.get_ref<const std::string &>().empty()) {
                    throw std::runtime_error(
                        "target_planning diagnostics strict_warnings "
                        "entries must be non-empty strings");
                }
                result.diagnostic_policy.strict_warning_ids.push_back(
                    warning.get<std::string>());
            }
            std::sort(
                result.diagnostic_policy.strict_warning_ids.begin(),
                result.diagnostic_policy.strict_warning_ids.end());
            if (std::adjacent_find(
                    result.diagnostic_policy.strict_warning_ids.begin(),
                    result.diagnostic_policy.strict_warning_ids.end()) !=
                result.diagnostic_policy.strict_warning_ids.end()) {
                throw std::runtime_error(
                    "target_planning diagnostics strict_warnings "
                    "must not contain duplicates");
            }
        }
    }

    const auto graphs = declaration->find("graphs");
    if (graphs == declaration->end()) return result;
    if (!graphs->is_object()) {
        throw std::runtime_error(
            "target_planning graphs must be an object");
    }
    result.graphs.reserve(graphs->size());
    for (auto graph = graphs->begin(); graph != graphs->end();
         ++graph) {
        if (graph.key().empty() || !graph.value().is_object()) {
            throw std::runtime_error(
                "target_planning graph names must be non-empty and "
                "values must be objects");
        }
        const auto context =
            "target_planning graph '" + graph.key() + "'";
        requireOnlyKeys(
            graph.value(),
            {"required_capabilities", "nodes", "resources"},
            context);
        PlanningGraphConstraints compiled{
            .graph = graph.key() +
                     std::string{graph_name_suffix},
        };

        if (const auto capabilities =
                graph->find("required_capabilities");
            capabilities != graph->end()) {
            if (!capabilities->is_array()) {
                throw std::runtime_error(
                    context +
                    " required_capabilities must be an array");
            }
            for (const auto &capability : *capabilities) {
                if (!capability.is_string() ||
                    capability
                        .get_ref<const std::string &>()
                        .empty()) {
                    throw std::runtime_error(
                        context +
                        " required_capabilities entries must be "
                        "non-empty strings");
                }
                compiled.required_capabilities.push_back(
                    capability.get<std::string>());
            }
            std::sort(
                compiled.required_capabilities.begin(),
                compiled.required_capabilities.end());
            if (std::adjacent_find(
                    compiled.required_capabilities.begin(),
                    compiled.required_capabilities.end()) !=
                compiled.required_capabilities.end()) {
                throw std::runtime_error(
                    context +
                    " required_capabilities must not contain "
                    "duplicates");
            }
        }

        if (const auto nodes = graph->find("nodes");
            nodes != graph->end()) {
            if (!nodes->is_object()) {
                throw std::runtime_error(
                    context + " nodes must be an object");
            }
            compiled.nodes.reserve(nodes->size());
            for (auto node = nodes->begin();
                 node != nodes->end(); ++node) {
                if (node.key().empty() ||
                    !node.value().is_object()) {
                    throw std::runtime_error(
                        context +
                        " node names must be non-empty and values "
                        "must be objects");
                }
                const auto node_context =
                    context + " node '" + node.key() + "'";
                requireOnlyKeys(
                    node.value(), {"serial", "isolate"},
                    node_context);
                compiled.nodes.push_back(
                    PlanningNodeConstraint{
                        .node = node.key(),
                        .serial = optionalBoolean(
                            node.value(), "serial", node_context),
                        .isolate = optionalBoolean(
                            node.value(), "isolate", node_context),
                    });
            }
        }

        if (const auto resources =
                graph->find("resources");
            resources != graph->end()) {
            if (!resources->is_object()) {
                throw std::runtime_error(
                    context + " resources must be an object");
            }
            compiled.resources.reserve(resources->size());
            for (auto resource = resources->begin();
                 resource != resources->end(); ++resource) {
                if (resource.key().empty() ||
                    !resource.value().is_object()) {
                    throw std::runtime_error(
                        context +
                        " resource names must be non-empty and "
                        "values must be objects");
                }
                const auto resource_context =
                    context + " resource '" + resource.key() + "'";
                requireOnlyKeys(
                    resource.value(), {"no_alias"},
                    resource_context);
                compiled.resources.push_back(
                    PlanningResourceConstraint{
                        .resource = resource.key(),
                        .no_alias = optionalBoolean(
                            resource.value(), "no_alias",
                            resource_context),
                    });
            }
        }
        result.graphs.push_back(std::move(compiled));
    }
    return result;
}

nlohmann::json targetPlanningPolicyToJson(
    const TargetPlanningPolicy &policy) {
    nlohmann::json profile{
        {"kind", planningProfileKindName(policy.profile.kind)},
    };
    if (policy.profile.kind ==
        PlanningProfileKind::hazard_stress) {
        profile["seed"] = policy.profile.seed;
    }
    nlohmann::json result{
        {"profile", std::move(profile)},
    };
    if (!policy.graphs.empty()) {
        auto graphs = nlohmann::json::object();
        for (const auto &graph : policy.graphs) {
            auto declaration = nlohmann::json::object();
            if (!graph.required_capabilities.empty()) {
                declaration["required_capabilities"] =
                    graph.required_capabilities;
            }
            if (!graph.nodes.empty()) {
                auto nodes = nlohmann::json::object();
                for (const auto &node : graph.nodes) {
                    nodes[node.node] = {
                        {"serial", node.serial},
                        {"isolate", node.isolate},
                    };
                }
                declaration["nodes"] = std::move(nodes);
            }
            if (!graph.resources.empty()) {
                auto resources = nlohmann::json::object();
                for (const auto &resource : graph.resources) {
                    resources[resource.resource] = {
                        {"no_alias", resource.no_alias},
                    };
                }
                declaration["resources"] =
                    std::move(resources);
            }
            graphs[graph.graph] = std::move(declaration);
        }
        result["graphs"] = std::move(graphs);
    }
    if (!policy.diagnostic_policy.strict_warning_ids.empty()) {
        result["diagnostics"] = {
            {"strict_warnings",
             policy.diagnostic_policy.strict_warning_ids},
        };
    }
    return result;
}

std::vector<VulkanTargetPlanPinPackage>
compileVulkanTargetPlanPins(
    const nlohmann::json &config,
    RenderPipelineGraphVariant variant) {
    const auto declaration =
        config.find("vulkan_plan_pins");
    if (declaration == config.end()) return {};
    if (!declaration->is_object()) {
        throw std::runtime_error(
            "vulkan_plan_pins must be an object");
    }
    requireOnlyKeys(
        *declaration, {"flat", "preview", "xr"},
        "vulkan_plan_pins");
    const auto variant_name =
        renderPipelineGraphVariantName(variant);
    const auto packages =
        declaration->find(variant_name);
    if (packages == declaration->end()) return {};
    if (!packages->is_array()) {
        throw std::runtime_error(
            "vulkan_plan_pins " +
            std::string{variant_name} +
            " must be an array");
    }

    std::vector<VulkanTargetPlanPinPackage> result;
    result.reserve(packages->size());
    for (const auto &document : *packages) {
        result.push_back(
            vulkanTargetPlanPinPackageFromJson(document));
    }
    std::sort(
        result.begin(), result.end(),
        [](const auto &left, const auto &right) {
            return left.graph < right.graph;
        });
    if (std::adjacent_find(
            result.begin(), result.end(),
            [](const auto &left, const auto &right) {
                return left.graph == right.graph;
            }) != result.end()) {
        throw std::runtime_error(
            "vulkan_plan_pins has duplicate packages for a graph");
    }
    return result;
}

std::vector<VulkanPhysicalFragmentPackage>
compileVulkanPhysicalFragments(
    const nlohmann::json &config,
    RenderPipelineGraphVariant variant) {
    const auto declaration =
        config.find("vulkan_physical_fragments");
    if (declaration == config.end()) return {};
    if (!declaration->is_object()) {
        throw std::runtime_error(
            "vulkan_physical_fragments must be an object");
    }
    requireOnlyKeys(
        *declaration, {"flat", "preview", "xr"},
        "vulkan_physical_fragments");
    const auto variant_name =
        renderPipelineGraphVariantName(variant);
    const auto packages =
        declaration->find(variant_name);
    if (packages == declaration->end()) return {};
    if (!packages->is_array()) {
        throw std::runtime_error(
            "vulkan_physical_fragments " +
            std::string{variant_name} +
            " must be an array");
    }

    std::vector<VulkanPhysicalFragmentPackage> result;
    result.reserve(packages->size());
    for (const auto &document : *packages) {
        result.push_back(
            vulkanPhysicalFragmentPackageFromJson(
                document));
    }
    std::sort(
        result.begin(), result.end(),
        [](const auto &left, const auto &right) {
            return left.graph < right.graph;
        });
    if (std::adjacent_find(
            result.begin(), result.end(),
            [](const auto &left, const auto &right) {
                return left.graph == right.graph;
            }) != result.end()) {
        throw std::runtime_error(
            "vulkan_physical_fragments has duplicate packages "
            "for a graph");
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
    requireOnlyKeys(pipeline, {"preset", "settings"},
                    "rendering config pipeline");
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
                    {"pipeline", "features", "snapshots", "shader_defines",
                     "draw_sort", "graph_transforms",
                     "render_strategy", "target_planning",
                     "vulkan_plan_pins",
                     "vulkan_physical_fragments", "xr"},
                    "rendering config using pipeline preset");
    appendUniqueArray(resolved, authored_config, "features", "rendering config", false);
    appendUniqueArray(resolved, authored_config, "shader_defines", "rendering config", true);
    appendUniqueArray(resolved, authored_config, "graph_transforms",
                      "rendering config", false);
    if (authored_config.contains("render_strategy")) {
        if (!authored_config.at("render_strategy").is_object()) {
            throw std::runtime_error(
                "rendering config render_strategy must be an object");
        }
        if (resolved.contains("render_strategy")) {
            throw std::runtime_error(
                "rendering config cannot override preset render_strategy; "
                "copy/eject the preset first");
        }
        resolved["render_strategy"] =
            authored_config.at("render_strategy");
    }
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
    if (authored_config.contains("draw_sort")) {
        if (!authored_config.at("draw_sort").is_object()) {
            throw std::runtime_error(
                "rendering config draw_sort must be an object");
        }
        resolved["draw_sort"] = authored_config.at("draw_sort");
    }
    if (authored_config.contains("target_planning")) {
        if (!authored_config.at("target_planning").is_object()) {
            throw std::runtime_error(
                "rendering config target_planning must be an object");
        }
        if (resolved.contains("target_planning")) {
            throw std::runtime_error(
                "rendering config cannot override preset target_planning; "
                "copy/eject the preset first");
        }
        resolved["target_planning"] =
            authored_config.at("target_planning");
    }
    if (authored_config.contains("vulkan_plan_pins")) {
        if (!authored_config.at("vulkan_plan_pins")
                 .is_object()) {
            throw std::runtime_error(
                "rendering config vulkan_plan_pins must be an object");
        }
        if (resolved.contains("vulkan_plan_pins")) {
            throw std::runtime_error(
                "rendering config cannot override preset "
                "vulkan_plan_pins; copy/eject the preset first");
        }
        resolved["vulkan_plan_pins"] =
            authored_config.at("vulkan_plan_pins");
    }
    if (authored_config.contains(
            "vulkan_physical_fragments")) {
        if (!authored_config
                 .at("vulkan_physical_fragments")
                 .is_object()) {
            throw std::runtime_error(
                "rendering config vulkan_physical_fragments "
                "must be an object");
        }
        if (resolved.contains(
                "vulkan_physical_fragments")) {
            throw std::runtime_error(
                "rendering config cannot override preset "
                "vulkan_physical_fragments; copy/eject the preset "
                "first");
        }
        resolved["vulkan_physical_fragments"] =
            authored_config.at(
                "vulkan_physical_fragments");
    }
    if (authored_config.contains("xr")) {
        if (!authored_config.at("xr").is_object()) {
            throw std::runtime_error(
                "rendering config xr must be an object");
        }
        resolved["xr"] = authored_config.at("xr");
    }
    if (pipeline.contains("settings")) {
        const auto &settings = pipeline.at("settings");
        if (!settings.is_object()) {
            throw std::runtime_error(
                "rendering config pipeline settings must be an object");
        }
        requireOnlyKeys(settings, {"msaa", "xr"},
                        "rendering config pipeline settings");
        if (settings.contains("msaa")) {
            if (!settings.at("msaa").is_object()) {
                throw std::runtime_error(
                    "rendering config pipeline settings msaa must be an object");
            }
            // The preset envelope is an authoring convenience. The expanded
            // config uses the same canonical compiler input as an ejected
            // verbose rendering config.
            resolved["multisampling"] = settings.at("msaa");
        }
        if (settings.contains("xr")) {
            if (authored_config.contains("xr")) {
                throw std::runtime_error(
                    "rendering config cannot define xr both at top level "
                    "and in pipeline settings");
            }
            if (!settings.at("xr").is_object()) {
                throw std::runtime_error(
                    "rendering config pipeline settings xr must be an object");
            }
            resolved["xr"] = settings.at("xr");
        }
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
        std::optional<MaterialOutputSchema>
            selected_output_schema;
        bool selected_output_schema_initialized = false;
        std::vector<MaterialOutputAttachmentState>
            selected_output_states;
        bool selected_output_states_initialized = false;
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
            std::optional<MaterialOutputSchema>
                pass_output_schema;
            if (pass->contains("material_outputs")) {
                pass_output_schema =
                    parseMaterialOutputSchema(
                        pass->at("material_outputs"),
                        "material_routing pass '" +
                            pass_name +
                            "' material_outputs");
            }
            std::vector<MaterialOutputAttachmentState>
                pass_output_states;
            if (pass->contains("material_output_states")) {
                if (!pass_output_schema) {
                    throw std::runtime_error(
                        "material_routing pass '" +
                        pass_name +
                        "' material_output_states requires "
                        "material_outputs");
                }
                pass_output_states =
                    parseMaterialOutputAttachmentStates(
                        pass->at(
                            "material_output_states"),
                        *pass_output_schema,
                        "material_routing pass '" +
                            pass_name +
                            "' material_output_states");
            }
            if (!selected_output_schema_initialized) {
                selected_output_schema =
                    pass_output_schema;
                selected_output_schema_initialized =
                    true;
            } else if (
                selected_output_schema !=
                pass_output_schema) {
                throw std::runtime_error(
                    "material_routing route '" +
                    std::string{route_name} +
                    "' pass '" + pass_name +
                    "' has different material_outputs "
                    "schemas across rendering pass variants");
            }
            if (!selected_output_states_initialized) {
                selected_output_states =
                    std::move(pass_output_states);
                selected_output_states_initialized = true;
            } else if (
                selected_output_states !=
                pass_output_states) {
                throw std::runtime_error(
                    "material_routing route '" +
                    std::string{route_name} +
                    "' pass '" + pass_name +
                    "' has different material_output_states "
                    "across rendering pass variants");
            }
        }
        auto resolved_route = nlohmann::json{
            {"pass", pass_name},
            {"contract", materialPassContractName(expected)},
            {"shader_contract", materialShaderContractName(
                                    materialPassShaderContract(expected))},
            {"phase", materialPhaseName(materialPassPhase(expected))},
        };
        if (selected_output_schema) {
            resolved_route["output_schema"] =
                materialOutputSchemaToJson(
                    *selected_output_schema);
            resolved_route["output_schema_fingerprint"] =
                materialOutputSchemaFingerprint(
                    *selected_output_schema);
            if (!selected_output_states.empty()) {
                resolved_route["output_states"] =
                    materialOutputAttachmentStatesToJson(
                        selected_output_states,
                        *selected_output_schema);
                resolved_route[
                    "output_states_fingerprint"] =
                    materialOutputAttachmentStatesFingerprint(
                        selected_output_states,
                        *selected_output_schema);
            }
        }
        resolved_routes[route_name] =
            std::move(resolved_route);
    }
    return {
        {"policy", policy},
        {"routes", std::move(resolved_routes)},
    };
}

ResolvedRenderPipeline resolveRenderPipeline(
    const RenderPipelineRequest &request,
    const RenderEnvironmentCapabilities &capabilities,
    const RenderPipelineResolveDependencies &dependencies) {
    if (!request.authored_config.is_object()) {
        throw std::runtime_error("Rendering config must be an object: " +
                                 request.source_name);
    }
    validatePassFieldOwnershipInConfig(
        request.authored_config,
        capabilities.pass_field_ownership,
        request.source_name);

    const auto graph_variant_policy = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{capabilities.graph_variant},
        capabilities.graph_variant_capabilities);
    std::vector<GraphVariantFeatureDecision> feature_decisions;
    auto composed = composeRenderFeatureConfig(
        request.authored_config,
        RenderFeatureComposeDependencies{
            .load_feature_json = dependencies.load_feature_json,
            .runtime_shader_compiler_enabled =
                capabilities.runtime_shader_compiler_enabled,
            .include_feature =
                [&graph_variant_policy, &feature_decisions](
                std::string_view feature_name,
                const nlohmann::json &feature) {
                auto decision = decideGraphVariantFeature(
                    graph_variant_policy, feature_name, feature);
                if (decision.disposition ==
                    GraphVariantFeatureDisposition::reject) {
                    throw std::runtime_error(
                        graphVariantFeatureRejectionMessage(
                            graph_variant_policy, decision));
                }
                const auto include =
                    decision.disposition ==
                    GraphVariantFeatureDisposition::include;
                if (!include) {
                    feature_decisions.push_back(
                        std::move(decision));
                }
                return include;
            },
            .load_pipeline_json = dependencies.load_pipeline_json,
            .transform_resolved_config =
                [&dependencies, &graph_variant_policy,
                 &capabilities, &request](
                const nlohmann::json &config) {
                validatePassFieldOwnershipInConfig(
                    config, capabilities.pass_field_ownership,
                    request.source_name);
                if (dependencies.resolve_render_strategy) {
                    return dependencies.resolve_render_strategy(
                        config, graph_variant_policy);
                }
                if (config.contains("render_strategy")) {
                    throw std::runtime_error(
                        "render strategy resolver is unavailable");
                }
                return config;
            },
            .validate_feature = dependencies.validate_feature,
        });

    validatePassFieldOwnershipInConfig(
        composed.config, capabilities.pass_field_ownership,
        request.source_name);

    ResolvedRenderPipeline result;
    result.normalized_config = std::move(composed.config);
    result.shader_defines = std::move(composed.shader_defines);
    result.feature_names = std::move(composed.feature_names);
    result.excluded_feature_names =
        std::move(composed.excluded_feature_names);
    result.projection_jitter = std::move(composed.projection_jitter);
    result.feature_instances = std::move(composed.feature_instances);
    result.surface_resource_contracts =
        std::move(composed.surface_resource_contracts);
    result.material_routing = std::move(composed.material_routing);
    result.draw_sort = std::move(composed.draw_sort);
    result.pipeline_preset = std::move(composed.pipeline_preset);
    result.pass_provenance =
        std::move(composed.pass_provenance);
    result.resource_provenance =
        std::move(composed.resource_provenance);
    result.used_features = composed.used_features;
    result.graph_variant_policy = graph_variant_policy;
    result.graph_variant_feature_decisions =
        std::move(feature_decisions);

    if (dependencies.normalize_config) {
        result.normalized_config = dependencies.normalize_config(
            result.normalized_config, result.feature_names);
    }
    transformGraphVariantConfig(
        result.graph_variant_policy, result.normalized_config);
    if (dependencies.transform_config) {
        dependencies.transform_config(result.normalized_config);
    }
    validatePassFieldOwnershipInConfig(
        result.normalized_config,
        capabilities.pass_field_ownership,
        request.source_name);
    validateGraphVariantConfig(
        result.graph_variant_policy, result.normalized_config);
    if (dependencies.validate_config) {
        dependencies.validate_config(result.normalized_config);
    }
    result.sample_count_policy =
        compileSampleCountPolicy(result.normalized_config);
    result.target_planning =
        compileTargetPlanningPolicy(
            result.normalized_config,
            result.graph_variant_policy
                .rendering_pass_name_suffix);
    result.vulkan_plan_pins =
        compileVulkanTargetPlanPins(
            result.normalized_config,
            result.graph_variant_policy.variant);
    result.vulkan_physical_fragments =
        compileVulkanPhysicalFragments(
            result.normalized_config,
            result.graph_variant_policy.variant);
    result.xr_target_policy =
        compileXrTargetPolicy(
            result.normalized_config,
            result.graph_variant_policy.variant);
    suffixRenderingPassNames(result.normalized_config,
                             result.graph_variant_policy
                                 .rendering_pass_name_suffix);

    result.diagnostics.push_back(RenderPipelineDiagnostic{
        RenderPipelineDiagnosticKind::graph_variant_selected,
        std::string{renderPipelineGraphVariantName(
            result.graph_variant_policy.variant)},
        "selected_by_environment",
    });
    if (result.pipeline_preset) {
        result.diagnostics.push_back(RenderPipelineDiagnostic{
            RenderPipelineDiagnosticKind::pipeline_preset_resolved,
            result.pipeline_preset->reference,
            result.pipeline_preset->name + "@" +
                std::to_string(result.pipeline_preset->version),
        });
    }
    for (const auto &decision :
         result.graph_variant_feature_decisions) {
        result.diagnostics.push_back(RenderPipelineDiagnostic{
            RenderPipelineDiagnosticKind::feature_excluded,
            decision.feature_name,
            std::string{renderPipelineGraphVariantName(
                result.graph_variant_policy.variant)} +
                ":" +
                std::string{graphVariantFeatureReasonName(
                    decision.reason)},
        });
    }
    return result;
}

std::string_view projectionJitterPatternName(ProjectionJitterPattern pattern) {
    switch (pattern) {
    case ProjectionJitterPattern::halton23: return "halton23";
    case ProjectionJitterPattern::table: return "table";
    }
    return "unknown";
}

double compiledRenderNumericValueAsDouble(
    const CompiledRenderNumericValue &value) {
    return std::visit(
        [](const auto typed_value) { return static_cast<double>(typed_value); },
        value);
}

std::string_view materialRoutingPolicyName(MaterialRoutingPolicy policy) {
    switch (policy) {
    case MaterialRoutingPolicy::hybrid_auto_v1: return "hybrid_auto_v1";
    }
    return "unknown";
}

std::string_view drawSortXrViewPolicyName(DrawSortXrViewPolicy policy) {
    switch (policy) {
    case DrawSortXrViewPolicy::logical_view_center:
        return "logical_view_center";
    case DrawSortXrViewPolicy::per_view: return "per_view";
    }
    return "unknown";
}

std::string_view renderCompilerProgramModeName(
    RenderCompilerProgramMode mode) {
    switch (mode) {
    case RenderCompilerProgramMode::portable:
        return "portable";
    case RenderCompilerProgramMode::mixed:
        return "mixed";
    case RenderCompilerProgramMode::backend_native:
        return "backend_native";
    }
    return "unknown";
}

namespace {

CompiledProjectionJitter compileProjectionJitter(
    const nlohmann::json &declaration) {
    constexpr std::string_view context = "resolved projection_jitter";
    if (!declaration.is_object()) {
        throw std::runtime_error(std::string{context} + " must be an object");
    }
    requireOnlyKeys(declaration, {"provider", "pattern", "phases", "offsets_px"},
                    context);

    CompiledProjectionJitter result;
    result.provider = requireString(declaration, "provider", context);
    const auto pattern = requireString(declaration, "pattern", context);
    if (pattern == "halton23") {
        result.pattern = ProjectionJitterPattern::halton23;
    } else if (pattern == "table") {
        result.pattern = ProjectionJitterPattern::table;
    } else {
        throw std::runtime_error(std::string{context} +
                                 " has unknown pattern: " + pattern);
    }

    const auto phases = declaration.find("phases");
    if (phases == declaration.end() || !phases->is_number_integer()) {
        throw std::runtime_error(std::string{context} +
                                 " requires unsigned integer phases");
    }
    const bool phases_in_range =
        phases->is_number_unsigned()
            ? phases->get<std::uint64_t>() >= 1 &&
                  phases->get<std::uint64_t>() <= 64
            : phases->get<std::int64_t>() >= 1 &&
                  phases->get<std::int64_t>() <= 64;
    if (!phases_in_range) {
        throw std::runtime_error(std::string{context} +
                                 " phases must be in range 1..64");
    }
    result.phases = phases->get<std::uint32_t>();

    if (result.pattern == ProjectionJitterPattern::halton23) {
        if (declaration.contains("offsets_px")) {
            throw std::runtime_error(std::string{context} +
                                     " halton23 must not define offsets_px");
        }
        return result;
    }

    const auto offsets = declaration.find("offsets_px");
    if (offsets == declaration.end() || !offsets->is_array() ||
        offsets->size() != result.phases) {
        throw std::runtime_error(std::string{context} +
                                 " table offsets_px length must match phases");
    }
    result.offsets_px.reserve(offsets->size());
    for (std::size_t index = 0; index < offsets->size(); ++index) {
        const auto &offset = offsets->at(index);
        if (!offset.is_array() || offset.size() != 2) {
            throw std::runtime_error(std::string{context} + " offsets_px[" +
                                     std::to_string(index) +
                                     "] must contain exactly two numbers");
        }
        std::array<CompiledRenderNumericValue, 2> compiled_offset{};
        for (std::size_t component = 0; component < 2; ++component) {
            if (!offset.at(component).is_number()) {
                throw std::runtime_error(std::string{context} + " offsets_px[" +
                                         std::to_string(index) + "][" +
                                         std::to_string(component) +
                                         "] must be a finite number");
            }
            CompiledRenderNumericValue typed_value;
            if (offset.at(component).is_number_unsigned()) {
                typed_value = offset.at(component).get<std::uint64_t>();
            } else if (offset.at(component).is_number_integer()) {
                typed_value = offset.at(component).get<std::int64_t>();
            } else {
                typed_value = offset.at(component).get<double>();
            }
            const auto value = compiledRenderNumericValueAsDouble(typed_value);
            if (!std::isfinite(value) || value < -0.5 || value >= 0.5) {
                throw std::runtime_error(std::string{context} + " offsets_px[" +
                                         std::to_string(index) + "][" +
                                         std::to_string(component) +
                                         "] must be finite and in range [-0.5, 0.5)");
            }
            compiled_offset[component] = std::move(typed_value);
        }
        result.offsets_px.push_back(compiled_offset);
    }
    return result;
}

CompiledRenderFeatureParameterValue compileFeatureParameterValue(
    const nlohmann::json &value, std::string_view context) {
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) return value.get<std::int64_t>();
    if (value.is_number_float()) return value.get<double>();
    if (value.is_string()) return value.get<std::string>();
    throw std::runtime_error(std::string{context} +
                             " must be bool, integer, number, or string");
}

std::vector<CompiledRenderFeatureInstance> compileFeatureInstances(
    const nlohmann::json &instances) {
    if (!instances.is_array()) {
        throw std::runtime_error("resolved feature_instances must be an array");
    }
    std::vector<CompiledRenderFeatureInstance> result;
    result.reserve(instances.size());
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const auto &instance = instances.at(index);
        const auto context =
            "resolved feature_instances[" + std::to_string(index) + "]";
        if (!instance.is_object()) {
            throw std::runtime_error(context + " must be an object");
        }
        requireOnlyKeys(instance, {"feature", "parameters", "ref"}, context);
        CompiledRenderFeatureInstance compiled;
        compiled.feature = requireString(instance, "feature", context);
        compiled.reference = requireString(instance, "ref", context);
        const auto parameters = instance.find("parameters");
        if (parameters == instance.end() || !parameters->is_object()) {
            throw std::runtime_error(context + " parameters must be an object");
        }
        compiled.parameters.reserve(parameters->size());
        for (auto parameter = parameters->begin(); parameter != parameters->end();
             ++parameter) {
            const auto parameter_context =
                context + " parameter '" + parameter.key() + "'";
            compiled.parameters.push_back(CompiledRenderFeatureParameter{
                parameter.key(),
                compileFeatureParameterValue(parameter.value(),
                                             parameter_context),
            });
        }
        result.push_back(std::move(compiled));
    }
    return result;
}

std::vector<std::string> compileStringArray(
    const nlohmann::json &object, std::string_view field,
    std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_array()) {
        throw std::runtime_error(
            std::string{context} + " requires array " +
            std::string{field});
    }
    std::vector<std::string> result;
    result.reserve(found->size());
    for (const auto &entry : *found) {
        if (!entry.is_string() ||
            entry.get_ref<const std::string &>().empty()) {
            throw std::runtime_error(
                std::string{context} + " " +
                std::string{field} +
                " entries must be non-empty strings");
        }
        result.push_back(entry.get<std::string>());
    }
    return result;
}

bool jsonStringListContains(
    const nlohmann::json &value,
    std::string_view needle) {
    if (value.is_string()) {
        return value.get_ref<const std::string &>() ==
               needle;
    }
    return value.is_array() &&
           std::any_of(
               value.begin(), value.end(),
               [&](const auto &entry) {
                   return entry.is_string() &&
                          entry.get_ref<
                              const std::string &>() ==
                              needle;
               });
}

bool compiledPassWritesResource(
    const nlohmann::json &pass,
    std::string_view resource) {
    if (!pass.contains("output") ||
        !pass.at("output").is_object()) {
        return false;
    }
    const auto &output = pass.at("output");
    return (output.contains("color") &&
            jsonStringListContains(
                output.at("color"), resource)) ||
           (output.contains("depth") &&
            output.at("depth").is_string() &&
            output.at("depth")
                    .get_ref<const std::string &>() ==
                resource);
}

std::vector<CompiledSurfaceResourceContract>
compileSurfaceResourceContracts(
    const nlohmann::json &declarations,
    const nlohmann::json &config) {
    if (!declarations.is_array()) {
        throw std::runtime_error(
            "resolved surface_resource_contracts must be an array");
    }

    std::unordered_map<
        std::string, const nlohmann::json *>
        passes;
    if (!config.contains("rendering_passes") ||
        !config.at("rendering_passes").is_array()) {
        if (!declarations.empty()) {
            throw std::runtime_error(
                "surface resource contracts require rendering_passes");
        }
        return {};
    }
    for (const auto &pass_set :
         config.at("rendering_passes")) {
        if (!pass_set.is_object() ||
            !pass_set.contains("passes") ||
            !pass_set.at("passes").is_array()) {
            throw std::runtime_error(
                "surface resource contract validation requires "
                "rendering pass arrays");
        }
        for (const auto &pass :
             pass_set.at("passes")) {
            if (!pass.is_object()) continue;
            const auto name =
                pass.value("name", std::string{});
            if (!name.empty() &&
                !passes.emplace(name, &pass).second) {
                throw std::runtime_error(
                    "surface resource contract pass name is "
                    "ambiguous: " +
                    name);
            }
        }
    }

    std::unordered_set<std::string> targets;
    if (config.contains("render_targets") &&
        config.at("render_targets").is_array()) {
        for (const auto &target :
             config.at("render_targets")) {
            if (target.is_object()) {
                targets.insert(
                    target.value("name", std::string{}));
            }
        }
    }

    const auto types =
        makeBuiltinLogicalTypeRegistry();
    std::unordered_set<std::string> contracts;
    std::vector<CompiledSurfaceResourceContract> result;
    result.reserve(declarations.size());
    for (std::size_t index = 0;
         index < declarations.size(); ++index) {
        const auto &declaration =
            declarations.at(index);
        const auto context =
            "resolved surface_resource_contracts[" +
            std::to_string(index) + "]";
        if (!declaration.is_object()) {
            throw std::runtime_error(
                context + " must be an object");
        }
        requireOnlyKeys(
            declaration,
            {"contract", "resource", "producer",
             "provider_feature", "provider_ref",
             "material_consumers",
             "fullscreen_consumers"},
            context);

        CompiledSurfaceResourceContract compiled;
        const auto contract_name =
            requireString(
                declaration, "contract", context);
        if (!contracts.insert(contract_name).second) {
            throw std::runtime_error(
                context + " duplicates contract '" +
                contract_name + "'");
        }
        compiled.contract =
            makeBuiltinMaterialPassInputContract(
                types, contract_name);
        compiled.resource =
            requireString(
                declaration, "resource", context);
        compiled.producer =
            requireString(
                declaration, "producer", context);
        compiled.provider_feature =
            requireString(
                declaration, "provider_feature", context);
        compiled.provider_reference =
            requireString(
                declaration, "provider_ref", context);
        compiled.material_consumers =
            compileStringArray(
                declaration, "material_consumers",
                context);
        compiled.fullscreen_consumers =
            compileStringArray(
                declaration, "fullscreen_consumers",
                context);

        if (!targets.contains(compiled.resource)) {
            throw std::runtime_error(
                context + " resource target is missing: " +
                compiled.resource);
        }
        const auto producer =
            passes.find(compiled.producer);
        if (producer == passes.end() ||
            !compiledPassWritesResource(
                *producer->second,
                compiled.resource)) {
            throw std::runtime_error(
                context + " producer '" +
                compiled.producer +
                "' no longer writes resource '" +
                compiled.resource + "'");
        }
        for (const auto &consumer :
             compiled.material_consumers) {
            const auto found = passes.find(consumer);
            if (found == passes.end() ||
                found->second->value(
                    "type", std::string{}) !=
                    "material" ||
                !found->second->contains(
                    "surface_resources") ||
                !found->second->at(
                    "surface_resources")
                     .is_object() ||
                found->second->at(
                    "surface_resources")
                     .value(
                         compiled.contract.name,
                         std::string{}) !=
                    compiled.resource) {
                throw std::runtime_error(
                    context + " material consumer '" +
                    consumer +
                    "' no longer binds contract '" +
                    compiled.contract.name + "'");
            }
        }
        for (const auto &consumer :
             compiled.fullscreen_consumers) {
            const auto found = passes.find(consumer);
            if (found == passes.end() ||
                found->second->value(
                    "type", std::string{}) !=
                    "fullscreen" ||
                !found->second->contains("input") ||
                !jsonStringListContains(
                    found->second->at("input"),
                    compiled.resource)) {
                throw std::runtime_error(
                    context + " fullscreen consumer '" +
                    consumer +
                    "' no longer reads resource '" +
                    compiled.resource + "'");
            }
        }
        if (compiled.material_consumers.empty() &&
            compiled.fullscreen_consumers.empty()) {
            throw std::runtime_error(
                context + " has no consumers");
        }
        result.push_back(std::move(compiled));
    }
    return result;
}

CompiledMaterialRouting compileMaterialRouting(
    const nlohmann::json &routing) {
    constexpr std::string_view context = "resolved material_routing";
    if (!routing.is_object()) {
        throw std::runtime_error(std::string{context} + " must be an object");
    }
    requireOnlyKeys(routing, {"policy", "routes"}, context);
    const auto policy = requireString(routing, "policy", context);
    if (policy != materialRoutingPolicyName(
                      MaterialRoutingPolicy::hybrid_auto_v1)) {
        throw std::runtime_error(std::string{context} +
                                 " has unsupported policy: " + policy);
    }
    const auto routes = routing.find("routes");
    if (routes == routing.end() || !routes->is_object()) {
        throw std::runtime_error(std::string{context} +
                                 " requires object routes");
    }
    requireOnlyKeys(*routes,
                    {"deferred_geometry", "forward_opaque",
                     "forward_transparent"},
                    "resolved material_routing routes");

    constexpr std::array route_values{
        MaterialRouteClass::deferred_geometry,
        MaterialRouteClass::forward_opaque,
        MaterialRouteClass::forward_transparent,
    };
    CompiledMaterialRouting result;
    std::unordered_set<std::string> selected_passes;
    result.routes.reserve(route_values.size());
    for (const auto route : route_values) {
        const auto route_name = materialRouteClassName(route);
        const auto entry = routes->find(route_name);
        if (entry == routes->end() || !entry->is_object()) {
            throw std::runtime_error("resolved material_routing route '" +
                                     std::string{route_name} +
                                     "' must be an object");
        }
        const auto route_context = "resolved material_routing route '" +
                                   std::string{route_name} + "'";
        requireOnlyKeys(*entry,
                        {"pass", "contract", "shader_contract", "phase",
                         "output_schema",
                         "output_schema_fingerprint",
                         "output_states",
                         "output_states_fingerprint"},
                        route_context);
        const auto pass_name = requireString(*entry, "pass", route_context);
        if (!selected_passes.insert(pass_name).second) {
            throw std::runtime_error(
                "resolved material_routing routes must select distinct passes: " +
                pass_name);
        }
        const auto contract_name =
            requireString(*entry, "contract", route_context);
        const auto contract = materialPassContractFromName(contract_name);
        const auto expected = expectedContract(route);
        if (!contract || *contract != expected) {
            throw std::runtime_error(route_context + " requires contract '" +
                                     std::string{materialPassContractName(expected)} +
                                     "'");
        }
        const auto shader_contract =
            requireString(*entry, "shader_contract", route_context);
        const auto expected_shader = materialShaderContractName(
            materialPassShaderContract(expected));
        if (shader_contract != expected_shader) {
            throw std::runtime_error(route_context +
                                     " requires shader_contract '" +
                                     std::string{expected_shader} + "'");
        }
        const auto phase = requireString(*entry, "phase", route_context);
        const auto expected_phase =
            materialPhaseName(materialPassPhase(expected));
        if (phase != expected_phase) {
            throw std::runtime_error(route_context + " requires phase '" +
                                     std::string{expected_phase} + "'");
        }
        std::optional<MaterialOutputSchema>
            output_schema;
        const auto encoded_output_schema =
            entry->find("output_schema");
        const auto encoded_fingerprint =
            entry->find("output_schema_fingerprint");
        if ((encoded_output_schema == entry->end()) !=
            (encoded_fingerprint == entry->end())) {
            throw std::runtime_error(
                route_context +
                " must provide output_schema and "
                "output_schema_fingerprint together");
        }
        if (encoded_output_schema != entry->end()) {
            output_schema = parseMaterialOutputSchema(
                *encoded_output_schema,
                route_context + " output_schema");
            if (!encoded_fingerprint->is_string() ||
                encoded_fingerprint->get<std::string>() !=
                    materialOutputSchemaFingerprint(
                        *output_schema)) {
                throw std::runtime_error(
                    route_context +
                    " output_schema_fingerprint does not match "
                    "output_schema");
            }
        }
        std::vector<MaterialOutputAttachmentState>
            output_states;
        const auto encoded_output_states =
            entry->find("output_states");
        const auto encoded_states_fingerprint =
            entry->find("output_states_fingerprint");
        if ((encoded_output_states == entry->end()) !=
            (encoded_states_fingerprint == entry->end())) {
            throw std::runtime_error(
                route_context +
                " must provide output_states and "
                "output_states_fingerprint together");
        }
        if (encoded_output_states != entry->end()) {
            if (!output_schema) {
                throw std::runtime_error(
                    route_context +
                    " output_states requires output_schema");
            }
            output_states =
                parseMaterialOutputAttachmentStates(
                    *encoded_output_states,
                    *output_schema,
                    route_context + " output_states");
            if (!encoded_states_fingerprint->is_string() ||
                encoded_states_fingerprint
                        ->get<std::string>() !=
                    materialOutputAttachmentStatesFingerprint(
                        output_states,
                        *output_schema)) {
                throw std::runtime_error(
                    route_context +
                    " output_states_fingerprint does not match "
                    "output_states");
            }
        }
        result.routes.push_back(
            CompiledMaterialRoute{
                route, pass_name, expected,
                std::move(output_schema),
                std::move(output_states)});
    }
    return result;
}

CompiledDrawSorting compileDrawSorting(const nlohmann::json &declaration) {
    CompiledDrawSorting result;
    if (declaration.is_null()) return result;
    if (!declaration.is_object()) {
        throw std::runtime_error("resolved draw_sort must be an object");
    }
    result.authored = true;
    requireOnlyKeys(declaration, {"opaque", "transparent", "xr_view_policy"},
                    "resolved draw_sort");
    const auto compile_phase = [&](std::string_view phase,
                                   CompiledDrawSortPolicy &output) {
        const auto found = declaration.find(phase);
        if (found == declaration.end()) return;
        const auto context = "resolved draw_sort " + std::string{phase};
        if (!found->is_object()) {
            throw std::runtime_error(context + " must be an object");
        }
        requireOnlyKeys(*found, {"provider"}, context);
        output.provider = requireString(*found, "provider", context);
    };
    compile_phase("opaque", result.opaque);
    compile_phase("transparent", result.transparent);

    if (const auto found = declaration.find("xr_view_policy");
        found != declaration.end()) {
        if (!found->is_string()) {
            throw std::runtime_error(
                "resolved draw_sort xr_view_policy must be a string");
        }
        const auto value = found->get<std::string>();
        if (value == "logical_view_center") {
            result.xr_view_policy =
                DrawSortXrViewPolicy::logical_view_center;
        } else if (value == "per_view") {
            result.xr_view_policy = DrawSortXrViewPolicy::per_view;
        } else {
            throw std::runtime_error(
                "resolved draw_sort has unknown xr_view_policy: " + value);
        }
    }
    return result;
}

nlohmann::json serializeFeatureParameterValue(
    const CompiledRenderFeatureParameterValue &value) {
    return std::visit(
        [](const auto &typed_value) { return nlohmann::json(typed_value); },
        value);
}

nlohmann::json serializeNumericValue(
    const CompiledRenderNumericValue &value) {
    return std::visit(
        [](const auto typed_value) { return nlohmann::json(typed_value); },
        value);
}

} // namespace

std::string renderPipelineProvenanceSourceName(
    RenderPipelineProvenanceSource source,
    std::string_view provider_feature) {
    switch (source) {
    case RenderPipelineProvenanceSource::project:
        return "project";
    case RenderPipelineProvenanceSource::feature:
        if (provider_feature.empty()) {
            throw std::runtime_error(
                "feature provenance requires provider_feature");
        }
        return "feature:" + std::string{provider_feature};
    case RenderPipelineProvenanceSource::engine:
        return "engine";
    }
    throw std::runtime_error("unknown render pipeline provenance source");
}

void synchronizeRenderPipelineProvenance(
    const nlohmann::json &config,
    std::vector<RenderPassProvenance> &passes,
    std::vector<RenderResourceProvenance> &resources,
    RenderPipelineProvenanceSource default_source) {
    std::unordered_map<std::string, RenderPassProvenance>
        known_passes;
    known_passes.reserve(passes.size());
    for (auto &pass : passes) {
        if (pass.name.empty() ||
            !known_passes.emplace(pass.name, std::move(pass)).second) {
            throw std::runtime_error(
                "render pass provenance has an empty or duplicate name");
        }
    }

    std::vector<RenderPassProvenance> synchronized_passes;
    std::unordered_set<std::string> seen_passes;
    const auto append_pass = [&](const nlohmann::json &entry,
                                 std::string_view context) {
        const auto name = requireString(entry, "name", context);
        if (!seen_passes.insert(name).second) return;
        const auto found = known_passes.find(name);
        if (found != known_passes.end()) {
            synchronized_passes.push_back(found->second);
        } else {
            synchronized_passes.push_back(RenderPassProvenance{
                .name = name,
                .source = default_source,
            });
        }
    };
    if (const auto rendering_passes = config.find("rendering_passes");
        rendering_passes != config.end()) {
        if (!rendering_passes->is_array()) {
            throw std::runtime_error("rendering_passes must be an array");
        }
        for (const auto &pass_set : *rendering_passes) {
            if (!pass_set.is_object() ||
                !pass_set.contains("passes") ||
                !pass_set.at("passes").is_array()) {
                throw std::runtime_error(
                    "rendering pass requires passes array");
            }
            for (const auto &pass : pass_set.at("passes")) {
                append_pass(pass, "render pass");
            }
        }
    }
    if (const auto compute_tasks = config.find("compute_tasks");
        compute_tasks != config.end()) {
        if (!compute_tasks->is_array()) {
            throw std::runtime_error("compute_tasks must be an array");
        }
        for (const auto &task : *compute_tasks) {
            append_pass(task, "compute task");
        }
    }
    passes = std::move(synchronized_passes);

    std::unordered_map<std::string, RenderResourceProvenance>
        known_resources;
    known_resources.reserve(resources.size());
    for (auto &resource : resources) {
        if (resource.name.empty() ||
            !known_resources.emplace(resource.name, std::move(resource)).second) {
            throw std::runtime_error(
                "render resource provenance has an empty or duplicate name");
        }
    }

    std::vector<RenderResourceProvenance> synchronized_resources;
    std::unordered_set<std::string> seen_resources;
    const auto append_resource =
        [&](std::string name, std::string kind,
            std::optional<std::vector<std::string>> usage = std::nullopt) {
        if (name.empty() || !seen_resources.insert(name).second) {
            if (name.empty()) {
                throw std::runtime_error(
                    "render resource provenance requires a name");
            }
            return;
        }
        const auto found = known_resources.find(name);
        if (found != known_resources.end()) {
            auto retained = found->second;
            retained.kind = std::move(kind);
            retained.usage = std::move(usage);
            synchronized_resources.push_back(std::move(retained));
        } else {
            synchronized_resources.push_back(RenderResourceProvenance{
                .name = std::move(name),
                .kind = std::move(kind),
                .source = default_source,
                .usage = std::move(usage),
            });
        }
    };
    if (const auto render_targets = config.find("render_targets");
        render_targets != config.end()) {
        if (!render_targets->is_array()) {
            throw std::runtime_error("render_targets must be an array");
        }
        for (const auto &target : *render_targets) {
            const auto name =
                requireString(target, "name", "render target");
            validateAuthoredRenderResourceName(
                name, "render target");
            append_resource(
                name,
                "render_target",
                target.contains("usage")
                    ? std::optional<std::vector<std::string>>{
                          compileStringArray(
                              target, "usage", "render target '" +
                                                   name + "'")}
                    : std::nullopt);
        }
    }
    if (const auto buffers = config.find("buffers");
        buffers != config.end()) {
        if (!buffers->is_array()) {
            throw std::runtime_error("buffers must be an array");
        }
        for (const auto &buffer : *buffers) {
            if (buffer.is_string()) {
                const auto name = buffer.get<std::string>();
                validateAuthoredRenderResourceName(
                    name, "buffer");
                append_resource(name, "buffer");
            } else if (buffer.is_object()) {
                const auto name =
                    requireString(buffer, "name", "buffer");
                validateAuthoredRenderResourceName(
                    name, "buffer");
                append_resource(name, "buffer");
            } else {
                throw std::runtime_error(
                    "buffers entries must be strings or objects");
            }
        }
    }
    if (seen_passes.contains("output_transform")) {
        append_resource("swapchain", "frame_target");
    }
    resources = std::move(synchronized_resources);
}

CompiledRenderPipeline compileRenderPipeline(
    const ResolvedRenderPipeline &pipeline) {
    if (renderPipelineGraphVariantName(
            pipeline.graph_variant_policy.variant) == "unknown") {
        throw std::runtime_error("resolved render pipeline has unknown graph variant");
    }
    const auto canonical_graph_variant_policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                pipeline.graph_variant_policy.variant});
    if (pipeline.graph_variant_policy !=
        canonical_graph_variant_policy) {
        throw std::runtime_error(
            "resolved render pipeline has inconsistent graph variant policy");
    }
    const auto canonical_xr_target_policy =
        compileXrTargetPolicy(
            pipeline.normalized_config,
            pipeline.graph_variant_policy.variant);
    const auto canonical_target_planning =
        compileTargetPlanningPolicy(
            pipeline.normalized_config,
            pipeline.graph_variant_policy
                .rendering_pass_name_suffix);
    const auto canonical_vulkan_plan_pins =
        compileVulkanTargetPlanPins(
            pipeline.normalized_config,
            pipeline.graph_variant_policy.variant);
    const auto canonical_vulkan_physical_fragments =
        compileVulkanPhysicalFragments(
            pipeline.normalized_config,
            pipeline.graph_variant_policy.variant);
    if (pipeline.target_planning.authored &&
        pipeline.target_planning !=
            canonical_target_planning) {
        throw std::runtime_error(
            "resolved render pipeline has inconsistent target planning "
            "policy");
    }
    if (!pipeline.vulkan_plan_pins.empty() &&
        pipeline.vulkan_plan_pins !=
            canonical_vulkan_plan_pins) {
        throw std::runtime_error(
            "resolved render pipeline has inconsistent Vulkan plan "
            "pins");
    }
    if (!pipeline.vulkan_physical_fragments.empty() &&
        pipeline.vulkan_physical_fragments !=
            canonical_vulkan_physical_fragments) {
        throw std::runtime_error(
            "resolved render pipeline has inconsistent Vulkan "
            "physical fragments");
    }
    if (pipeline.xr_target_policy.authored &&
        pipeline.xr_target_policy !=
            canonical_xr_target_policy) {
        throw std::runtime_error(
            "resolved render pipeline has inconsistent XR target policy");
    }

    CompiledRenderPipeline result;
    result.shader_defines = pipeline.shader_defines;
    result.feature_names = pipeline.feature_names;
    result.excluded_feature_names = pipeline.excluded_feature_names;
    if (pipeline.projection_jitter) {
        result.projection_jitter =
            compileProjectionJitter(*pipeline.projection_jitter);
    }
    result.feature_instances =
        compileFeatureInstances(pipeline.feature_instances);
    result.surface_resource_contracts =
        compileSurfaceResourceContracts(
            pipeline.surface_resource_contracts,
            pipeline.normalized_config);
    if (!pipeline.material_routing.is_null()) {
        result.material_routing =
            compileMaterialRouting(pipeline.material_routing);
    }
    result.lighting_data =
        compileLightingDataPlan(
            pipeline.normalized_config);
    result.draw_sorting = compileDrawSorting(pipeline.draw_sort);
    result.sample_count_policy = pipeline.sample_count_policy;
    result.target_planning =
        canonical_target_planning;
    result.vulkan_plan_pins =
        canonical_vulkan_plan_pins;
    result.vulkan_physical_fragments =
        canonical_vulkan_physical_fragments;
    result.xr_target_policy = canonical_xr_target_policy;
    result.pipeline_preset = pipeline.pipeline_preset;
    result.graph_variant_policy =
        pipeline.graph_variant_policy;
    result.graph_variant_feature_decisions =
        pipeline.graph_variant_feature_decisions;
    result.diagnostics = pipeline.diagnostics;
    result.pass_provenance = pipeline.pass_provenance;
    result.resource_provenance = pipeline.resource_provenance;
    synchronizeRenderPipelineProvenance(
        pipeline.normalized_config,
        result.pass_provenance,
        result.resource_provenance);
    result.used_features = pipeline.used_features;
    return result;
}

nlohmann::json serializeCompiledRenderPipelineMetadata(
    const CompiledRenderPipeline &pipeline) {
    nlohmann::json metadata = nlohmann::json::object();
    if (pipeline.render_compiler_program) {
        const auto &selection =
            *pipeline.render_compiler_program;
        metadata["render_compiler_program"] = {
            {"schema_version",
             selection.schema_version},
            {"name", selection.name},
            {"implementation",
             selection.implementation},
            {"backend", selection.backend},
            {"mode",
             renderCompilerProgramModeName(
                 selection.mode)},
        };
    }
    if (pipeline.render_strategy) {
        const auto &selection =
            *pipeline.render_strategy;
        metadata["render_strategy"] = {
            {"name", selection.name},
            {"provider", selection.provider},
            {"implementation",
             selection.implementation},
            {"contract", selection.contract},
            {"output_contract",
             selection.output_contract},
            {"graph_variant",
             selection.graph_variant},
            {"facade_capability_bits",
             selection.facade_capability_bits},
            {"input_config_fingerprint",
             selection.input_config_fingerprint},
            {"output_config_fingerprint",
             selection.output_config_fingerprint},
            {"provider_owner",
             selection.provider_owner},
            {"provider_identity",
             selection.provider_identity},
            {"provider_generation",
             selection.provider_generation},
            {"provider_version",
             selection.provider_version},
            {"provider_capability_bits",
             selection.provider_capability_bits},
            {"explicitly_selected",
             selection.explicitly_selected},
        };
    }
    if (!pipeline.graph_transforms.empty()) {
        auto transforms = nlohmann::json::array();
        for (const auto &selection :
             pipeline.graph_transforms) {
            transforms.push_back({
                {"name", selection.name},
                {"provider", selection.provider},
                {"implementation",
                 selection.implementation},
                {"contract", selection.contract},
                {"boundary_fingerprint",
                 selection.boundary_fingerprint},
                {"input_graph_fingerprint",
                 selection.input_graph_fingerprint},
                {"output_graph_fingerprint",
                 selection.output_graph_fingerprint},
                {"provider_owner",
                 selection.provider_owner},
                {"provider_identity",
                 selection.provider_identity},
                {"provider_generation",
                 selection.provider_generation},
                {"provider_version",
                 selection.provider_version},
                {"provider_capability_bits",
                 selection.provider_capability_bits},
                {"transform_index",
                 selection.transform_index},
                {"explicitly_selected",
                 selection.explicitly_selected},
            });
        }
        metadata["graph_transforms"] =
            std::move(transforms);
    }
    if (pipeline.projection_jitter) {
        const auto &jitter = *pipeline.projection_jitter;
        nlohmann::json declaration{
            {"provider", jitter.provider},
            {"pattern", projectionJitterPatternName(jitter.pattern)},
            {"phases", jitter.phases},
        };
        if (!jitter.offsets_px.empty()) {
            auto offsets = nlohmann::json::array();
            for (const auto &offset : jitter.offsets_px) {
                offsets.push_back(
                    {serializeNumericValue(offset[0]),
                     serializeNumericValue(offset[1])});
            }
            declaration["offsets_px"] = std::move(offsets);
        }
        metadata["projection_jitter"] = std::move(declaration);
    }
    nlohmann::json bound_instances = nlohmann::json::array();
    for (const auto &instance : pipeline.feature_instances) {
        if (!instance.parameters.empty()) {
            auto parameters = nlohmann::json::object();
            for (const auto &parameter : instance.parameters) {
                parameters[parameter.name] =
                    serializeFeatureParameterValue(parameter.value);
            }
            bound_instances.push_back({
                {"feature", instance.feature},
                {"parameters", std::move(parameters)},
                {"ref", instance.reference},
            });
        }
    }
    if (!bound_instances.empty()) {
        metadata["feature_instances"] = std::move(bound_instances);
    }
    if (!pipeline.surface_resource_contracts.empty()) {
        auto resources = nlohmann::json::array();
        for (const auto &resource :
             pipeline.surface_resource_contracts) {
            nlohmann::json declaration{
                {"contract", resource.contract.name},
                {"resource", resource.resource},
                {"producer", resource.producer},
                {"provider_feature",
                 resource.provider_feature},
                {"provider_ref",
                 resource.provider_reference},
                {"material_consumers",
                 resource.material_consumers},
                {"fullscreen_consumers",
                 resource.fullscreen_consumers},
                {"source_type",
                 logicalTypeToJson(
                     resource.contract.source_type)},
                {"sampled_type",
                 logicalTypeToJson(
                     resource.contract.sampled_type)},
                {"footprint",
                 logicalReadFootprintKindName(
                     resource.contract.footprint.kind)},
                {"sampling",
                 materialPassInputSamplingName(
                     resource.contract.sampling)},
                {"view_policy",
                 materialPassInputViewPolicyName(
                     resource.contract.view_policy)},
                {"fallback",
                 materialPassInputFallbackName(
                     resource.contract.fallback)},
                {"fallback_reason",
                 resource.contract.fallback ==
                         MaterialPassInputFallback::
                             fully_lit
                     ? "provider_feature_absent"
                     : "none"},
            };
            if (resource.contract.relation) {
                declaration["relation"] = {
                    {"kind",
                     materialPassInputRelationKindName(
                         resource.contract.relation->kind)},
                    {"light_index",
                     resource.contract.relation->light_index},
                    {"transform",
                     resource.contract.relation->transform},
                };
            }
            resources.push_back(
                std::move(declaration));
        }
        metadata["surface_resource_contracts"] =
            std::move(resources);
    }
    if (pipeline.material_routing) {
        nlohmann::json routes = nlohmann::json::object();
        for (const auto &route : pipeline.material_routing->routes) {
            auto declaration = nlohmann::json{
                {"pass", route.pass_name},
                {"contract", materialPassContractName(route.pass_contract)},
                {"shader_contract",
                 materialShaderContractName(
                     materialPassShaderContract(route.pass_contract))},
                {"phase",
                 materialPhaseName(materialPassPhase(route.pass_contract))},
            };
            if (route.output_schema) {
                declaration["output_schema"] =
                    materialOutputSchemaToJson(
                        *route.output_schema);
                declaration["output_schema_fingerprint"] =
                    materialOutputSchemaFingerprint(
                        *route.output_schema);
                if (!route.output_states.empty()) {
                    declaration["output_states"] =
                        materialOutputAttachmentStatesToJson(
                            route.output_states,
                            *route.output_schema);
                    declaration[
                        "output_states_fingerprint"] =
                        materialOutputAttachmentStatesFingerprint(
                            route.output_states,
                            *route.output_schema);
                }
            }
            routes[materialRouteClassName(route.route)] =
                std::move(declaration);
        }
        metadata["material_routing"] = {
            {"policy",
             materialRoutingPolicyName(pipeline.material_routing->policy)},
            {"routes", std::move(routes)},
        };
    }
    if (pipeline.lighting_data) {
        metadata["lighting_data"] =
            lightingDataPlanToJson(
                *pipeline.lighting_data);
    }
    if (pipeline.draw_sorting.authored) {
        metadata["draw_sort"] = {
            {"opaque", {{"provider", pipeline.draw_sorting.opaque.provider}}},
            {"transparent",
             {{"provider", pipeline.draw_sorting.transparent.provider}}},
            {"xr_view_policy",
             drawSortXrViewPolicyName(
                 pipeline.draw_sorting.xr_view_policy)},
        };
    }
    if (pipeline.sample_count_policy.authored) {
        metadata["sample_count"] =
            sampleCountPolicyToJson(pipeline.sample_count_policy);
    }
    if (pipeline.target_planning.authored) {
        metadata["target_planning"] =
            targetPlanningPolicyToJson(
                pipeline.target_planning);
    }
    if (!pipeline.vulkan_plan_pins.empty()) {
        auto pins = nlohmann::json::array();
        for (const auto &package :
             pipeline.vulkan_plan_pins) {
            pins.push_back(
                vulkanTargetPlanPinPackageToJson(
                    package));
        }
        metadata["vulkan_plan_pins"] =
            std::move(pins);
    }
    if (!pipeline.vulkan_physical_fragments.empty()) {
        auto fragments = nlohmann::json::array();
        for (const auto &package :
             pipeline.vulkan_physical_fragments) {
            fragments.push_back(
                vulkanPhysicalFragmentPackageToJson(
                    package));
        }
        metadata["vulkan_physical_fragments"] =
            std::move(fragments);
    }
    if (pipeline.xr_target_policy.authored) {
        metadata["xr_target_policy"] =
            xrTargetPolicyToJson(
                pipeline.xr_target_policy);
    }
    if (pipeline.pipeline_preset) {
        metadata["pipeline_preset"] = {
            {"ref", pipeline.pipeline_preset->reference},
            {"name", pipeline.pipeline_preset->name},
            {"version", pipeline.pipeline_preset->version},
        };
    }
    if (pipeline.graph_variant_policy.variant ==
        RenderPipelineGraphVariant::xr) {
        metadata["graph_variant"] = "xr";
        metadata["excluded_features"] = pipeline.excluded_feature_names;
    }
    return metadata;
}

} // namespace Pelican
