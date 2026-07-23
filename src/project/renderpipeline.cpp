#include "renderpipeline.hpp"
#include "featurecompose.hpp"

#include <algorithm>
#include <array>
#include <cmath>
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

void suffixRenderingPassNames(nlohmann::json &config,
                              std::string_view suffix) {
    if (suffix.empty()) return;
    for (auto &pass : config.at("rendering_passes")) {
        pass["name"] =
            pass.at("name").get<std::string>() + std::string{suffix};
    }
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
                     "draw_sort"},
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
    if (authored_config.contains("draw_sort")) {
        if (!authored_config.at("draw_sort").is_object()) {
            throw std::runtime_error(
                "rendering config draw_sort must be an object");
        }
        resolved["draw_sort"] = authored_config.at("draw_sort");
    }
    if (pipeline.contains("settings")) {
        const auto &settings = pipeline.at("settings");
        if (!settings.is_object()) {
            throw std::runtime_error(
                "rendering config pipeline settings must be an object");
        }
        requireOnlyKeys(settings, {"msaa"},
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

ResolvedRenderPipeline resolveRenderPipeline(
    const RenderPipelineRequest &request,
    const RenderEnvironmentCapabilities &capabilities,
    const RenderPipelineResolveDependencies &dependencies) {
    if (!request.authored_config.is_object()) {
        throw std::runtime_error("Rendering config must be an object: " +
                                 request.source_name);
    }

    const auto graph_variant_policy = compileGraphVariantPolicy(
        GraphVariantPolicyRequest{capabilities.graph_variant},
        capabilities.graph_variant_capabilities);
    std::vector<GraphVariantFeatureDecision> feature_decisions;
    auto composed = composeRenderFeatureConfig(
        request.authored_config,
        RenderFeatureComposeDependencies{
            dependencies.load_feature_json,
            capabilities.runtime_shader_compiler_enabled,
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
            dependencies.load_pipeline_json,
        });

    ResolvedRenderPipeline result;
    result.normalized_config = std::move(composed.config);
    result.shader_defines = std::move(composed.shader_defines);
    result.feature_names = std::move(composed.feature_names);
    result.excluded_feature_names =
        std::move(composed.excluded_feature_names);
    result.projection_jitter = std::move(composed.projection_jitter);
    result.feature_instances = std::move(composed.feature_instances);
    result.material_routing = std::move(composed.material_routing);
    result.draw_sort = std::move(composed.draw_sort);
    result.pipeline_preset = std::move(composed.pipeline_preset);
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
    validateGraphVariantConfig(
        result.graph_variant_policy, result.normalized_config);
    if (dependencies.validate_config) {
        dependencies.validate_config(result.normalized_config);
    }
    result.sample_count_policy =
        compileSampleCountPolicy(result.normalized_config);
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
                        {"pass", "contract", "shader_contract", "phase"},
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
        result.routes.push_back(
            CompiledMaterialRoute{route, pass_name, expected});
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
    if (!pipeline.material_routing.is_null()) {
        result.material_routing =
            compileMaterialRouting(pipeline.material_routing);
    }
    result.draw_sorting = compileDrawSorting(pipeline.draw_sort);
    result.sample_count_policy = pipeline.sample_count_policy;
    result.pipeline_preset = pipeline.pipeline_preset;
    result.graph_variant_policy =
        pipeline.graph_variant_policy;
    result.graph_variant_feature_decisions =
        pipeline.graph_variant_feature_decisions;
    result.diagnostics = pipeline.diagnostics;
    result.used_features = pipeline.used_features;
    return result;
}

nlohmann::json serializeCompiledRenderPipelineMetadata(
    const CompiledRenderPipeline &pipeline) {
    nlohmann::json metadata = nlohmann::json::object();
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
    if (pipeline.material_routing) {
        nlohmann::json routes = nlohmann::json::object();
        for (const auto &route : pipeline.material_routing->routes) {
            routes[materialRouteClassName(route.route)] = {
                {"pass", route.pass_name},
                {"contract", materialPassContractName(route.pass_contract)},
                {"shader_contract",
                 materialShaderContractName(
                     materialPassShaderContract(route.pass_contract))},
                {"phase",
                 materialPhaseName(materialPassPhase(route.pass_contract))},
            };
        }
        metadata["material_routing"] = {
            {"policy",
             materialRoutingPolicyName(pipeline.material_routing->policy)},
            {"routes", std::move(routes)},
        };
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
