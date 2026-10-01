#include "featurecompose.hpp"
#include "rendergraphcomposition.hpp"
#include "featurejitter.hpp"
#include "materialscreeninput.hpp"
#include "renderresourcename.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace Pelican {

namespace {

constexpr std::string_view feature_schema = "pelican.render_feature";
constexpr int supported_feature_version = 1;
constexpr int color_format_resolver_version = 2;
constexpr std::string_view feature_parameters_schema = "pelican.render_feature_parameters";
constexpr int supported_feature_parameters_version = 1;
constexpr std::array<std::string_view, 8> canonical_anchors = {
    "sprite", "post_main", "tonemap", "post_ldr", "pelican_ui", "debug_draw", "debug_text", "imgui"};

std::unordered_set<std::string> collectRenderTargetNames(const nlohmann::json &config);

std::string canonicalAnchorNodeName(std::string_view anchor) {
    return "__anchor_" + std::string{anchor};
}

} // namespace

bool isCanonicalRenderPassAnchor(std::string_view name) {
    return std::find(canonical_anchors.begin(), canonical_anchors.end(),
                     name) != canonical_anchors.end();
}

namespace {

bool isIdentifier(std::string_view value) {
    if (value.empty() || (std::isalpha(static_cast<unsigned char>(value.front())) == 0 &&
                          value.front() != '_')) {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
    });
}

std::string requireStringField(const nlohmann::json &json, std::string_view field_name,
                               std::string_view context) {
    const auto it = json.find(field_name);
    if (it == json.end() || !it->is_string()) {
        throw std::runtime_error(std::string{context} + " requires string field: " +
                                 std::string{field_name});
    }
    return it->get<std::string>();
}

const nlohmann::json &requireArrayField(const nlohmann::json &json, std::string_view field_name,
                                        std::string_view context) {
    const auto it = json.find(field_name);
    if (it == json.end() || !it->is_array()) {
        throw std::runtime_error(std::string{context} + " requires array field: " +
                                 std::string{field_name});
    }
    return it.value();
}

std::vector<std::string> parseStringArray(const nlohmann::json &json, std::string_view field_name,
                                          std::string_view context) {
    const auto &array = requireArrayField(json, field_name, context);
    std::vector<std::string> values;
    values.reserve(array.size());
    for (const auto &entry : array) {
        if (!entry.is_string()) {
            throw std::runtime_error(std::string{context} + " requires string array field: " +
                                     std::string{field_name});
        }
        values.push_back(entry.get<std::string>());
    }
    return values;
}

void appendUnique(std::vector<std::string> &values, const std::string &value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void appendUnique(std::vector<std::string> &values, const std::vector<std::string> &more_values) {
    for (const auto &value : more_values) {
        appendUnique(values, value);
    }
}

std::string validateFeatureEnvelope(const nlohmann::json &feature, std::string_view ref) {
    if (!feature.is_object()) {
        throw std::runtime_error("render feature must be an object: " + std::string{ref});
    }
    if (feature.value("schema", std::string{}) != feature_schema) {
        throw std::runtime_error("render feature schema is not supported: " + std::string{ref});
    }
    if (!feature.contains("version") || !feature.at("version").is_number_integer()) {
        throw std::runtime_error("render feature requires numeric version: " + std::string{ref});
    }
    if (feature.at("version").get<int>() != supported_feature_version) {
        throw std::runtime_error("render feature version is not supported: " + std::string{ref});
    }
    // G-4: which route automatic materials take is a property of the whole
    // project (read once at engine start), not of one composable feature.
    if (feature.contains("material_route_default")) {
        throw std::runtime_error(
            "render feature cannot declare material_route_default; set it "
            "on the rendering config root: " +
            std::string{ref});
    }
    return requireStringField(feature, "name", "render feature");
}

nlohmann::json loadFeatureJson(std::string_view ref,
                               const RenderFeatureComposeDependencies &dependencies) {
    if (!dependencies.load_feature_json) {
        throw std::runtime_error("render feature loader is not configured: " + std::string{ref});
    }
    return nlohmann::json::parse(dependencies.load_feature_json(ref));
}

struct FeatureInstance {
    std::string ref;
    nlohmann::json parameters = nlohmann::json::object();
    // LP-0d: {"<pass or compute task of the feature>": "<graph node or
    // graph.loop id>"}, in the order of the pass names.
    std::vector<std::pair<std::string, std::string>> pass_replacements;
};

std::vector<FeatureInstance> parseFeatureInstances(const nlohmann::json &config) {
    if (!config.contains("features")) {
        return {};
    }
    const auto &features = config.at("features");
    if (!features.is_array()) {
        throw std::runtime_error("rendering config features must be an array");
    }

    std::vector<FeatureInstance> instances;
    instances.reserve(features.size());
    for (const auto &entry : features) {
        if (entry.is_string()) {
            instances.push_back(FeatureInstance{entry.get<std::string>(), nlohmann::json::object()});
            continue;
        }
        if (!entry.is_object()) {
            throw std::runtime_error(
                "rendering config features entries must be strings or {ref, parameters} objects");
        }
        for (auto field = entry.begin(); field != entry.end(); ++field) {
            if (field.key() != "ref" && field.key() != "parameters" && field.key() != "pass_replacements") {
                throw std::runtime_error("render feature instance has unknown field: " + field.key());
            }
        }
        const auto ref = requireStringField(entry, "ref", "render feature instance");
        const auto parameters = entry.value("parameters", nlohmann::json::object());
        if (!parameters.is_object()) {
            throw std::runtime_error("render feature instance parameters must be an object: " + ref);
        }
        FeatureInstance instance{ref, parameters, {}};
        if (const auto replacements = entry.find("pass_replacements"); replacements != entry.end()) {
            if (!replacements->is_object()) {
                throw std::runtime_error(
                    "render feature instance pass_replacements must be an object of pass names to graph node ids: " +
                    ref);
            }
            for (auto replacement = replacements->begin(); replacement != replacements->end(); ++replacement) {
                if (replacement.key().empty() || !replacement.value().is_string() ||
                    replacement.value().get_ref<const std::string &>().empty()) {
                    throw std::runtime_error(
                        "render feature instance pass_replacements must map a pass name to a graph node id: " + ref);
                }
                instance.pass_replacements.emplace_back(replacement.key(), replacement.value().get<std::string>());
            }
        }
        instances.push_back(std::move(instance));
    }
    return instances;
}

nlohmann::json &ensureArray(nlohmann::json &config, std::string_view field_name) {
    const auto field = std::string{field_name};
    if (!config.contains(field)) {
        config[field] = nlohmann::json::array();
    }
    if (!config.at(field).is_array()) {
        throw std::runtime_error("rendering config " + field + " must be an array");
    }
    return config.at(field);
}

bool stringListContains(const nlohmann::json &value, std::string_view needle) {
    if (value.is_string()) {
        return value.get<std::string>() == needle;
    }
    if (!value.is_array()) {
        return false;
    }
    return std::any_of(value.begin(), value.end(), [needle](const auto &entry) {
        return entry.is_string() && entry.get<std::string>() == needle;
    });
}

bool passWritesSwapchain(const nlohmann::json &pass) {
    return pass.contains("output") && pass.at("output").is_object() &&
           pass.at("output").contains("color") &&
           stringListContains(pass.at("output").at("color"), "swapchain");
}

void replaceSwapchainAlias(nlohmann::json &value) {
    if (value.is_string()) {
        if (value.get<std::string>() == "swapchain") {
            value = "display";
        }
        return;
    }
    if (!value.is_array()) {
        return;
    }
    for (auto &entry : value) {
        replaceSwapchainAlias(entry);
    }
}

std::string inferFormatClass(const nlohmann::json &target) {
    const auto name = target.value("name", std::string{});
    const auto format = target.value("format", std::string{});
    if (name == "display") {
        return "display";
    }
    if (format.rfind("D", 0) == 0 || format == "R8_UNORM" ||
        name.find("normal") != std::string::npos || name.find("material") != std::string::npos ||
        name.find("worldpos") != std::string::npos || name.find("ssao") != std::string::npos ||
        name.find("depth") != std::string::npos || name.find("shadow") != std::string::npos) {
        return "data";
    }
    if (name == "gbuffer_albedo" || name == "g_emissive" || name == "lit_color") {
        return "scene";
    }
    return "explicit(" + format + ")";
}

void addFormatClassesAndDisplay(nlohmann::json &config) {
    auto &targets = ensureArray(config, "render_targets");
    bool has_display = false;
    for (auto &target : targets) {
        if (!target.is_object()) {
            throw std::runtime_error("render_targets entries must be objects");
        }
        if (target.value("name", std::string{}) == "display") {
            has_display = true;
        }
        if (!target.contains("format_class")) {
            target["format_class"] = inferFormatClass(target);
        }
        if (!target.contains("role")) {
            const auto format_class = target.at("format_class").get<std::string>();
            target["role"] = (format_class == "scene" || format_class == "display") ? "color" : "data";
        }
    }
    if (has_display) {
        throw std::runtime_error("Render target name is reserved by the canonical color pipeline: display");
    }
    targets.push_back({
        {"name", "display"},
        {"extent_scale", 1.0},
        {"format", "B8G8R8A8_SRGB"},
        {"format_class", "display"},
        {"role", "color"},
        {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"})},
    });
}

size_t canonicalBucket(const nlohmann::json &pass, bool hdr_enabled) {
    const auto type = pass.value("type", std::string{});
    if (type == "ui") {
        return 4;
    }
    if (type == "debug_draw" || type == "gizmo") {
        return 5;
    }
    if (type == "debug_text") {
        return 6;
    }
    if (pass.contains("canonical_anchor") && pass.at("canonical_anchor").is_string()) {
        const auto requested = pass.at("canonical_anchor").get<std::string>();
        const auto found = std::find(canonical_anchors.begin(), canonical_anchors.end(), requested);
        if (found == canonical_anchors.end()) {
            throw std::runtime_error("Unknown canonical pass anchor: " + requested);
        }
        return static_cast<size_t>(std::distance(canonical_anchors.begin(), found));
    }
    const auto name = pass.value("name", std::string{});
    if (name == "lighting_pass") {
        return canonical_anchors.size();
    }
    if (passWritesSwapchain(pass)) {
        return hdr_enabled ? 1 : 3;
    }
    return canonical_anchors.size(); // scene passes, before post_main
}

nlohmann::json makeAnchor(std::string_view anchor) {
    return {
        {"name", canonicalAnchorNodeName(anchor)},
        {"type", "canonical_anchor"},
        {"anchor", anchor},
    };
}

nlohmann::json makeOutputTransform() {
    return {
        {"name", "output_transform"},
        {"type", "output_transform"},
        {"input", nlohmann::json::array({"display"})},
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "engine://fullscreen"},
                    {"fragment", "engine://output_transform"}}},
        {"color_load_op", "dont_care"},
    };
}

void prepareHdrSceneOutput(nlohmann::json &config) {
    auto &targets = ensureArray(config, "render_targets");
    targets.push_back({
        {"name", "scene_ldr_in"},
        {"extent_scale", 1.0},
        {"format", "B8G8R8A8_UNORM"},
        {"format_class", "scene"},
        {"role", "color"},
        {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
    });

    nlohmann::json *scene_terminal = nullptr;
    for (auto &pass_set : ensureArray(config, "rendering_passes")) {
        for (auto &pass : pass_set.at("passes")) {
            const auto type = pass.value("type", std::string{});
            if (type != "ui" && type != "debug_draw" && type != "gizmo" &&
                type != "debug_text" &&
                passWritesSwapchain(pass)) {
                scene_terminal = &pass;
            }
        }
    }
    if (scene_terminal == nullptr) {
        throw std::runtime_error("HDR canonical pipeline requires a scene pass that writes swapchain/display");
    }
    replaceSwapchainAlias(scene_terminal->at("output").at("color"));
    auto &output = scene_terminal->at("output").at("color");
    if (output.is_string()) {
        output = "scene_ldr_in";
    } else {
        for (auto &entry : output) {
            if (entry == "display") {
                entry = "scene_ldr_in";
            }
        }
    }
}

void appendAfter(nlohmann::json &node, const std::string &dependency) {
    if (!node.contains("after")) {
        node["after"] = nlohmann::json::array();
    } else if (node.at("after").is_string()) {
        node["after"] = nlohmann::json::array({node.at("after")});
    }
    if (!node.at("after").is_array()) {
        throw std::runtime_error("canonical frame pass after must be a string or array");
    }
    auto &after = node.at("after");
    if (std::find(after.begin(), after.end(), dependency) == after.end()) {
        after.push_back(dependency);
    }
}

bool hasExplicitRelation(const nlohmann::json &lhs, const std::string &rhs_name) {
    return (lhs.contains("after") && stringListContains(lhs.at("after"), rhs_name)) ||
           (lhs.contains("before") && stringListContains(lhs.at("before"), rhs_name));
}

void enforceCanonicalOrder(nlohmann::json &passes) {
    std::string active_anchor;
    std::vector<std::string> active_passes;
    nlohmann::json *last_active_pass = nullptr;
    for (auto &pass : passes) {
        const auto type = pass.value("type", std::string{});
        const auto name = requireStringField(pass, "name", "canonical frame node");
        if (type == "canonical_anchor") {
            if (!active_anchor.empty()) {
                appendAfter(pass, active_anchor);
            }
            for (const auto &active_pass : active_passes) {
                appendAfter(pass, active_pass);
            }
            active_anchor = name;
            active_passes.clear();
            last_active_pass = nullptr;
        } else if (type == "output_transform") {
            if (!active_anchor.empty()) {
                appendAfter(pass, active_anchor);
            }
            for (const auto &active_pass : active_passes) {
                appendAfter(pass, active_pass);
            }
        } else {
            if (!active_anchor.empty()) {
                appendAfter(pass, active_anchor);
            }
            if (last_active_pass != nullptr) {
                const auto previous_name = last_active_pass->at("name").get<std::string>();
                if (!hasExplicitRelation(pass, previous_name) &&
                    !hasExplicitRelation(*last_active_pass, name)) {
                    appendAfter(pass, previous_name);
                }
            }
            active_passes.push_back(name);
            last_active_pass = &pass;
        }
    }
}

void canonicalizePasses(nlohmann::json &config, bool hdr_enabled) {
    auto &rendering_passes = ensureArray(config, "rendering_passes");
    for (auto &pass_set : rendering_passes) {
        auto &passes = pass_set.at("passes");
        if (!passes.is_array()) {
            throw std::runtime_error("rendering pass requires passes array");
        }
        std::array<nlohmann::json, canonical_anchors.size()> buckets;
        for (auto &bucket : buckets) {
            bucket = nlohmann::json::array();
        }
        nlohmann::json scene_passes = nlohmann::json::array();
        std::unordered_set<std::string> names;
        for (auto &pass : passes) {
            const auto name = requireStringField(pass, "name", "pass");
            if (!names.insert(name).second || name == "output_transform" ||
                name.rfind("__anchor_", 0) == 0) {
                throw std::runtime_error("Pass name collides with canonical frame node: " + name);
            }
            const auto bucket = canonicalBucket(pass, hdr_enabled);
            if (bucket == canonical_anchors.size()) {
                scene_passes.push_back(pass);
            } else {
                buckets[bucket].push_back(pass);
            }
        }

        nlohmann::json canonical = std::move(scene_passes);
        for (size_t i = 0; i < canonical_anchors.size(); ++i) {
            canonical.push_back(makeAnchor(canonical_anchors[i]));
            for (auto &pass : buckets[i]) {
                canonical.push_back(std::move(pass));
            }
        }
        canonical.push_back(makeOutputTransform());
        passes = std::move(canonical);
    }
}

void retargetSwapchainAliases(nlohmann::json &config) {
    for (auto &pass_set : ensureArray(config, "rendering_passes")) {
        for (auto &pass : pass_set.at("passes")) {
            if (pass.value("type", std::string{}) == "output_transform") {
                continue;
            }
            if (pass.contains("output") && pass.at("output").is_object() &&
                pass.at("output").contains("color")) {
                replaceSwapchainAlias(pass.at("output").at("color"));
            }
        }
    }
}

void enforceTerminalAfterComputeTasks(nlohmann::json &config) {
    std::vector<std::string> compute_tasks;
    if (config.contains("compute_tasks")) {
        for (const auto &task : ensureArray(config, "compute_tasks")) {
            compute_tasks.push_back(requireStringField(task, "name", "compute task"));
        }
    }
    for (auto &pass_set : ensureArray(config, "rendering_passes")) {
        for (auto &pass : pass_set.at("passes")) {
            if (pass.value("type", std::string{}) != "output_transform") {
                continue;
            }
            for (const auto &task : compute_tasks) {
                appendAfter(pass, task);
            }
        }
    }
}

void materializeSnapshots(nlohmann::json &config) {
    if (!config.contains("snapshots")) {
        return;
    }
    auto &snapshots = config.at("snapshots");
    if (!snapshots.is_array()) {
        throw std::runtime_error("rendering config snapshots must be an array");
    }
    if (snapshots.size() > 1) {
        const auto second_name = snapshots.at(1).is_object()
                                     ? snapshots.at(1).value("name", std::string{"<unnamed>"})
                                     : std::string{"<invalid>"};
        throw std::runtime_error("snapshot '" + second_name +
                                 "' is unsupported: v1 permits exactly one opaque snapshot; "
                                 "sequential refraction is not supported");
    }
    if (snapshots.empty()) {
        return;
    }

    auto &snapshot = snapshots.front();
    if (!snapshot.is_object()) {
        throw std::runtime_error("rendering config snapshots entries must be objects");
    }
    const auto name = requireStringField(snapshot, "name", "snapshot");
    const auto authored_after = requireStringField(snapshot, "after", "snapshot '" + name + "'");
    if (!isIdentifier(name)) {
        throw std::runtime_error("snapshot '" + name + "' must be a shader identifier");
    }

    auto target_names = collectRenderTargetNames(config);
    if (target_names.contains(name)) {
        throw std::runtime_error("snapshot '" + name + "' collides with render target name");
    }

    const auto anchor_it = std::find(canonical_anchors.begin(), canonical_anchors.end(), authored_after);
    const auto after = anchor_it == canonical_anchors.end()
                           ? authored_after
                           : canonicalAnchorNodeName(authored_after);
    const auto post_ldr = canonicalAnchorNodeName("post_ldr");
    const auto transparent_begin = canonicalAnchorNodeName("pelican_ui");
    const auto node = "__snapshot_" + name;

    for (auto &pass_set : ensureArray(config, "rendering_passes")) {
        auto &passes = pass_set.at("passes");
        if (std::any_of(passes.begin(), passes.end(), [&](const auto &pass) {
                return pass.value("name", std::string{}) == node;
            })) {
            throw std::runtime_error("snapshot '" + name + "' node collides with pass name: " + node);
        }
        auto found = std::find_if(passes.begin(), passes.end(), [&](const auto &pass) {
            return pass.value("name", std::string{}) == after;
        });
        if (found == passes.end()) {
            throw std::runtime_error("snapshot '" + name + "' copy point was not found: " +
                                     authored_after);
        }
        const auto post_ldr_it = std::find_if(passes.begin(), passes.end(), [&](const auto &pass) {
            return pass.value("name", std::string{}) == post_ldr;
        });
        const auto transparent_it = std::find_if(passes.begin(), passes.end(), [&](const auto &pass) {
            return pass.value("name", std::string{}) == transparent_begin;
        });
        if (found < post_ldr_it || found >= transparent_it) {
            throw std::runtime_error("snapshot '" + name + "' after '" + authored_after +
                                     "' is not the opaque post_ldr region; transparent-after "
                                     "snapshots and sequential refraction are unsupported in v1");
        }

        nlohmann::json copy{
            {"name", node},
            {"type", "snapshot_copy"},
            {"source", "display"},
            {"destination", name},
            {"snapshot", name},
            {"snapshot_after", authored_after},
        };
        passes.insert(found + 1, std::move(copy));
    }

    ensureArray(config, "render_targets").push_back({
        {"name", name},
        {"extent_scale", 1.0},
        {"format", "B8G8R8A8_SRGB"},
        {"format_class", "display"},
        {"role", "color"},
        {"usage", nlohmann::json::array({"TRANSFER_DST", "SAMPLED"})},
    });
}

void initializeCanonicalColorPipeline(nlohmann::json &config, bool hdr_enabled) {
    if (config.contains("resolver_version") &&
        (!config.at("resolver_version").is_number_integer() ||
         config.at("resolver_version").get<int>() != color_format_resolver_version)) {
        throw std::runtime_error("Only rendering resolver_version 2 is supported");
    }
    config["resolver_version"] = color_format_resolver_version;
    addFormatClassesAndDisplay(config);
    if (hdr_enabled) {
        prepareHdrSceneOutput(config);
    }
    canonicalizePasses(config, hdr_enabled);
}

std::unordered_set<std::string> collectRenderTargetNames(const nlohmann::json &config) {
    std::unordered_set<std::string> names;
    if (!config.contains("render_targets")) {
        return names;
    }
    const auto &targets = config.at("render_targets");
    if (!targets.is_array()) {
        throw std::runtime_error("render_targets must be an array");
    }
    for (const auto &target : targets) {
        if (!target.is_object()) {
            throw std::runtime_error("render_targets entries must be objects");
        }
        const auto name = requireStringField(target, "name", "render target");
        validateAuthoredRenderResourceName(
            name, "render target");
        if (!names.insert(name).second) {
            throw std::runtime_error("Duplicate render target name: " + name);
        }
    }
    return names;
}

std::unordered_set<std::string> collectPassNames(const nlohmann::json &config) {
    std::unordered_set<std::string> names;
    if (!config.contains("rendering_passes")) {
        return names;
    }
    const auto &rendering_passes = config.at("rendering_passes");
    if (!rendering_passes.is_array()) {
        throw std::runtime_error("rendering_passes must be an array");
    }
    for (const auto &pass_set : rendering_passes) {
        const auto &passes = requireArrayField(pass_set, "passes", "rendering pass");
        for (const auto &pass : passes) {
            const auto name = requireStringField(pass, "name", "pass");
            names.insert(name);
        }
    }
    return names;
}

std::unordered_set<std::string> collectBufferNames(const nlohmann::json &config) {
    std::unordered_set<std::string> names;
    if (!config.contains("buffers")) {
        return names;
    }
    const auto &buffers = config.at("buffers");
    if (!buffers.is_array()) {
        throw std::runtime_error("buffers must be an array");
    }
    for (const auto &buffer : buffers) {
        std::string name;
        if (buffer.is_string()) {
            name = buffer.get<std::string>();
        } else if (buffer.is_object()) {
            name = requireStringField(buffer, "name", "buffer");
        } else {
            throw std::runtime_error("buffers entries must be strings or objects");
        }
        validateAuthoredRenderResourceName(name, "buffer");
        if (!names.insert(name).second) {
            throw std::runtime_error("Duplicate buffer name: " + name);
        }
    }
    return names;
}

std::unordered_set<std::string> collectComputeTaskNames(const nlohmann::json &config) {
    std::unordered_set<std::string> names;
    if (!config.contains("compute_tasks")) {
        return names;
    }
    const auto &tasks = config.at("compute_tasks");
    if (!tasks.is_array()) {
        throw std::runtime_error("compute_tasks must be an array");
    }
    for (const auto &task : tasks) {
        if (!task.is_object()) {
            throw std::runtime_error("compute_tasks entries must be objects");
        }
        names.insert(requireStringField(task, "name", "compute task"));
    }
    return names;
}

nlohmann::json *findRenderTarget(nlohmann::json &config, const std::string &name) {
    auto &targets = ensureArray(config, "render_targets");
    for (auto &target : targets) {
        if (target.is_object() && target.value("name", std::string{}) == name) {
            return &target;
        }
    }
    return nullptr;
}

struct RenderTargetParameterDeclaration {
    std::string name;
    bool required = true;
    std::optional<std::string> default_target;
    nlohmann::json constraints;
};

enum class ScalarParameterType {
    floating,
    integer,
    boolean,
};

// Where a scalar's value reaches the shader.  `define` bakes it into the
// compiled shader, so changing it recompiles; `buffer` seeds the feature's own
// runtime_f32 frame-graph buffer, which the common graph can then overwrite
// every frame through `render.scalar`.
enum class ScalarParameterBinding {
    define,
    buffer,
};

struct ScalarParameterDeclaration {
    std::string name;
    ScalarParameterType type = ScalarParameterType::floating;
    nlohmann::json default_value;
    std::optional<std::pair<nlohmann::json, nlohmann::json>> range;
    bool shader_define = true;
    ScalarParameterBinding binding = ScalarParameterBinding::define;
};

// The frame-graph buffer a `binding: buffer` scalar initializes. The name is
// derived, never authored twice: `render.scalar{target}` names the same string.
std::string featureScalarBufferName(const std::string &feature_name,
                                    const std::string &scalar_name) {
    return feature_name + "." + scalar_name;
}

enum class ShaderAssetParameterStage {
    vertex,
    fragment,
    compute,
};

struct ShaderAssetParameterDeclaration {
    std::string name;
    ShaderAssetParameterStage stage =
        ShaderAssetParameterStage::compute;
    std::string default_reference;
};

struct FeatureParameterDeclarations {
    std::vector<RenderTargetParameterDeclaration> render_targets;
    std::vector<ScalarParameterDeclaration> scalars;
    std::vector<ShaderAssetParameterDeclaration> shader_assets;
};

[[noreturn]] void throwBindingError(const std::string &feature_name,
                                    const std::string &parameter_name,
                                    const std::string &target_name,
                                    const std::string &detail) {
    throw std::runtime_error("render feature '" + feature_name + "' parameter '" +
                             parameter_name + "' target '" + target_name + "': " + detail);
}

[[noreturn]] void throwParameterError(const std::string &feature_name,
                                      const std::string &parameter_name,
                                      const std::string &detail) {
    throw std::runtime_error("render feature '" + feature_name + "' parameter '" +
                             parameter_name + "': " + detail);
}

std::int64_t scalarIntegerValue(const nlohmann::json &value,
                                const std::string &feature_name,
                                const std::string &parameter_name,
                                std::string_view field) {
    std::int64_t result;
    if (value.is_number_unsigned()) {
        const auto unsigned_value = value.get<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throwParameterError(feature_name, parameter_name,
                                std::string{field} + " is outside the int range");
        }
        result = static_cast<std::int64_t>(unsigned_value);
    } else if (value.is_number_integer()) {
        result = value.get<std::int64_t>();
    } else {
        throwParameterError(feature_name, parameter_name,
                            std::string{field} + " must have type int");
    }
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) {
        throwParameterError(feature_name, parameter_name,
                            std::string{field} + " is outside the shader int range");
    }
    return result;
}

double scalarFloatValue(const nlohmann::json &value,
                        const std::string &feature_name,
                        const std::string &parameter_name,
                        std::string_view field) {
    if (!value.is_number()) {
        throwParameterError(feature_name, parameter_name,
                            std::string{field} + " must have type float");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result) ||
        std::abs(result) > static_cast<double>(std::numeric_limits<float>::max())) {
        throwParameterError(feature_name, parameter_name,
                            std::string{field} + " is outside the finite float range");
    }
    return result;
}

void validateScalarValue(const nlohmann::json &value,
                         const ScalarParameterDeclaration &declaration,
                         const std::string &feature_name,
                         std::string_view field) {
    switch (declaration.type) {
    case ScalarParameterType::floating: {
        const auto numeric = scalarFloatValue(value, feature_name, declaration.name, field);
        if (declaration.range) {
            const auto minimum = declaration.range->first.get<double>();
            const auto maximum = declaration.range->second.get<double>();
            if (numeric < minimum || numeric > maximum) {
                throwParameterError(feature_name, declaration.name,
                                    std::string{field} + " is outside declared range [" +
                                        declaration.range->first.dump() + ", " +
                                        declaration.range->second.dump() + "]");
            }
        }
        return;
    }
    case ScalarParameterType::integer: {
        const auto numeric = scalarIntegerValue(value, feature_name, declaration.name, field);
        if (declaration.range) {
            const auto minimum = scalarIntegerValue(declaration.range->first, feature_name,
                                                    declaration.name, "range minimum");
            const auto maximum = scalarIntegerValue(declaration.range->second, feature_name,
                                                    declaration.name, "range maximum");
            if (numeric < minimum || numeric > maximum) {
                throwParameterError(feature_name, declaration.name,
                                    std::string{field} + " is outside declared range [" +
                                        declaration.range->first.dump() + ", " +
                                        declaration.range->second.dump() + "]");
            }
        }
        return;
    }
    case ScalarParameterType::boolean:
        if (!value.is_boolean()) {
            throwParameterError(feature_name, declaration.name,
                                std::string{field} + " must have type bool");
        }
        return;
    }
}

std::string upperIdentifier(std::string_view value, const std::string &feature_name) {
    if (!isIdentifier(value)) {
        throw std::runtime_error("render feature '" + feature_name +
                                 "' name must be an identifier when scalar parameters are declared");
    }
    std::string result{value};
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    });
    return result;
}

// X-1: a vendored package feature carries its namespace in the document name
// (`acme.glowkit.glow`), because every place that compares feature names reads
// that field.  The shader define prefix is the one consumer that needs an
// identifier, so the dots become underscores here:
// PELICAN_FEATURE_ACME_GLOWKIT_GLOW_.
std::string upperFeatureNameIdentifier(std::string_view value,
                                       const std::string &feature_name) {
    std::string result;
    result.reserve(value.size());
    std::size_t begin = 0;
    while (true) {
        const auto dot = value.find('.', begin);
        const auto segment = value.substr(
            begin, dot == std::string_view::npos ? std::string_view::npos
                                                 : dot - begin);
        if (!isIdentifier(segment)) {
            throw std::runtime_error(
                "render feature '" + feature_name +
                "' name must be dot-separated identifiers when scalar parameters are declared");
        }
        result.append(segment);
        if (dot == std::string_view::npos) break;
        result.push_back('_');
        begin = dot + 1;
    }
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    });
    return result;
}

FeatureParameterDeclarations parseFeatureParameters(
    const nlohmann::json &feature, const std::string &feature_name) {
    if (!feature.contains("parameters")) {
        return {};
    }
    const auto &parameters = feature.at("parameters");
    if (!parameters.is_object()) {
        throw std::runtime_error("render feature '" + feature_name +
                                 "' parameters declaration must be an object");
    }
    for (auto field = parameters.begin(); field != parameters.end(); ++field) {
        if (field.key() != "schema" && field.key() != "version" &&
            field.key() != "render_targets" && field.key() != "scalars" &&
            field.key() != "shader_assets") {
            throw std::runtime_error("render feature '" + feature_name +
                                     "' parameters declaration has unknown field: " + field.key());
        }
    }
    if (parameters.value("schema", std::string{}) != feature_parameters_schema) {
        throw std::runtime_error("render feature '" + feature_name +
                                 "' parameter schema is not supported");
    }
    if (!parameters.contains("version") || !parameters.at("version").is_number_integer() ||
        parameters.at("version").get<int>() != supported_feature_parameters_version) {
        throw std::runtime_error("render feature '" + feature_name +
                                 "' parameter version is not supported");
    }
    FeatureParameterDeclarations declarations;
    std::unordered_set<std::string> names;
    const auto targets = parameters.contains("render_targets")
                             ? parameters.at("render_targets")
                             : nlohmann::json::array();
    if (!targets.is_array()) {
        throw std::runtime_error("render feature parameters: " + feature_name +
                                 " requires array field: render_targets");
    }
    declarations.render_targets.reserve(targets.size());
    for (const auto &target : targets) {
        if (!target.is_object()) {
            throw std::runtime_error("render feature '" + feature_name +
                                     "' render-target parameters must be objects");
        }
        for (auto field = target.begin(); field != target.end(); ++field) {
            if (field.key() != "name" && field.key() != "required" &&
                field.key() != "default" && field.key() != "role" &&
                field.key() != "format_class" && field.key() != "extent_scale" &&
                field.key() != "width" && field.key() != "height" &&
                field.key() != "sample_count" && field.key() != "usage") {
                throw std::runtime_error("render feature '" + feature_name +
                                         "' render-target parameter has unknown field: " +
                                         field.key());
            }
        }
        const auto name = requireStringField(target, "name", "render-target parameter");
        if (!isIdentifier(name) || !names.insert(name).second) {
            throw std::runtime_error("render feature '" + feature_name +
                                     "' has invalid or duplicate render-target parameter: " + name);
        }
        if (target.contains("required") && !target.at("required").is_boolean()) {
            throwBindingError(feature_name, name, "<declaration>", "required must be boolean");
        }
        std::optional<std::string> default_target;
        if (target.contains("default")) {
            if (!target.at("default").is_string()) {
                throwBindingError(feature_name, name, "<declaration>",
                                  "default must name a render target");
            }
            default_target = target.at("default").get<std::string>();
        }
        declarations.render_targets.push_back(RenderTargetParameterDeclaration{
            name, target.value("required", true), std::move(default_target), target});
    }

    const auto scalars = parameters.contains("scalars")
                             ? parameters.at("scalars")
                             : nlohmann::json::array();
    if (!scalars.is_array()) {
        throw std::runtime_error("render feature parameters: " + feature_name +
                                 " requires array field: scalars");
    }
    declarations.scalars.reserve(scalars.size());
    std::unordered_set<std::string> define_components;
    for (const auto &scalar : scalars) {
        if (!scalar.is_object()) {
            throw std::runtime_error("render feature '" + feature_name +
                                     "' scalar parameters must be objects");
        }
        for (auto field = scalar.begin(); field != scalar.end(); ++field) {
            if (field.key() != "name" && field.key() != "type" &&
                field.key() != "range" && field.key() != "default" &&
                field.key() != "shader_define" && field.key() != "binding") {
                throw std::runtime_error("render feature '" + feature_name +
                                         "' scalar parameter has unknown field: " + field.key());
            }
        }
        const auto name = requireStringField(scalar, "name", "scalar parameter");
        if (!isIdentifier(name) || !names.insert(name).second) {
            throw std::runtime_error("render feature '" + feature_name +
                                     "' has invalid or duplicate parameter: " + name);
        }
        if (!define_components.insert(upperIdentifier(name, feature_name)).second) {
            throwParameterError(feature_name, name,
                                "name collides after shader define uppercasing");
        }
        const auto type_name = requireStringField(scalar, "type", "scalar parameter: " + name);
        ScalarParameterType type;
        if (type_name == "float") {
            type = ScalarParameterType::floating;
        } else if (type_name == "int") {
            type = ScalarParameterType::integer;
        } else if (type_name == "bool") {
            type = ScalarParameterType::boolean;
        } else {
            throwParameterError(feature_name, name, "unknown scalar type: " + type_name);
        }
        if (!scalar.contains("default")) {
            throwParameterError(feature_name, name, "declaration requires default");
        }
        if (scalar.contains("shader_define") &&
            !scalar.at("shader_define").is_boolean()) {
            throwParameterError(
                feature_name, name,
                "shader_define must have type bool");
        }
        auto binding = ScalarParameterBinding::define;
        if (scalar.contains("binding")) {
            const auto binding_name =
                requireStringField(scalar, "binding", "scalar parameter: " + name);
            if (binding_name == "define") {
                binding = ScalarParameterBinding::define;
            } else if (binding_name == "buffer") {
                binding = ScalarParameterBinding::buffer;
            } else {
                throwParameterError(feature_name, name,
                                    "unknown scalar binding: " + binding_name);
            }
        }
        // A buffer-bound scalar reaches the shader through a resource port, so
        // it emits no define.  Asking for both is a contradiction, not a
        // combination: the define would freeze the value the buffer exists to
        // change.  The other direction (`binding: define` with
        // `shader_define: false`) keeps its 1c meaning — a scalar that only
        // feeds physical declarations such as `$resolution`.
        if (binding == ScalarParameterBinding::buffer) {
            if (scalar.value("shader_define", false)) {
                throwParameterError(
                    feature_name, name,
                    "binding 'buffer' cannot also emit a shader define");
            }
            if (type != ScalarParameterType::floating) {
                throwParameterError(
                    feature_name, name,
                    "binding 'buffer' requires scalar type float");
            }
        }

        std::optional<std::pair<nlohmann::json, nlohmann::json>> range;
        if (type == ScalarParameterType::boolean) {
            if (scalar.contains("range")) {
                throwParameterError(feature_name, name, "bool declaration must not have range");
            }
        } else {
            if (!scalar.contains("range") || !scalar.at("range").is_array() ||
                scalar.at("range").size() != 2) {
                throwParameterError(feature_name, name,
                                    "numeric declaration requires two-element range");
            }
            const auto &minimum = scalar.at("range").at(0);
            const auto &maximum = scalar.at("range").at(1);
            if (type == ScalarParameterType::floating) {
                const auto min_value = scalarFloatValue(minimum, feature_name, name,
                                                        "range minimum");
                const auto max_value = scalarFloatValue(maximum, feature_name, name,
                                                        "range maximum");
                if (min_value > max_value) {
                    throwParameterError(feature_name, name, "range minimum exceeds maximum");
                }
                range = std::pair{nlohmann::json(min_value), nlohmann::json(max_value)};
            } else {
                const auto min_value = scalarIntegerValue(minimum, feature_name, name,
                                                          "range minimum");
                const auto max_value = scalarIntegerValue(maximum, feature_name, name,
                                                          "range maximum");
                if (min_value > max_value) {
                    throwParameterError(feature_name, name, "range minimum exceeds maximum");
                }
                range = std::pair{nlohmann::json(min_value), nlohmann::json(max_value)};
            }
        }

        ScalarParameterDeclaration declaration{
            name, type, scalar.at("default"),
            std::move(range),
            scalar.value(
                "shader_define",
                binding == ScalarParameterBinding::define),
            binding};
        validateScalarValue(declaration.default_value, declaration, feature_name, "default");
        declarations.scalars.push_back(std::move(declaration));
    }

    const auto shader_assets =
        parameters.contains("shader_assets")
            ? parameters.at("shader_assets")
            : nlohmann::json::array();
    if (!shader_assets.is_array()) {
        throw std::runtime_error(
            "render feature parameters: " + feature_name +
            " requires array field: shader_assets");
    }
    declarations.shader_assets.reserve(
        shader_assets.size());
    for (const auto &asset : shader_assets) {
        if (!asset.is_object()) {
            throw std::runtime_error(
                "render feature '" + feature_name +
                "' shader asset parameters must be objects");
        }
        for (auto field = asset.begin();
             field != asset.end(); ++field) {
            if (field.key() != "name" &&
                field.key() != "stage" &&
                field.key() != "default") {
                throw std::runtime_error(
                    "render feature '" + feature_name +
                    "' shader asset parameter has unknown field: " +
                    field.key());
            }
        }
        const auto name = requireStringField(
            asset, "name", "shader asset parameter");
        if (!isIdentifier(name) ||
            !names.insert(name).second) {
            throw std::runtime_error(
                "render feature '" + feature_name +
                "' has invalid or duplicate parameter: " +
                name);
        }
        const auto stage_name = requireStringField(
            asset, "stage",
            "shader asset parameter: " + name);
        ShaderAssetParameterStage stage;
        if (stage_name == "vertex") {
            stage = ShaderAssetParameterStage::vertex;
        } else if (stage_name == "fragment") {
            stage = ShaderAssetParameterStage::fragment;
        } else if (stage_name == "compute") {
            stage = ShaderAssetParameterStage::compute;
        } else {
            throwParameterError(
                feature_name, name,
                "unknown shader asset stage: " +
                    stage_name);
        }
        const auto default_reference =
            requireStringField(
                asset, "default",
                "shader asset parameter: " + name);
        if (default_reference.empty() ||
            default_reference.front() == '$') {
            throwParameterError(
                feature_name, name,
                "default must be a concrete non-empty shader reference");
        }
        declarations.shader_assets.push_back(
            ShaderAssetParameterDeclaration{
                name, stage, default_reference});
    }
    return declarations;
}

std::string scalarDefineValue(const nlohmann::json &value,
                              const ScalarParameterDeclaration &declaration,
                              const std::string &feature_name) {
    switch (declaration.type) {
    case ScalarParameterType::floating: {
        const auto numeric = static_cast<float>(
            scalarFloatValue(value, feature_name, declaration.name, "value"));
        std::array<char, 32> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), numeric,
                                                std::chars_format::general,
                                                std::numeric_limits<float>::max_digits10);
        if (error != std::errc{}) {
            throwParameterError(feature_name, declaration.name,
                                "could not format float define value");
        }
        return std::string{buffer.data(), end};
    }
    case ScalarParameterType::integer:
        return std::to_string(scalarIntegerValue(value, feature_name, declaration.name, "value"));
    case ScalarParameterType::boolean:
        return value.get<bool>() ? "1" : "0";
    }
    throwParameterError(feature_name, declaration.name, "unknown scalar type");
}

void validateTargetBinding(const nlohmann::json &config,
                           const RenderTargetParameterDeclaration &declaration,
                           const std::string &target_name,
                           const std::string &feature_name) {
    const nlohmann::json *target = nullptr;
    for (const auto &candidate : config.at("render_targets")) {
        if (candidate.value("name", std::string{}) == target_name) {
            target = &candidate;
            break;
        }
    }
    if (target == nullptr) {
        throwBindingError(feature_name, declaration.name, target_name, "unknown render target");
    }

    const auto &constraints = declaration.constraints;
    const auto requireEqual = [&](std::string_view field, const nlohmann::json &actual) {
        if (constraints.contains(field) && constraints.at(field) != actual) {
            throwBindingError(feature_name, declaration.name, target_name,
                              std::string{field} + " is incompatible (expected " +
                                  constraints.at(field).dump() + ", got " + actual.dump() + ")");
        }
    };
    requireEqual("role", target->value("role", std::string{}));
    requireEqual("format_class", target->value("format_class", std::string{}));
    requireEqual("extent_scale", target->value("extent_scale", 1.0));
    requireEqual("width", target->value("width", 0));
    requireEqual("height", target->value("height", 0));
    requireEqual("sample_count", target->value("sample_count", 1));

    if (constraints.contains("usage")) {
        const auto required_usage = parseStringArray(constraints, "usage",
                                                     "render-target parameter: " + declaration.name);
        const auto actual_usage = parseStringArray(*target, "usage", "render target: " + target_name);
        for (const auto &usage : required_usage) {
            if (std::find(actual_usage.begin(), actual_usage.end(), usage) == actual_usage.end()) {
                throwBindingError(feature_name, declaration.name, target_name,
                                  "usage is incompatible; missing " + usage);
            }
        }
    }
}

std::string replaceBoundTargetReference(
    const std::string &authored,
    const std::unordered_map<std::string, std::string> &bindings,
    const std::string &feature_name) {
    if (authored.empty() || authored.front() != '$') {
        return authored;
    }
    constexpr std::string_view history_suffix = "@history";
    const bool history = authored.size() > history_suffix.size() &&
                         authored.ends_with(history_suffix);
    const auto end = history ? authored.size() - history_suffix.size() : authored.size();
    const auto parameter = authored.substr(1, end - 1);
    const auto found = bindings.find(parameter);
    if (found == bindings.end()) {
        throwBindingError(feature_name, parameter, "<unresolved>",
                          "placeholder has no resolved binding");
    }
    return found->second + (history ? std::string{history_suffix} : std::string{});
}

void replaceBoundTargetValue(nlohmann::json &value,
                             const std::unordered_map<std::string, std::string> &bindings,
                             const std::string &feature_name) {
    if (value.is_string()) {
        value = replaceBoundTargetReference(value.get<std::string>(), bindings, feature_name);
    } else if (value.is_array()) {
        for (auto &entry : value) {
            replaceBoundTargetValue(entry, bindings, feature_name);
        }
    }
}

void replaceBoundScalarValues(
    nlohmann::json &value,
    const std::unordered_map<std::string, nlohmann::json>
        &bindings) {
    if (value.is_string()) {
        const auto &text =
            value.get_ref<const std::string &>();
        if (text.size() > 1 && text.front() == '$') {
            const auto found =
                bindings.find(text.substr(1));
            if (found != bindings.end()) {
                value = found->second;
            }
        }
        return;
    }
    if (value.is_array()) {
        for (auto &entry : value) {
            replaceBoundScalarValues(
                entry, bindings);
        }
        return;
    }
    if (value.is_object()) {
        for (auto &entry : value.items()) {
            replaceBoundScalarValues(
                entry.value(), bindings);
        }
    }
}

std::string_view shaderAssetParameterStageName(
    ShaderAssetParameterStage stage) {
    switch (stage) {
    case ShaderAssetParameterStage::vertex:
        return "vertex";
    case ShaderAssetParameterStage::fragment:
        return "fragment";
    case ShaderAssetParameterStage::compute:
        return "compute";
    }
    return "unknown";
}

struct BoundShaderAsset {
    ShaderAssetParameterStage stage =
        ShaderAssetParameterStage::compute;
    std::string reference;
};

void replaceBoundShaderAssetValues(
    nlohmann::json &value,
    const std::unordered_map<std::string, BoundShaderAsset>
        &bindings,
    const std::string &feature_name,
    std::optional<ShaderAssetParameterStage>
        shader_slot = std::nullopt) {
    if (value.is_string()) {
        const auto &text =
            value.get_ref<const std::string &>();
        if (text.size() <= 1 || text.front() != '$') {
            return;
        }
        const auto found =
            bindings.find(text.substr(1));
        if (found == bindings.end()) {
            return;
        }
        if (!shader_slot) {
            throwParameterError(
                feature_name, found->first,
                "shader asset placeholder may only be used in a "
                "typed shader slot");
        }
        if (*shader_slot != found->second.stage) {
            throwParameterError(
                feature_name, found->first,
                "shader asset declares stage '" +
                    std::string{
                        shaderAssetParameterStageName(
                            found->second.stage)} +
                    "' but is used in a '" +
                    std::string{
                        shaderAssetParameterStageName(
                            *shader_slot)} +
                    "' slot");
        }
        value = found->second.reference;
        return;
    }
    if (value.is_array()) {
        for (auto &entry : value) {
            replaceBoundShaderAssetValues(
                entry, bindings, feature_name);
        }
        return;
    }
    if (!value.is_object()) {
        return;
    }

    for (auto &entry : value.items()) {
        if (entry.key() != "shader") {
            replaceBoundShaderAssetValues(
                entry.value(), bindings,
                feature_name);
            continue;
        }
        if (entry.value().is_string()) {
            replaceBoundShaderAssetValues(
                entry.value(), bindings,
                feature_name,
                ShaderAssetParameterStage::compute);
            continue;
        }
        if (!entry.value().is_object()) {
            replaceBoundShaderAssetValues(
                entry.value(), bindings,
                feature_name);
            continue;
        }
        for (auto &stage :
             entry.value().items()) {
            std::optional<ShaderAssetParameterStage>
                typed_stage;
            if (stage.key() == "vertex" ||
                stage.key() ==
                    "skinned_vertex") {
                typed_stage =
                    ShaderAssetParameterStage::vertex;
            } else if (
                stage.key() == "fragment") {
                typed_stage =
                    ShaderAssetParameterStage::fragment;
            } else if (
                stage.key() == "compute") {
                typed_stage =
                    ShaderAssetParameterStage::compute;
            }
            replaceBoundShaderAssetValues(
                stage.value(), bindings,
                feature_name, typed_stage);
        }
    }
}

// A materialized feature keeps the declarations it stays editable through and
// records the chosen values in `bound_parameters`.  Those values are the
// derived feature's own defaults: an instance override still wins, and a
// parameter nobody ever bound falls back to the declaration default.
// `materialized_document`, when asked for, receives the variant that keeps the
// scalar declarations and leaves their `$name` placeholders unresolved, so a
// later edit reaches the physical fields as well as the defines.
nlohmann::json bindFeatureParameters(const nlohmann::json &authored_feature,
                                     const nlohmann::json &instance_parameters,
                                     const nlohmann::json &config,
                                     const std::string &feature_name,
                                     nlohmann::json &resolved_parameters,
                                     std::vector<std::string> &scalar_defines,
                                     nlohmann::json *materialized_document = nullptr) {
    const nlohmann::json *bound_parameters = nullptr;
    if (const auto bound = authored_feature.find("bound_parameters"); bound != authored_feature.end()) {
        if (!bound->is_object())
            throw std::runtime_error("Materialized render feature '" + feature_name +
                "' requires a bound_parameters object");
        bound_parameters = &*bound;
    }
    const auto bound_value =
        [bound_parameters](const std::string &name) -> const nlohmann::json * {
        if (bound_parameters == nullptr) return nullptr;
        const auto found = bound_parameters->find(name);
        return found == bound_parameters->end() ? nullptr : &*found;
    };
    const auto declarations = parseFeatureParameters(authored_feature, feature_name);
    std::unordered_map<std::string, const RenderTargetParameterDeclaration *> by_name;
    for (const auto &declaration : declarations.render_targets) {
        by_name.emplace(declaration.name, &declaration);
    }
    std::unordered_map<std::string, const ScalarParameterDeclaration *> scalars_by_name;
    for (const auto &declaration : declarations.scalars) {
        scalars_by_name.emplace(declaration.name, &declaration);
    }
    std::unordered_map<
        std::string,
        const ShaderAssetParameterDeclaration *>
        shader_assets_by_name;
    for (const auto &declaration :
         declarations.shader_assets) {
        shader_assets_by_name.emplace(
            declaration.name, &declaration);
    }
    for (auto parameter = instance_parameters.begin(); parameter != instance_parameters.end(); ++parameter) {
        if (!by_name.contains(parameter.key()) &&
            !scalars_by_name.contains(parameter.key()) &&
            !shader_assets_by_name.contains(
                parameter.key())) {
            if (parameter.value().is_string()) {
                throwBindingError(feature_name, parameter.key(),
                                  parameter.value().get<std::string>(), "unknown parameter");
            }
            throwParameterError(feature_name, parameter.key(), "unknown parameter");
        }
        if (by_name.contains(parameter.key()) && !parameter.value().is_string()) {
            throwBindingError(feature_name, parameter.key(), "<non-string>",
                               "binding must name a render target");
        }
        if (shader_assets_by_name.contains(
                parameter.key()) &&
            (!parameter.value().is_string() ||
             parameter.value()
                 .get_ref<const std::string &>()
                 .empty())) {
            throwParameterError(
                feature_name, parameter.key(),
                "shader asset value must be a non-empty string reference");
        }
    }

    std::unordered_map<std::string, std::string> bindings;
    // Values bound at materialize time that no longer have a declaration (the
    // render-target and shader-asset bindings materialize consumes) stay
    // visible to the runtime through the resolved instance parameters.
    resolved_parameters = bound_parameters != nullptr ? *bound_parameters : nlohmann::json::object();
    for (const auto &declaration : declarations.render_targets) {
        std::optional<std::string> target_name;
        if (const auto bound = instance_parameters.find(declaration.name);
            bound != instance_parameters.end()) {
            target_name = bound->get<std::string>();
        } else if (const auto *recorded = bound_value(declaration.name);
                   recorded != nullptr && recorded->is_string()) {
            target_name = recorded->get<std::string>();
        } else {
            target_name = declaration.default_target;
        }
        if (!target_name) {
            if (declaration.required) {
                throwBindingError(feature_name, declaration.name, "<missing>",
                                  "required parameter is missing");
            }
            continue;
        }
        validateTargetBinding(config, declaration, *target_name, feature_name);
        bindings.emplace(declaration.name, *target_name);
        resolved_parameters[declaration.name] = *target_name;
    }

    const auto feature_define_prefix = declarations.scalars.empty()
                                           ? std::string{}
                                           : "PELICAN_FEATURE_" +
                                                 upperFeatureNameIdentifier(feature_name, feature_name) + "_";
    std::unordered_map<std::string, nlohmann::json>
        scalar_bindings;
    scalar_bindings.reserve(
        declarations.scalars.size());
    // name of the feature's runtime_f32 buffer -> the value it starts at.
    std::vector<std::pair<std::string, double>> buffer_scalar_values;
    for (const auto &declaration : declarations.scalars) {
        const auto supplied = instance_parameters.find(declaration.name);
        const auto *recorded = bound_value(declaration.name);
        const auto *origin = "default";
        const nlohmann::json *selected = &declaration.default_value;
        if (supplied != instance_parameters.end()) {
            selected = &supplied.value();
            origin = "value";
        } else if (recorded != nullptr) {
            selected = recorded;
            origin = "bound value";
        }
        const auto &value = *selected;
        validateScalarValue(value, declaration, feature_name, origin);
        resolved_parameters[declaration.name] = value;
        scalar_bindings.emplace(
            declaration.name, value);
        if (declaration.shader_define) {
            scalar_defines.push_back(feature_define_prefix +
                                     upperIdentifier(declaration.name, feature_name) + "=" +
                                     scalarDefineValue(value, declaration, feature_name));
        }
        if (declaration.binding == ScalarParameterBinding::buffer) {
            buffer_scalar_values.emplace_back(
                featureScalarBufferName(feature_name, declaration.name),
                scalarFloatValue(value, feature_name, declaration.name, "value"));
        }
    }

    std::unordered_map<std::string, BoundShaderAsset>
        shader_asset_bindings;
    shader_asset_bindings.reserve(
        declarations.shader_assets.size());
    for (const auto &declaration :
         declarations.shader_assets) {
        const auto supplied =
            instance_parameters.find(
                declaration.name);
        const auto *recorded =
            bound_value(declaration.name);
        const auto reference =
            supplied != instance_parameters.end()
                ? supplied->get<std::string>()
            : recorded != nullptr && recorded->is_string()
                ? recorded->get<std::string>()
                : declaration.default_reference;
        if (reference.empty() ||
            reference.front() == '$') {
            throwParameterError(
                feature_name, declaration.name,
                "shader asset value must be a concrete non-empty "
                "shader reference");
        }
        resolved_parameters[
            declaration.name] = reference;
        shader_asset_bindings.emplace(
            declaration.name,
            BoundShaderAsset{
                declaration.stage,
                reference});
    }

    auto feature = authored_feature;
    feature.erase("parameters");
    feature.erase("bound_parameters");
    replaceBoundShaderAssetValues(
        feature, shader_asset_bindings,
        feature_name);
    if (feature.contains("passes")) {
        for (auto &entry : feature.at("passes")) {
            auto &pass = entry.at("pass");
            if (pass.contains("input")) {
                replaceBoundTargetValue(pass.at("input"), bindings, feature_name);
            }
            if (pass.contains("output") && pass.at("output").is_object()) {
                auto &output = pass.at("output");
                if (output.contains("color")) {
                    replaceBoundTargetValue(output.at("color"), bindings, feature_name);
                }
                if (output.contains("depth")) {
                    replaceBoundTargetValue(output.at("depth"), bindings, feature_name);
                }
            }
        }
    }
    if (feature.contains("pass_overrides")) {
        for (auto &override_json : feature.at("pass_overrides")) {
            if (override_json.contains("input")) {
                replaceBoundTargetValue(override_json.at("input"), bindings, feature_name);
            }
        }
    }
    if (feature.contains("surface_resources")) {
        for (auto &resource :
             feature.at("surface_resources")) {
            if (resource.is_object() &&
                resource.contains("resource") &&
                resource.at("resource").is_string()) {
                const auto value =
                    resource.at("resource")
                        .get<std::string>();
                resource["resource"] =
                    replaceBoundTargetReference(
                        value, bindings, feature_name);
            }
        }
    }
    if (feature.contains("render_target_overrides")) {
        nlohmann::json replaced = nlohmann::json::object();
        for (auto override_it = feature.at("render_target_overrides").begin();
             override_it != feature.at("render_target_overrides").end(); ++override_it) {
            const auto target_name =
                replaceBoundTargetReference(override_it.key(), bindings, feature_name);
            if (replaced.contains(target_name)) {
                throwBindingError(feature_name, override_it.key(), target_name,
                                  "render_target_overrides binding collides");
            }
            replaced[target_name] = override_it.value();
        }
        feature["render_target_overrides"] = std::move(replaced);
    }
    if (materialized_document != nullptr) {
        // Captured before the scalar substitution: the derived document keeps
        // its scalar declarations and its `$name` placeholders, so editing a
        // bound scalar later still reaches every field it feeds.
        *materialized_document = feature;
        if (const auto parameters = authored_feature.find("parameters");
            parameters != authored_feature.end()) {
            if (const auto scalars = parameters->find("scalars");
                scalars != parameters->end() && !scalars->empty()) {
                (*materialized_document)["parameters"] = nlohmann::json{
                    {"schema", std::string{feature_parameters_schema}},
                    {"version", supported_feature_parameters_version},
                    {"scalars", *scalars},
                };
            }
        }
    }
    // A `binding: buffer` scalar is the initial value of the feature's own
    // single-float runtime_f32 buffer.  The value is written into the composed
    // buffer declaration (not into the derived document above, which keeps the
    // declaration editable), so re-composing after `set_parameters` re-registers
    // the buffer at the new value.  `render.scalar{target}` names the same
    // buffer and overwrites it every frame.
    for (const auto &[buffer_name, value] : buffer_scalar_values) {
        nlohmann::json *entry = nullptr;
        if (const auto buffers = feature.find("buffers");
            buffers != feature.end() && buffers->is_array()) {
            for (auto &candidate : *buffers) {
                if (candidate.is_object() && candidate.value("name", std::string{}) == buffer_name) {
                    entry = &candidate;
                    break;
                }
            }
        }
        const auto parameter_name = buffer_name.substr(feature_name.size() + 1);
        if (entry == nullptr) {
            throwParameterError(feature_name, parameter_name,
                                "binding 'buffer' requires the feature to declare a buffer named '" +
                                    buffer_name + "'");
        }
        if (entry->value("host_source", std::string{}) != "runtime_f32" ||
            !entry->contains("size") || !entry->at("size").is_number_integer() ||
            entry->at("size").get<std::int64_t>() != static_cast<std::int64_t>(sizeof(float))) {
            throwParameterError(feature_name, parameter_name,
                                "binding 'buffer' requires buffer '" + buffer_name +
                                    "' to declare host_source 'runtime_f32' and size 4");
        }
        if (entry->contains("initial_f32")) {
            throwParameterError(feature_name, parameter_name,
                                "buffer '" + buffer_name +
                                    "' initial_f32 is derived from the scalar, not authored");
        }
        (*entry)["initial_f32"] = nlohmann::json::array({value});
    }
    // Exact "$name" scalar placeholders preserve the parameter's JSON type.
    // This lets one typed value configure both shader defines and physical
    // declarations such as fixed extent or array-layer count.
    replaceBoundScalarValues(
        feature, scalar_bindings);
    return feature;
}

void addRenderTargets(nlohmann::json &config, const nlohmann::json &feature,
                      std::unordered_set<std::string> &target_names,
                      std::string_view provider_feature,
                      std::string_view provider_reference,
                      std::vector<RenderResourceProvenance> &provenance) {
    if (!feature.contains("render_targets")) {
        return;
    }

    const auto &feature_targets = requireArrayField(feature, "render_targets", "render feature");
    auto &targets = ensureArray(config, "render_targets");
    for (const auto &target : feature_targets) {
        if (!target.is_object()) {
            throw std::runtime_error("render feature render_targets entries must be objects");
        }
        const auto name = requireStringField(target, "name", "render feature render target");
        validateAuthoredRenderResourceName(
            name, "render target");
        if (!target_names.insert(name).second) {
            throw std::runtime_error("Render feature render target name collides: " + name);
        }
        targets.push_back(target);
        provenance.push_back(RenderResourceProvenance{
            .name = name,
            .kind = "render_target",
            .source = RenderPipelineProvenanceSource::feature,
            .provider_feature = std::string{provider_feature},
            .provider_reference = std::string{provider_reference},
        });
    }
}

void addBuffers(nlohmann::json &config, const nlohmann::json &feature,
                std::unordered_set<std::string> &buffer_names,
                std::string_view provider_feature,
                std::string_view provider_reference,
                std::vector<RenderResourceProvenance> &provenance) {
    if (!feature.contains("buffers")) {
        return;
    }

    const auto &feature_buffers = requireArrayField(feature, "buffers", "render feature");
    auto &buffers = ensureArray(config, "buffers");
    for (const auto &buffer : feature_buffers) {
        std::string name;
        if (buffer.is_string()) {
            name = buffer.get<std::string>();
        } else if (buffer.is_object()) {
            name = requireStringField(buffer, "name", "render feature buffer");
        } else {
            throw std::runtime_error("render feature buffers entries must be strings or objects");
        }
        validateAuthoredRenderResourceName(name, "buffer");
        if (!buffer_names.insert(name).second) {
            throw std::runtime_error("Render feature buffer name collides: " + name);
        }
        buffers.push_back(buffer);
        provenance.push_back(RenderResourceProvenance{
            .name = name,
            .kind = "buffer",
            .source = RenderPipelineProvenanceSource::feature,
            .provider_feature = std::string{provider_feature},
            .provider_reference = std::string{provider_reference},
        });
    }
}

void mergeUsage(nlohmann::json &target, const nlohmann::json &override_json, const std::string &name) {
    const auto override_usage = parseStringArray(override_json, "usage", "render target override: " + name);
    if (!target.contains("usage") || !target.at("usage").is_array()) {
        throw std::runtime_error("render target override requires existing usage array: " + name);
    }

    std::vector<std::string> usage;
    for (const auto &entry : target.at("usage")) {
        if (!entry.is_string()) {
            throw std::runtime_error("render target usage must be a string array: " + name);
        }
        appendUnique(usage, entry.get<std::string>());
    }
    appendUnique(usage, override_usage);
    target["usage"] = usage;
}

void mergeFormatCandidates(
    nlohmann::json &target,
    const nlohmann::json &override_json,
    const std::string &name) {
    auto candidates = std::vector<std::string>{};
    if (target.contains("format_candidates")) {
        if (!target.at("format_candidates").is_array()) {
            throw std::runtime_error(
                "render target format_candidates must be a "
                "string array: " +
                name);
        }
        for (const auto &entry :
             target.at("format_candidates")) {
            if (!entry.is_string() ||
                entry.get_ref<const std::string &>().empty()) {
                throw std::runtime_error(
                    "render target format_candidates must contain "
                    "non-empty strings: " +
                    name);
            }
            appendUnique(
                candidates,
                entry.get<std::string>());
        }
    }
    const auto additions = parseStringArray(
        override_json, "format_candidates",
        "render target override: " + name);
    for (const auto &candidate : additions) {
        if (candidate.empty()) {
            throw std::runtime_error(
                "render target override format_candidates must "
                "contain non-empty strings: " +
                name);
        }
    }
    appendUnique(candidates, additions);
    target["format_candidates"] =
        std::move(candidates);
}

void applyRenderTargetOverrides(nlohmann::json &config, const nlohmann::json &feature) {
    if (!feature.contains("render_target_overrides")) {
        return;
    }
    const auto &overrides = feature.at("render_target_overrides");
    if (!overrides.is_object()) {
        throw std::runtime_error("render feature render_target_overrides must be an object");
    }

    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        const auto name = it.key();
        const auto &override_json = it.value();
        if (!override_json.is_object()) {
            throw std::runtime_error("render target override must be an object: " + name);
        }

        auto *target = findRenderTarget(config, name);
        if (target == nullptr) {
            throw std::runtime_error("render target override references unknown render target: " + name);
        }

        for (auto field = override_json.begin(); field != override_json.end(); ++field) {
            if (field.key() != "format" && field.key() != "usage" &&
                field.key() != "format_candidates" &&
                field.key() != "width" && field.key() != "height") {
                throw std::runtime_error(
                    "render target override only supports format, "
                    "format_candidates, usage, width, and height: " +
                    name);
            }
        }
        if (override_json.contains("format")) {
            if (!override_json.at("format").is_string()) {
                throw std::runtime_error("render target override format must be a string: " + name);
            }
            (*target)["format"] = override_json.at("format");
        }
        if (override_json.contains("usage")) {
            mergeUsage(*target, override_json, name);
        }
        if (override_json.contains(
                "format_candidates")) {
            mergeFormatCandidates(
                *target, override_json, name);
        }
        if (override_json.contains("width")) {
            if (!override_json.at("width").is_number_integer()) {
                throw std::runtime_error("render target override width must be an integer: " + name);
            }
            (*target)["width"] = override_json.at("width");
        }
        if (override_json.contains("height")) {
            if (!override_json.at("height").is_number_integer()) {
                throw std::runtime_error("render target override height must be an integer: " + name);
            }
            (*target)["height"] = override_json.at("height");
        }
    }
}

void insertPassAtEnd(nlohmann::json &config, const nlohmann::json &pass) {
    auto &rendering_passes = ensureArray(config, "rendering_passes");
    if (rendering_passes.size() != 1) {
        throw std::runtime_error("render feature insert:end requires exactly one rendering pass");
    }
    auto &passes = rendering_passes.at(0).at("passes");
    passes.push_back(pass);
}

void insertPassAtBeginning(
    nlohmann::json &config, const nlohmann::json &pass) {
    auto &rendering_passes =
        ensureArray(config, "rendering_passes");
    if (rendering_passes.size() != 1) {
        throw std::runtime_error(
            "render feature insert:begin requires exactly one "
            "rendering pass");
    }
    auto &passes =
        rendering_passes.at(0).at("passes");
    passes.insert(passes.begin(), pass);
}

void appendStringListValue(nlohmann::json &json, const std::string &field_name,
                           const std::string &value) {
    if (!json.contains(field_name)) {
        json[field_name] = nlohmann::json::array();
    } else if (json.at(field_name).is_string()) {
        json[field_name] = nlohmann::json::array({json.at(field_name).get<std::string>()});
    }
    if (!json.at(field_name).is_array()) {
        throw std::runtime_error("render feature pass explicit edge must be a string or array: " + field_name);
    }

    auto &values = json.at(field_name);
    const auto exists = std::find(values.begin(), values.end(), value);
    if (exists == values.end()) {
        values.push_back(value);
    }
}

} // namespace

std::vector<RenderPassAnchorMatch> findRenderPassAnchorMatches(nlohmann::json &config,
                                                               const std::string &anchor_name) {
    std::vector<RenderPassAnchorMatch> matches;
    auto &rendering_passes = ensureArray(config, "rendering_passes");
    for (auto &pass_set : rendering_passes) {
        auto &passes = pass_set.at("passes");
        for (size_t i = 0; i < passes.size(); ++i) {
            if (passes.at(i).is_object() &&
                passes.at(i).value("type", std::string{}) == "canonical_anchor" &&
                passes.at(i).value("anchor", std::string{}) == anchor_name) {
                matches.push_back({&passes, i, pass_set.value("name", std::string{})});
            }
        }
    }
    if (!matches.empty()) {
        return matches;
    }
    // Non-canonical names remain valid only for migration/negative fixtures.
    for (auto &pass_set : rendering_passes) {
        auto &passes = pass_set.at("passes");
        for (size_t i = 0; i < passes.size(); ++i) {
            if (passes.at(i).is_object() && passes.at(i).value("name", std::string{}) == anchor_name) {
                matches.push_back({&passes, i, pass_set.value("name", std::string{})});
            }
        }
    }
    return matches;
}

void insertRenderPassRunAtAnchor(nlohmann::json &passes, std::size_t anchor_index, bool after,
                                 std::vector<nlohmann::json> run) {
    if (run.empty()) {
        return;
    }
    auto index = anchor_index + (after ? 1 : 0);
    const bool canonical_anchor = passes.at(anchor_index).value("type", std::string{}) ==
                                  "canonical_anchor";
    if (after && canonical_anchor) {
        while (index < passes.size() &&
               passes.at(index).value("type", std::string{}) != "canonical_anchor" &&
               passes.at(index).value("type", std::string{}) != "output_transform") {
            ++index;
        }
    }
    const auto dependency_name = passes.at(anchor_index).value("name", std::string{});
    if (after) {
        appendStringListValue(run.front(), "after", dependency_name);
    } else {
        appendStringListValue(run.back(), "before", dependency_name);
    }
    // The rest of the run needs no edge of its own: the canonical order that
    // composition settles next makes each pass wait on the one declared before it.
    for (std::size_t i = 0; i < run.size(); ++i) {
        passes.insert(passes.begin() + static_cast<nlohmann::json::difference_type>(index + i),
                      std::move(run.at(i)));
    }
}

namespace {

void insertPassByAnchor(nlohmann::json &config, const std::string &insert, const nlohmann::json &pass) {
    constexpr std::string_view before_prefix = "before:";
    constexpr std::string_view after_prefix = "after:";

    bool after = false;
    std::string anchor;
    if (insert.rfind(before_prefix, 0) == 0) {
        anchor = insert.substr(before_prefix.size());
    } else if (insert.rfind(after_prefix, 0) == 0) {
        after = true;
        anchor = insert.substr(after_prefix.size());
    } else {
        throw std::runtime_error(
            "render feature pass insert must be begin, "
            "before:<pass>, after:<pass>, or end");
    }
    if (anchor.empty()) {
        throw std::runtime_error("render feature pass insert anchor must not be empty");
    }

    auto matches = findRenderPassAnchorMatches(config, anchor);
    if (matches.empty()) {
        throw std::runtime_error("render feature pass insert anchor was not found: " + anchor);
    }
    if (matches.size() > 1) {
        throw std::runtime_error("render feature pass insert anchor is ambiguous: " + anchor);
    }
    insertRenderPassRunAtAnchor(*matches.front().passes, matches.front().index, after, {pass});
}

void addFeaturePasses(nlohmann::json &config, const nlohmann::json &feature,
                      std::unordered_set<std::string> &pass_names,
                      std::string_view provider_feature,
                      std::string_view provider_reference,
                      std::vector<RenderPassProvenance> &provenance) {
    if (!feature.contains("passes")) {
        return;
    }
    const auto &passes = requireArrayField(feature, "passes", "render feature");
    for (const auto &entry : passes) {
        if (!entry.is_object()) {
            throw std::runtime_error("render feature passes entries must be objects");
        }
        const auto insert = requireStringField(entry, "insert", "render feature pass entry");
        const auto pass_it = entry.find("pass");
        if (pass_it == entry.end() || !pass_it->is_object()) {
            throw std::runtime_error("render feature pass entry requires pass object");
        }
        const auto pass_name = requireStringField(*pass_it, "name", "render feature pass");
        if (!pass_names.insert(pass_name).second) {
            throw std::runtime_error("Render feature pass name collides: " + pass_name);
        }

        if (insert == "begin") {
            insertPassAtBeginning(config, *pass_it);
        } else if (insert == "end") {
            insertPassAtEnd(config, *pass_it);
        } else {
            insertPassByAnchor(config, insert, *pass_it);
        }
        provenance.push_back(RenderPassProvenance{
            .name = pass_name,
            .source = RenderPipelineProvenanceSource::feature,
            .provider_feature = std::string{provider_feature},
            .provider_reference = std::string{provider_reference},
        });
    }
}

nlohmann::json *findPass(nlohmann::json &config, const std::string &pass_name) {
    auto &rendering_passes = ensureArray(config, "rendering_passes");
    nlohmann::json *found_pass = nullptr;
    for (auto &pass_set : rendering_passes) {
        auto &passes = pass_set.at("passes");
        for (auto &pass : passes) {
            if (!pass.is_object() || pass.value("name", std::string{}) != pass_name) {
                continue;
            }
            if (found_pass != nullptr) {
                throw std::runtime_error("render feature pass override is ambiguous: " + pass_name);
            }
            found_pass = &pass;
        }
    }
    return found_pass;
}

bool passWritesResource(
    const nlohmann::json &pass,
    std::string_view resource) {
    if (!pass.contains("output") ||
        !pass.at("output").is_object()) {
        return false;
    }
    const auto &output = pass.at("output");
    return (output.contains("color") &&
            stringListContains(
                output.at("color"), resource)) ||
           (output.contains("depth") &&
            output.at("depth").is_string() &&
            output.at("depth").get<std::string>() ==
                resource);
}

std::size_t stringListSize(
    const nlohmann::json &object,
    std::string_view field_name,
    std::string_view context) {
    const auto field = std::string{field_name};
    if (!object.contains(field)) return 0;
    const auto &value = object.at(field);
    if (value.is_string()) return 1;
    if (!value.is_array() ||
        !std::all_of(
            value.begin(), value.end(),
            [](const auto &entry) {
                return entry.is_string();
            })) {
        throw std::runtime_error(
            std::string{context} + " " + field +
            " must be a string or string array");
    }
    return value.size();
}

void appendFullscreenSurfaceResource(
    nlohmann::json &pass, const std::string &resource,
    const MaterialPassInputContract &contract) {
    const auto input_count =
        stringListSize(pass, "input",
                       "fullscreen surface-resource consumer");
    const auto already_bound =
        pass.contains("input") &&
        stringListContains(pass.at("input"), resource);
    if (already_bound) {
        throw std::runtime_error(
            "fullscreen surface-resource consumer already binds "
            "resource '" +
            resource + "': " +
            pass.value("name", std::string{"<unnamed>"}));
    }

    if (!pass.contains("input_sampling")) {
        pass["input_sampling"] =
            nlohmann::json::array();
        for (std::size_t index = 0;
             index < input_count; ++index) {
            pass["input_sampling"].push_back({
                {"filter", "linear"},
                {"address", "repeat"},
            });
        }
    } else if (!pass.at("input_sampling").is_array() ||
               pass.at("input_sampling").size() !=
                   input_count) {
        throw std::runtime_error(
            "fullscreen surface-resource consumer has "
            "inconsistent input_sampling: " +
            pass.value("name", std::string{"<unnamed>"}));
    }

    appendStringListValue(pass, "input", resource);
    switch (contract.sampling) {
    case MaterialPassInputSampling::linear_repeat:
        pass["input_sampling"].push_back({
            {"filter", "linear"},
            {"address", "repeat"},
        });
        break;
    case MaterialPassInputSampling::nearest_clamp_to_edge:
        pass["input_sampling"].push_back({
            {"filter", "nearest"},
            {"address", "clamp_to_edge"},
        });
        break;
    }
}

void requireOnlySurfaceResourceFields(
    const nlohmann::json &object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context) {
    for (auto field = object.begin();
         field != object.end(); ++field) {
        const auto known =
            std::find(
                allowed.begin(), allowed.end(),
                field.key()) != allowed.end();
        if (!known) {
            throw std::runtime_error(
                std::string{context} +
                " has unknown field: " + field.key());
        }
    }
}

void validateFullscreenConsumerSelector(
    const nlohmann::json &selector,
    std::string_view context) {
    if (!selector.is_object()) {
        throw std::runtime_error(
            std::string{context} + " must be an object");
    }
    requireOnlySurfaceResourceFields(
        selector, {"fragment", "uses_light_data"},
        context);
    if (requireStringField(
            selector, "fragment", context)
            .empty()) {
        throw std::runtime_error(
            std::string{context} +
            " fragment must not be empty");
    }
    if (!selector.contains("uses_light_data") ||
        !selector.at("uses_light_data").is_boolean()) {
        throw std::runtime_error(
            std::string{context} +
            " requires boolean field: uses_light_data");
    }
}

// RS-10: a fullscreen pass that lists the resource among its inputs reads
// it by its own declaration, whatever its fragment shader: it is a consumer
// without a selector naming its shader, and the input is not added again.
bool fullscreenPassReadsResource(const nlohmann::json &pass,
                                 const std::string &resource) {
    return pass.value("type", std::string{}) == "fullscreen" &&
           pass.contains("input") &&
           stringListContains(pass.at("input"), resource);
}

// RS-10: a fullscreen pass with the light-data contract a selector asks for
// (uses_light_data) whose fragment is not the one the selector names: the
// author's own lighting (Studio's Deferred lighting template). The engine
// cannot add an input its shader may not declare, so it is not bound; but
// it is where the pipeline lights the scene, and it reads the resource by
// name when it lists it (fullscreenPassReadsResource).
bool fullscreenLightingWithoutSelector(const nlohmann::json &pass,
                                       const nlohmann::json &selectors) {
    if (pass.value("type", std::string{}) != "fullscreen") return false;
    return std::any_of(
        selectors.begin(), selectors.end(),
        [&](const nlohmann::json &selector) {
            return selector.at("uses_light_data").get<bool>() &&
                   pass.value("uses_light_data", false);
        });
}

bool fullscreenConsumerMatches(
    const nlohmann::json &pass,
    const nlohmann::json &selector) {
    if (pass.value("type", std::string{}) !=
        "fullscreen") {
        return false;
    }
    if (pass.value("uses_light_data", false) !=
        selector.at("uses_light_data").get<bool>()) {
        return false;
    }
    return pass.contains("shader") &&
           pass.at("shader").is_object() &&
           pass.at("shader").value(
               "fragment", std::string{}) ==
               selector.at("fragment").get_ref<
                   const std::string &>();
}

void applySurfaceResourceContracts(
    nlohmann::json &config,
    const nlohmann::json &feature,
    const std::string &feature_name,
    const std::string &feature_ref,
    nlohmann::json &compiled_declarations) {
    if (!feature.contains("surface_resources")) {
        return;
    }
    const auto &declarations =
        requireArrayField(
            feature, "surface_resources",
            "render feature");
    const auto types =
        makeBuiltinLogicalTypeRegistry();
    for (std::size_t declaration_index = 0;
         declaration_index < declarations.size();
         ++declaration_index) {
        const auto &declaration =
            declarations.at(declaration_index);
        const auto context =
            "render feature '" + feature_name +
            "' surface_resources[" +
            std::to_string(declaration_index) + "]";
        if (!declaration.is_object()) {
            throw std::runtime_error(
                context + " must be an object");
        }
        requireOnlySurfaceResourceFields(
            declaration,
            {"contract", "resource", "producer",
             "material_contracts",
             "fullscreen_consumers"},
            context);
        const auto contract_name =
            requireStringField(
                declaration, "contract", context);
        const auto resource =
            requireStringField(
                declaration, "resource", context);
        const auto producer =
            requireStringField(
                declaration, "producer", context);
        const auto contract =
            makeBuiltinMaterialPassInputContract(
                types, contract_name);
        if (!isFeatureOwnedMaterialPassInputContract(contract.name)) {
            throw std::runtime_error(
                context +
                " does not name a public feature-owned "
                "material input contract");
        }
        if (findRenderTarget(config, resource) == nullptr) {
            throw std::runtime_error(
                context + " resource target was not found: " +
                resource);
        }
        auto *producer_pass =
            findPass(config, producer);
        if (producer_pass == nullptr ||
            !passWritesResource(
                *producer_pass, resource)) {
            throw std::runtime_error(
                context + " producer '" + producer +
                "' does not write resource '" + resource +
                "'");
        }

        std::vector<std::string> material_contracts;
        if (declaration.contains("material_contracts")) {
            material_contracts = parseStringArray(
                declaration, "material_contracts",
                context);
            for (const auto &name : material_contracts) {
                const auto parsed =
                    materialPassContractFromName(name);
                if (!parsed ||
                    materialPassShaderContract(*parsed) !=
                        MaterialShaderContract::
                            forward_scene_color_v1) {
                    throw std::runtime_error(
                        context +
                        " material_contracts must name forward "
                        "material contracts: " +
                        name);
                }
            }
        }

        nlohmann::json fullscreen_selectors =
            nlohmann::json::array();
        if (declaration.contains(
                "fullscreen_consumers")) {
            fullscreen_selectors =
                declaration.at(
                    "fullscreen_consumers");
            if (!fullscreen_selectors.is_array()) {
                throw std::runtime_error(
                    context +
                    " fullscreen_consumers must be an array");
            }
            for (std::size_t selector_index = 0;
                 selector_index <
                 fullscreen_selectors.size();
                 ++selector_index) {
                validateFullscreenConsumerSelector(
                    fullscreen_selectors.at(
                        selector_index),
                    context +
                        " fullscreen_consumers[" +
                        std::to_string(selector_index) +
                        "]");
            }
        }

        nlohmann::json material_consumers =
            nlohmann::json::array();
        nlohmann::json fullscreen_consumers =
            nlohmann::json::array();
        bool unbound_lighting = false;
        for (auto &pass_set :
             ensureArray(config, "rendering_passes")) {
            auto &passes =
                pass_set.at("passes");
            for (auto &pass : passes) {
                const auto pass_name =
                    pass.value("name", std::string{});
                const auto material_contract =
                    pass.value(
                        "material_contract",
                        std::string{});
                if (pass.value("type", std::string{}) ==
                        "material" &&
                    std::find(
                        material_contracts.begin(),
                        material_contracts.end(),
                        material_contract) !=
                        material_contracts.end()) {
                    if (!pass.contains(
                            "surface_resources")) {
                        pass["surface_resources"] =
                            nlohmann::json::object();
                    }
                    if (!pass.at(
                             "surface_resources")
                             .is_object()) {
                        throw std::runtime_error(
                            context +
                            " consumer surface_resources must "
                            "be an object: " +
                            pass_name);
                    }
                    auto &resources =
                        pass.at("surface_resources");
                    if (resources.contains(contract.name)) {
                        throw std::runtime_error(
                            context +
                            " duplicates material contract '" +
                            contract.name + "' in pass '" +
                            pass_name + "'");
                    }
                    resources[contract.name] = resource;
                    material_consumers.push_back(
                        pass_name);
                }

                for (std::size_t selector_index = 0;
                     selector_index <
                     fullscreen_selectors.size();
                     ++selector_index) {
                    const auto &selector =
                        fullscreen_selectors.at(
                            selector_index);
                    if (!fullscreenConsumerMatches(
                            pass, selector)) {
                        continue;
                    }
                    if (std::find(
                            fullscreen_consumers.begin(),
                            fullscreen_consumers.end(),
                            pass_name) !=
                        fullscreen_consumers.end()) {
                        continue;
                    }
                    appendFullscreenSurfaceResource(
                        pass, resource, contract);
                    fullscreen_consumers.push_back(
                        pass_name);
                }
                if (fullscreen_selectors.empty()) continue;
                const bool listed =
                    std::find(fullscreen_consumers.begin(),
                              fullscreen_consumers.end(),
                              pass_name) !=
                    fullscreen_consumers.end();
                if (!listed &&
                    fullscreenPassReadsResource(pass, resource)) {
                    fullscreen_consumers.push_back(pass_name);
                } else if (!listed &&
                           fullscreenLightingWithoutSelector(
                               pass, fullscreen_selectors)) {
                    unbound_lighting = true;
                }
            }
        }
        // RS-10: a pipeline that lights the scene in a fullscreen pass of
        // its own shader (deferred geometry and the author's lighting) is
        // not refused: the resource is produced and declared, and that pass
        // reads it by listing it as an input.
        if (material_consumers.empty() &&
            fullscreen_consumers.empty() &&
            !unbound_lighting &&
            (declaration.contains("material_contracts") ||
             declaration.contains("fullscreen_consumers"))) {
            throw std::runtime_error(
                context +
                " did not match any material or fullscreen "
                "consumer");
        }

        compiled_declarations.push_back({
            {"contract", contract.name},
            {"resource", resource},
            {"producer", producer},
            {"provider_feature", feature_name},
            {"provider_ref", feature_ref},
            {"material_consumers",
             std::move(material_consumers)},
            {"fullscreen_consumers",
             std::move(fullscreen_consumers)},
        });
    }
}

// LP-Vb (docs/design_graph_loops.md 14.7): the graph's resolution stage adds
// passes after applySurfaceResourceContracts picked the consumers -- the
// copies of a graph.loop, which may take the place of feature passes that
// were consumers (pass_replacements; the replaced names have left the
// consumer lists there). Among `passes` only, pick the consumers of each of
// the feature's declarations by the same selectors and bind them the same
// way, adding them to the declaration composed earlier (matched by contract).
void selectSurfaceResourceConsumersAmong(
    nlohmann::json &config,
    const nlohmann::json &feature,
    nlohmann::json &compiled_declarations,
    const std::set<std::string> &passes) {
    if (!feature.contains("surface_resources") ||
        !feature.at("surface_resources").is_array()) {
        return;
    }
    const auto types = makeBuiltinLogicalTypeRegistry();
    for (const auto &declaration : feature.at("surface_resources")) {
        if (!declaration.is_object()) continue;
        const auto contract_name = declaration.value("contract", std::string{});
        const auto compiled = std::find_if(
            compiled_declarations.begin(), compiled_declarations.end(),
            [&](const nlohmann::json &entry) {
                return entry.is_object() &&
                       entry.value("contract", std::string{}) == contract_name;
            });
        if (compiled == compiled_declarations.end()) continue;
        const auto resource = compiled->value("resource", std::string{});
        const auto contract =
            makeBuiltinMaterialPassInputContract(types, contract_name);
        std::vector<std::string> material_contracts;
        if (declaration.contains("material_contracts"))
            material_contracts = parseStringArray(
                declaration, "material_contracts",
                "render feature surface_resources");
        const auto selectors =
            declaration.value("fullscreen_consumers", nlohmann::json::array());
        auto &material_consumers = (*compiled)["material_consumers"];
        auto &fullscreen_consumers = (*compiled)["fullscreen_consumers"];
        for (auto &pass_set : ensureArray(config, "rendering_passes")) {
            for (auto &pass : pass_set.at("passes")) {
                const auto pass_name = pass.value("name", std::string{});
                if (!passes.contains(pass_name)) continue;
                if (pass.value("type", std::string{}) == "material" &&
                    std::find(material_contracts.begin(),
                              material_contracts.end(),
                              pass.value("material_contract", std::string{})) !=
                        material_contracts.end()) {
                    auto &resources = pass["surface_resources"];
                    if (resources.is_null()) resources = nlohmann::json::object();
                    if (!resources.is_object() || resources.contains(contract.name)) {
                        throw std::runtime_error(
                            "render feature surface_resources consumer cannot bind contract '" +
                            contract.name + "': " + pass_name);
                    }
                    resources[contract.name] = resource;
                    material_consumers.push_back(pass_name);
                }
                if (!selectors.is_array()) continue;
                for (const auto &selector : selectors) {
                    if (!fullscreenConsumerMatches(pass, selector) ||
                        std::find(fullscreen_consumers.begin(),
                                  fullscreen_consumers.end(),
                                  pass_name) != fullscreen_consumers.end()) {
                        continue;
                    }
                    appendFullscreenSurfaceResource(pass, resource, contract);
                    fullscreen_consumers.push_back(pass_name);
                }
                // RS-10: as in applySurfaceResourceContracts.
                if (!selectors.empty() &&
                    std::find(fullscreen_consumers.begin(),
                              fullscreen_consumers.end(),
                              pass_name) == fullscreen_consumers.end() &&
                    fullscreenPassReadsResource(pass, resource))
                    fullscreen_consumers.push_back(pass_name);
            }
        }
    }
}

void applyPassOverrides(nlohmann::json &config, const nlohmann::json &feature) {
    if (!feature.contains("pass_overrides")) {
        return;
    }
    const auto &overrides = feature.at("pass_overrides");
    if (!overrides.is_object()) {
        throw std::runtime_error("render feature pass_overrides must be an object");
    }

    for (auto it = overrides.begin(); it != overrides.end(); ++it) {
        const auto pass_name = it.key();
        const auto &override_json = it.value();
        if (!override_json.is_object()) {
            throw std::runtime_error("render feature pass override must be an object: " + pass_name);
        }
        auto *pass = findPass(config, pass_name);
        if (pass == nullptr) {
            throw std::runtime_error("render feature pass override references unknown pass: " + pass_name);
        }

        for (auto field = override_json.begin();
             field != override_json.end(); ++field) {
            if (field.key() != "input" &&
                field.key() != "resource_ports" &&
                field.key() != "material_resources") {
                throw std::runtime_error(
                    "render feature pass override has unsupported field '" +
                    field.key() + "': " + pass_name);
            }
        }
        if (override_json.contains("input")) {
            for (const auto &input : parseStringArray(override_json, "input",
                                                      "render feature pass override: " + pass_name)) {
                appendStringListValue(*pass, "input", input);
            }
        }
        const auto merge_named_resources =
            [&](std::string_view field_name) {
                const auto field =
                    std::string{field_name};
                if (!override_json.contains(field)) {
                    return;
                }
                const auto &resources =
                    override_json.at(field);
                if (!resources.is_object()) {
                    throw std::runtime_error(
                        "render feature pass override " +
                        std::string{field_name} +
                        " must be an object: " +
                        pass_name);
                }
                if (!pass->contains(field)) {
                    (*pass)[field] =
                        nlohmann::json::object();
                }
                if (!pass->at(field)
                         .is_object()) {
                    throw std::runtime_error(
                        "render feature pass override cannot merge "
                        "non-object " +
                        std::string{field_name} +
                        ": " + pass_name);
                }
                for (auto resource =
                         resources.begin();
                     resource != resources.end();
                     ++resource) {
                    if (pass->at(field)
                            .contains(resource.key())) {
                        throw std::runtime_error(
                            "render feature pass override " +
                            std::string{field_name} +
                            " collides at '" +
                            resource.key() + "': " +
                            pass_name);
                    }
                    (*pass)[field]
                           [resource.key()] =
                        resource.value();
                }
            };
        merge_named_resources("resource_ports");
        merge_named_resources("material_resources");
    }
}

void resolveInheritedPassBindings(
    nlohmann::json &config) {
    const std::string field_name =
        "inherit_bindings_from";
    std::vector<std::string> pass_names;
    for (auto &pass_set :
         ensureArray(config, "rendering_passes")) {
        for (auto &pass :
             pass_set.at("passes")) {
            if (pass.is_object() &&
                pass.contains(field_name)) {
                (void)requireStringField(
                    pass, field_name,
                    "render feature pass binding inheritance");
                pass_names.push_back(
                    requireStringField(
                        pass, "name",
                        "render feature pass binding inheritance"));
            }
        }
    }

    enum class VisitState {
        visiting,
        resolved,
    };
    std::unordered_map<std::string, VisitState>
        states;
    std::function<void(const std::string &)>
        resolve =
            [&](const std::string &pass_name) {
                const auto state =
                    states.find(pass_name);
                if (state != states.end()) {
                    if (state->second ==
                        VisitState::visiting) {
                        throw std::runtime_error(
                            "render feature pass binding inheritance "
                            "contains a cycle at: " +
                            pass_name);
                    }
                    return;
                }
                auto *pass =
                    findPass(config, pass_name);
                if (pass == nullptr ||
                    !pass->contains(field_name)) {
                    states[pass_name] =
                        VisitState::resolved;
                    return;
                }
                states[pass_name] =
                    VisitState::visiting;
                const auto source_name =
                    requireStringField(
                        *pass, field_name,
                        "render feature pass binding inheritance");
                if (source_name == pass_name) {
                    throw std::runtime_error(
                        "render feature pass cannot inherit bindings "
                        "from itself: " +
                        pass_name);
                }
                auto *source =
                    findPass(config, source_name);
                if (source == nullptr) {
                    throw std::runtime_error(
                        "render feature pass binding inheritance "
                        "references unknown pass '" +
                        source_name + "': " +
                        pass_name);
                }
                if (source->contains(field_name)) {
                    resolve(source_name);
                    pass = findPass(
                        config, pass_name);
                    source = findPass(
                        config, source_name);
                }

                const auto pass_type =
                    pass->value(
                        "type", std::string{});
                const auto source_type =
                    source->value(
                        "type", std::string{});
                if (pass_type != source_type ||
                    (pass_type != "material" &&
                     pass_type != "fullscreen")) {
                    throw std::runtime_error(
                        "render feature pass binding inheritance "
                        "requires matching material or fullscreen "
                        "pass types: " +
                        pass_name + " <- " +
                        source_name);
                }

                const auto merge_missing_object =
                    [&](std::string_view object_name) {
                        const auto object =
                            std::string{object_name};
                        if (!source->contains(object)) {
                            return;
                        }
                        if (!source->at(object)
                                 .is_object()) {
                            throw std::runtime_error(
                                "render feature pass binding source has "
                                "non-object " +
                                object + ": " +
                                source_name);
                        }
                        if (!pass->contains(object)) {
                            (*pass)[object] =
                                nlohmann::json::object();
                        }
                        if (!pass->at(object)
                                 .is_object()) {
                            throw std::runtime_error(
                                "render feature pass binding destination "
                                "has non-object " +
                                object + ": " +
                                pass_name);
                        }
                        for (const auto &entry :
                             source->at(object).items()) {
                            if (!pass->at(object)
                                     .contains(entry.key())) {
                                (*pass)[object]
                                       [entry.key()] =
                                    entry.value();
                            }
                        }
                    };

                if (pass_type == "material") {
                    if (pass->value(
                            "material_contract",
                            std::string{}) !=
                        source->value(
                            "material_contract",
                            std::string{})) {
                        throw std::runtime_error(
                            "render feature material pass binding "
                            "inheritance requires the same "
                            "material_contract: " +
                            pass_name + " <- " +
                            source_name);
                    }
                    merge_missing_object(
                        "surface_resources");
                    merge_missing_object(
                        "material_resources");
                    merge_missing_object(
                        "screen_inputs");
                } else {
                    if (source->contains(
                            "resource_ports")) {
                        merge_missing_object(
                            "resource_ports");
                        for (const auto &entry :
                             source->at(
                                 "resource_ports")
                                 .items()) {
                            const auto &effective =
                                pass->at(
                                    "resource_ports")
                                    .at(entry.key());
                            if (!effective.is_object() ||
                                !effective.contains(
                                    "resource") ||
                                !effective.at(
                                    "resource")
                                     .is_string()) {
                                throw std::runtime_error(
                                    "fullscreen inherited resource port "
                                    "requires a string resource: " +
                                    pass_name + "." +
                                    entry.key());
                            }
                            appendStringListValue(
                                *pass, "input",
                                effective.at(
                                    "resource")
                                    .get<std::string>());
                        }
                    }
                }
                pass->erase(field_name);
                states[pass_name] =
                    VisitState::resolved;
            };

    for (const auto &pass_name :
         pass_names) {
        if (pass_name.empty()) {
            throw std::runtime_error(
                "render feature pass binding inheritance "
                "requires a named pass");
        }
        resolve(pass_name);
    }
}

void addFeatureComputeTasks(nlohmann::json &config, const nlohmann::json &feature,
                            std::unordered_set<std::string> &task_names,
                            std::string_view provider_feature,
                            std::string_view provider_reference,
                            std::vector<RenderPassProvenance> &provenance) {
    if (!feature.contains("compute_tasks")) {
        return;
    }
    const auto &tasks = requireArrayField(feature, "compute_tasks", "render feature");
    auto &compute_tasks = ensureArray(config, "compute_tasks");
    for (const auto &task : tasks) {
        if (!task.is_object()) {
            throw std::runtime_error("render feature compute_tasks entries must be objects");
        }
        const auto task_name = requireStringField(task, "name", "render feature compute task");
        if (!task_names.insert(task_name).second) {
            throw std::runtime_error("Render feature compute task name collides: " + task_name);
        }
        compute_tasks.push_back(task);
        provenance.push_back(RenderPassProvenance{
            .name = task_name,
            .source = RenderPipelineProvenanceSource::feature,
            .provider_feature = std::string{provider_feature},
            .provider_reference = std::string{provider_reference},
        });
    }
}

void applyLightingDataPlan(
    nlohmann::json &config,
    const nlohmann::json &feature,
    const std::string &feature_name,
    const std::string &feature_ref) {
    if (!feature.contains("lighting_data")) {
        return;
    }
    if (config.contains("lighting_data")) {
        throw std::runtime_error(
            "render features '" +
            config.at("lighting_data")
                .value("provider_feature", std::string{"<authored>"}) +
            "' and '" + feature_name +
            "' cannot both provide lighting_data");
    }
    const auto &authored =
        feature.at("lighting_data");
    if (!authored.is_object()) {
        throw std::runtime_error(
            "render feature '" + feature_name +
            "' lighting_data must be an object");
    }
    if (authored.contains("provider_feature") ||
        authored.contains("provider_reference")) {
        throw std::runtime_error(
            "render feature '" + feature_name +
            "' lighting_data provider identity is assigned by "
            "feature composition");
    }
    config["lighting_data"] = authored;
    config["lighting_data"]["provider_feature"] =
        feature_name;
    config["lighting_data"]["provider_reference"] =
        feature_ref;
}

void appendShaderDefines(std::vector<std::string> &defines, const nlohmann::json &json,
                         std::string_view context) {
    if (!json.contains("shader_defines")) {
        return;
    }
    appendUnique(defines, parseStringArray(json, "shader_defines", context));
}

bool parseFeatureRequiresRuntimeShaderCompiler(
    const nlohmann::json &feature,
    std::string_view feature_name) {
    if (!feature.contains("runtime_shader_compiler")) {
        return true;
    }
    if (!feature.at("runtime_shader_compiler").is_string()) {
        throw std::runtime_error(
            "render feature '" + std::string{feature_name} +
            "' runtime_shader_compiler must be 'required' or 'optional'");
    }
    const auto value =
        feature.at("runtime_shader_compiler").get<std::string>();
    if (value == "required") return true;
    if (value == "optional") return false;
    throw std::runtime_error(
        "render feature '" + std::string{feature_name} +
        "' runtime_shader_compiler must be 'required' or 'optional'");
}

void appendRequiredCapabilities(
    nlohmann::json &config,
    const nlohmann::json &feature,
    std::string_view feature_name) {
    if (!feature.contains("required_capabilities")) return;
    const auto capabilities = renderFeatureRequiredCapabilities(
        feature, feature_name);
    auto &planning = config["target_planning"];
    if (planning.is_null()) planning = nlohmann::json::object();
    if (!planning.is_object()) {
        throw std::runtime_error(
            "rendering config target_planning must be an object");
    }
    auto &graphs = planning["graphs"];
    if (graphs.is_null()) graphs = nlohmann::json::object();
    if (!graphs.is_object()) {
        throw std::runtime_error(
            "rendering config target_planning.graphs must be an object");
    }
    for (const auto &pass_set :
         ensureArray(config, "rendering_passes")) {
        const auto graph_name = requireStringField(
            pass_set, "name", "rendering pass set");
        auto &graph = graphs[graph_name];
        if (graph.is_null()) graph = nlohmann::json::object();
        if (!graph.is_object()) {
            throw std::runtime_error(
                "rendering config target_planning graph must be an object: " +
                graph_name);
        }
        auto &required = graph["required_capabilities"];
        if (required.is_null()) required = nlohmann::json::array();
        if (!required.is_array()) {
            throw std::runtime_error(
                "rendering config required_capabilities must be an array: " +
                graph_name);
        }
        for (const auto &capability : capabilities) {
            if (std::none_of(
                    required.begin(), required.end(),
                    [&](const auto &entry) {
                        return entry.is_string() &&
                               entry.get<std::string>() == capability;
                    })) {
                required.push_back(capability);
            }
        }
    }
}

// Scalar defines are NOT baked here.  The derived document keeps the scalar
// declarations, so compose regenerates the defines from the values in force at
// that moment; baking them as well would leave a stale
// `PELICAN_FEATURE_<F>_<N>=<old>` next to the regenerated one.
nlohmann::json boundFeatureDocument(nlohmann::json feature,
    const nlohmann::json &parameters) {
    std::vector<std::string> defines;
    appendShaderDefines(defines, feature, "render feature");
    if (!defines.empty()) feature["shader_defines"] = std::move(defines);
    feature["bound_parameters"] = parameters;
    return feature;
}

} // namespace

nlohmann::json materializeRenderFeatureDocument(
    const nlohmann::json &authored_feature,
    const nlohmann::json &instance_parameters,
    const nlohmann::json &config) {
    const auto name = validateFeatureEnvelope(authored_feature, "materialize");
    if (!instance_parameters.is_object())
        throw std::invalid_argument("Render feature instance parameters must be an object");
    if (authored_feature.contains("bound_parameters"))
        throw std::runtime_error("Render feature '" + name +
            "' is already materialized and cannot be materialized again");
    nlohmann::json parameters;
    std::vector<std::string> scalar_defines;
    nlohmann::json materialized;
    bindFeatureParameters(authored_feature, instance_parameters,
        config, name, parameters, scalar_defines, &materialized);
    return boundFeatureDocument(std::move(materialized), parameters);
}

bool renderFeatureRequiresRuntimeShaderCompiler(
    const nlohmann::json &feature,
    std::string_view feature_name) {
    return parseFeatureRequiresRuntimeShaderCompiler(
        feature, feature_name);
}

std::vector<std::string> renderFeatureRequiredCapabilities(
    const nlohmann::json &feature,
    std::string_view feature_name) {
    if (!feature.contains("required_capabilities")) return {};
    auto capabilities = parseStringArray(
        feature, "required_capabilities",
        "render feature '" + std::string{feature_name} + "'");
    if (capabilities.empty()) {
        throw std::runtime_error(
            "render feature required_capabilities must not be empty: " +
            std::string{feature_name});
    }
    return capabilities;
}

namespace {
// LP-1e: the pass family a graph's render node or loop body pass joins when it
// names no `pass_set`: the composed config's first family. A preset's
// families come first there (the rendering section's own and the graph's are
// merged after them), then the rendering section's; `main` only when there
// is none. A preset that does not resolve is refused by the composition
// itself, with its own message; here it only says nothing.
std::string defaultGraphPassFamily(const nlohmann::json &document, const RenderPipelinePresetLoader &load_preset) {
    const auto first = [](const nlohmann::json &config) -> std::optional<std::string> {
        const auto families = config.find("rendering_passes");
        if (families == config.end() || !families->is_array() || families->empty() ||
            !families->front().is_object())
            return std::nullopt;
        const auto name = families->front().find("name");
        if (name == families->front().end() || !name->is_string()) return std::nullopt;
        return name->get<std::string>();
    };
    const auto rendering = document.value("rendering", nlohmann::json::object());
    if (!rendering.is_object()) return "main";
    if (rendering.contains("pipeline")) {
        auto authored = rendering;
        authored.erase("rendering_passes");
        authored.erase("render_targets");
        try {
            if (const auto family = first(resolveRenderPipelinePreset(authored, load_preset).config)) return *family;
        } catch (const std::exception &) {
        }
    }
    if (const auto family = first(rendering)) return *family;
    return "main";
}
} // namespace

RenderFeatureComposeResult composeRenderFeatureConfig(
    const nlohmann::json &config,
    const RenderFeatureComposeDependencies &dependencies) {
    if (!config.is_object()) {
        throw std::runtime_error("Rendering config must be an object");
    }

    const bool common_graph = config.value("schema", std::string{}) == "pelican.graph";
    // LP-0a: the graph is lowered before the preset and the features are
    // composed, and resolved after them (resolveGraphDocumentAfterComposition).
    const RenderPipelinePresetLoader load_preset =
        dependencies.load_pipeline_json ? dependencies.load_pipeline_json : dependencies.load_feature_json;
    auto lowered_graph = common_graph
        ? lowerRenderGraphDocumentForComposition(config, dependencies.inactive_graph_scopes,
                                                 builtinGraphModuleRegistry(),
                                                 defaultGraphPassFamily(config, load_preset))
        : LoweredRenderGraphDocument{config, {}, {}, {}, {}, {}, 0};
    auto native_config = std::move(lowered_graph.config);
    auto graph_buffers = nlohmann::json::array();
    auto graph_tasks = nlohmann::json::array();
    auto graph_targets = nlohmann::json::array();
    auto graph_passes = nlohmann::json::array();
    if (common_graph) {
        graph_buffers = std::move(native_config.at("buffers"));
        graph_tasks = std::move(native_config.at("compute_tasks"));
        native_config.erase("buffers");
        native_config.erase("compute_tasks");
        if (native_config.contains("render_targets")) {
            graph_targets = std::move(native_config.at("render_targets"));
            native_config.erase("render_targets");
        }
        if (native_config.contains("rendering_passes")) {
            graph_passes = std::move(native_config.at("rendering_passes"));
            native_config.erase("rendering_passes");
        }
    }
    const auto preset_resolution = resolveRenderPipelinePreset(native_config, load_preset);
    auto resolved_config = preset_resolution.config;
    // Preset roots accept settings and feature references, not concrete resource
    // declarations. Add the common graph's declarations after preset expansion.
    for (auto &buffer : graph_buffers) resolved_config["buffers"].push_back(std::move(buffer));
    for (auto &task : graph_tasks) resolved_config["compute_tasks"].push_back(std::move(task));
    // RS-6: the graph's render.image declarations (a loop's copies join later).
    std::set<std::string> graph_images;
    for (const auto &target : graph_targets)
        if (target.is_object() && target.contains("name") && target.at("name").is_string())
            graph_images.insert(target.at("name").get<std::string>());
    for (auto &target : graph_targets) resolved_config["render_targets"].push_back(std::move(target));
    for (auto &family : graph_passes) {
        auto &families = ensureArray(resolved_config, "rendering_passes");
        auto found = std::find_if(families.begin(), families.end(), [&](const auto &candidate) {
            return candidate.at("name") == family.at("name");
        });
        if (!family.contains("passes") || !family.at("passes").is_array()) {
            if (found == families.end()) families.push_back(std::move(family));
            else for (auto &pass : family.at("passes")) (*found)["passes"].push_back(std::move(pass));
            continue;
        }
        // The lowered family holds the rendering section's own passes and,
        // after them, the graph's appended ones.
        auto authored = std::move(family.at("passes"));
        if (found == families.end()) {
            family["passes"] = nlohmann::json::array();
            families.push_back(std::move(family));
            found = std::prev(families.end());
        }
        auto &passes = (*found)["passes"];
        if (passes.is_null()) passes = nlohmann::json::array();
        std::vector<nlohmann::json> graph_run;
        for (auto &pass : authored) {
            if (pass.is_object() && pass.contains("name") && pass.at("name").is_string() &&
                lowered_graph.graph_declarations.contains(pass.at("name").get<std::string>()))
                graph_run.push_back(std::move(pass));
            else passes.push_back(std::move(pass));
        }
        // LP-1e: the graph's passes follow the family's scene passes. RS-3:
        // unless one of those reads what the graph's passes write (the loops
        // that stand by them included) and nothing before it writes that:
        // then the graph's passes, in their own order, stand right before the
        // first such reader. A pass the canonical order puts past the first
        // anchor already stands after them and is not a reader here. RS-6:
        // what a graph compute task that waits on them writes counts as
        // theirs (the reader waits on the task, waitOnGraphTasksInScenePasses).
        const auto family_name = found->at("name").get<std::string>();
        std::vector<const nlohmann::json *> run;
        std::set<std::string> run_names;
        for (const auto &pass : graph_run) {
            run.push_back(&pass);
            run_names.insert(pass.value("name", std::string{}));
        }
        for (const auto &loop : lowered_graph.loops) {
            if (loop.shape.placement || !loop.positions.contains(family_name)) continue;
            run_names.insert(loop.shape.node);
            for (const auto &body : loop.branches)
                if (body)
                    for (const auto &[pass_family, pass] : body->passes)
                        if (pass_family == family_name) run.push_back(&pass);
        }
        const auto scene_pass = [](const nlohmann::json &pass) {
            const auto type = pass.value("type", std::string{});
            return type != "ui" && type != "debug_draw" && type != "gizmo" && type != "debug_text" &&
                   !(pass.contains("canonical_anchor") && pass.at("canonical_anchor").is_string()) &&
                   !passWritesSwapchain(pass);
        };
        auto position = graphPassRunReaderPosition(resolved_config, passes, run, run_names, scene_pass,
                                                   passes.size()).value_or(passes.size());
        for (auto &pass : graph_run)
            passes.insert(passes.begin() + static_cast<nlohmann::json::difference_type>(position++), std::move(pass));
    }
    // LP-3b: a graph pass placed with `declared: true` stands among the
    // declared passes before any feature is composed, as the pass it was
    // decomposed from did.
    if (common_graph) placeDeclaredRenderGraphPassRuns(resolved_config, lowered_graph);
    std::vector<RenderPassProvenance> pass_provenance;
    std::vector<RenderResourceProvenance> resource_provenance;
    const auto authored_source =
        preset_resolution.preset &&
                preset_resolution.preset->reference.rfind(
                    "engine://", 0) == 0
            ? RenderPipelineProvenanceSource::engine
            : RenderPipelineProvenanceSource::project;
    synchronizeRenderPipelineProvenance(
        resolved_config, pass_provenance,
        resource_provenance, authored_source);
    if (dependencies.transform_resolved_config) {
        resolved_config =
            dependencies.transform_resolved_config(
                resolved_config);
        if (!resolved_config.is_object()) {
            throw std::runtime_error(
                "resolved rendering config transform must "
                "return an object");
        }
    }

    const auto feature_instances = parseFeatureInstances(resolved_config);
    std::vector<std::string> shader_defines;
    appendShaderDefines(shader_defines, resolved_config, "rendering config");
    struct LoadedFeature {
        FeatureInstance instance;
        std::string name;
        nlohmann::json feature;
    };
    std::vector<LoadedFeature> loaded_features;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<nlohmann::json> projection_jitter;
    nlohmann::json surface_resource_contracts =
        nlohmann::json::array();
    // LP-0d: what the graph's resolution stage needs to know of the features.
    RenderGraphFeatureContext graph_features;
    bool hdr_enabled = false;
    loaded_features.reserve(feature_instances.size());
    for (const auto &instance : feature_instances) {
        auto feature = loadFeatureJson(instance.ref, dependencies);
        const auto feature_name = validateFeatureEnvelope(feature, instance.ref);
        if (dependencies.include_feature &&
            !dependencies.include_feature(feature_name, feature)) {
            appendUnique(excluded_feature_names, feature_name);
            // LP-0d: a graph node that depends on what this feature declares
            // is made inactive with it.
            const auto keep = [&](std::map<std::string, std::string> &names, const nlohmann::json &list,
                                  bool feature_pass) {
                if (!list.is_array()) return;
                for (const auto &entry : list) {
                    const auto *declaration = &entry;
                    if (feature_pass && entry.is_object() && entry.contains("pass")) declaration = &entry.at("pass");
                    if (declaration->is_object() && declaration->contains("name") &&
                        declaration->at("name").is_string())
                        names.try_emplace(declaration->at("name").get<std::string>(), feature_name);
                }
            };
            keep(graph_features.excluded_passes, feature.value("passes", nlohmann::json::array()), true);
            keep(graph_features.excluded_passes, feature.value("compute_tasks", nlohmann::json::array()), false);
            keep(graph_features.excluded_resources, feature.value("render_targets", nlohmann::json::array()), false);
            keep(graph_features.excluded_resources, feature.value("buffers", nlohmann::json::array()), false);
            for (const auto &[pass, node] : instance.pass_replacements)
                graph_features.excluded_replacements.push_back({feature_name, pass, node});
            continue;
        }
        for (const auto &[pass, node] : instance.pass_replacements)
            graph_features.replacements.push_back({feature_name, pass, node});
        if (dependencies.validate_feature) {
            dependencies.validate_feature(feature_name, feature);
        }
        appendUnique(feature_names, feature_name);
        hdr_enabled = hdr_enabled || feature_name == "hdr";
        if (auto declaration = FeatureComposeInternal::parseProjectionJitterDeclaration(
                feature, feature_name)) {
            if (projection_jitter) {
                throw std::runtime_error(
                    "projection_jitter providers '" + projection_jitter->at("provider").get<std::string>() +
                    "' and '" + feature_name + "' cannot both be enabled");
            }
            projection_jitter = std::move(declaration);
        }
        loaded_features.push_back(LoadedFeature{instance, feature_name, std::move(feature)});
    }
    if (!dependencies.runtime_shader_compiler_enabled &&
        std::any_of(
            loaded_features.begin(), loaded_features.end(),
            [](const auto &loaded) {
                return renderFeatureRequiresRuntimeShaderCompiler(
                    loaded.feature, loaded.name);
            })) {
        throw std::runtime_error(
            std::string{
                renderFeatureRuntimeCompilerRequiredMessage});
    }

    auto composed = resolved_config;
    composed.erase("features");
    initializeCanonicalColorPipeline(composed, hdr_enabled);

    auto target_names = collectRenderTargetNames(composed);
    auto pass_names = collectPassNames(composed);
    // LP-0b: a graph pass placed at an anchor joins the passes only in the
    // resolution stage, but its name is taken from the start, as an appended
    // graph pass's is: a feature pass of the same name collides, and an
    // integration's requires_passes sees it.
    for (const auto &run : lowered_graph.placed_runs) {
        for (const auto &pass : run.passes) {
            const auto name = requireStringField(pass, "name", "pass");
            if (!pass_names.insert(name).second) {
                throw std::runtime_error("Pass name collides with canonical frame node: " + name);
            }
        }
    }
    auto buffer_names = collectBufferNames(composed);
    auto task_names = collectComputeTaskNames(composed);
    nlohmann::json resolved_instances = nlohmann::json::array();
    struct PendingSurfaceResourceFeature {
        nlohmann::json feature;
        std::string name;
        std::string reference;
    };
    std::vector<PendingSurfaceResourceFeature>
        pending_surface_resources;
    pending_surface_resources.reserve(
        loaded_features.size());
    for (const auto &loaded : loaded_features) {
        nlohmann::json resolved_parameters;
        std::vector<std::string> scalar_defines;
        nlohmann::json materialized_document;
        const auto feature = bindFeatureParameters(loaded.feature, loaded.instance.parameters,
                                                   composed, loaded.name, resolved_parameters,
                                                   scalar_defines,
                                                   dependencies.materialized_feature
                                                       ? &materialized_document
                                                       : nullptr);
        if (dependencies.materialized_feature)
            dependencies.materialized_feature(loaded.instance.ref,
                boundFeatureDocument(std::move(materialized_document), resolved_parameters));
        addRenderTargets(
            composed, feature, target_names,
            loaded.name, loaded.instance.ref,
            resource_provenance);
        addBuffers(
            composed, feature, buffer_names,
            loaded.name, loaded.instance.ref,
            resource_provenance);
        applyRenderTargetOverrides(composed, feature);
        addFeaturePasses(
            composed, feature, pass_names,
            loaded.name, loaded.instance.ref,
            pass_provenance);
        applyPassOverrides(composed, feature);
        addFeatureComputeTasks(
            composed, feature, task_names,
            loaded.name, loaded.instance.ref,
            pass_provenance);
        appendRequiredCapabilities(
            composed, feature, loaded.name);
        applyLightingDataPlan(
            composed, feature, loaded.name,
            loaded.instance.ref);
        appendShaderDefines(shader_defines, feature, "render feature");
        appendUnique(shader_defines, scalar_defines);
        resolved_instances.push_back({
            {"feature", loaded.name},
            {"parameters", std::move(resolved_parameters)},
            {"ref", loaded.instance.ref},
        });
        pending_surface_resources.push_back({
            feature,
            loaded.name,
            loaded.instance.ref,
        });
    }
    // Cross-feature additions are evaluated only after every base fragment
    // has been composed.  This keeps an integration independent of feature
    // declaration order while retaining the same collision and validation
    // rules as ordinary feature content.
    std::unordered_set<std::string>
        enabled_feature_names{
            feature_names.begin(),
            feature_names.end()};
    const auto base_pass_names =
        pass_names;
    std::vector<PendingSurfaceResourceFeature>
        pending_integration_surface_resources;
    for (const auto &provider :
         pending_surface_resources) {
        if (!provider.feature.contains(
                "integrations")) {
            continue;
        }
        const auto &integrations =
            requireArrayField(
                provider.feature,
                "integrations",
                "render feature '" +
                    provider.name + "'");
        for (const auto &integration :
             integrations) {
            if (!integration.is_object()) {
                throw std::runtime_error(
                    "render feature integration must be an object: " +
                    provider.name);
            }
            for (auto field =
                     integration.begin();
                 field != integration.end();
                 ++field) {
                if (field.key() != "name" &&
                    field.key() != "requires" &&
                    field.key() !=
                        "requires_passes" &&
                    field.key() != "fragment") {
                    throw std::runtime_error(
                        "render feature integration has unknown field '" +
                        field.key() + "': " +
                        provider.name);
                }
            }
            const auto integration_name =
                requireStringField(
                    integration, "name",
                    "render feature integration in '" +
                        provider.name + "'");
            const auto required_features =
                parseStringArray(
                    integration, "requires",
                    "render feature integration '" +
                        integration_name + "'");
            if (required_features.empty()) {
                throw std::runtime_error(
                    "render feature integration requires at least one "
                    "feature: " +
                    integration_name);
            }
            std::unordered_set<std::string>
                unique_requirements;
            for (const auto &required :
                 required_features) {
                if (required.empty() ||
                    !unique_requirements
                         .insert(required)
                         .second) {
                    throw std::runtime_error(
                        "render feature integration has an empty or "
                        "duplicate requirement: " +
                        integration_name);
                }
            }
            if (!std::all_of(
                    required_features.begin(),
                    required_features.end(),
                    [&](const auto &required) {
                        return enabled_feature_names
                            .contains(required);
                    })) {
                continue;
            }
            std::vector<std::string>
                required_passes;
            if (integration.contains(
                    "requires_passes")) {
                required_passes =
                    parseStringArray(
                        integration,
                        "requires_passes",
                        "render feature integration '" +
                            integration_name + "'");
            }
            if (integration.contains(
                    "requires_passes") &&
                required_passes.empty()) {
                throw std::runtime_error(
                    "render feature integration requires_passes must not "
                    "be empty: " +
                    integration_name);
            }
            std::unordered_set<std::string>
                unique_pass_requirements;
            for (const auto &required :
                 required_passes) {
                if (required.empty() ||
                    !unique_pass_requirements
                         .insert(required)
                         .second) {
                    throw std::runtime_error(
                        "render feature integration has an empty or "
                        "duplicate pass requirement: " +
                        integration_name);
                }
            }
            if (!std::all_of(
                    required_passes.begin(),
                    required_passes.end(),
                    [&](const auto &required) {
                        return base_pass_names
                            .contains(required);
                    })) {
                continue;
            }
            if (!integration.contains(
                    "fragment") ||
                !integration.at("fragment")
                     .is_object()) {
                throw std::runtime_error(
                    "render feature integration fragment must be an "
                    "object: " +
                    integration_name);
            }
            const auto &fragment =
                integration.at("fragment");
            constexpr std::array
                supported_fragment_fields{
                    std::string_view{
                        "render_targets"},
                    std::string_view{"buffers"},
                    std::string_view{
                        "render_target_overrides"},
                    std::string_view{"passes"},
                    std::string_view{
                        "pass_overrides"},
                    std::string_view{
                        "compute_tasks"},
                    std::string_view{
                        "surface_resources"},
                    std::string_view{
                        "shader_defines"},
                };
            for (auto field =
                     fragment.begin();
                 field != fragment.end();
                 ++field) {
                if (std::find(
                        supported_fragment_fields
                            .begin(),
                        supported_fragment_fields
                            .end(),
                        field.key()) ==
                    supported_fragment_fields
                        .end()) {
                    throw std::runtime_error(
                        "render feature integration fragment has "
                        "unsupported field '" +
                        field.key() + "': " +
                        integration_name);
                }
            }
            const auto integration_provider =
                provider.name + "/" +
                integration_name;
            addRenderTargets(
                composed, fragment,
                target_names,
                integration_provider,
                provider.reference,
                resource_provenance);
            addBuffers(
                composed, fragment,
                buffer_names,
                integration_provider,
                provider.reference,
                resource_provenance);
            applyRenderTargetOverrides(
                composed, fragment);
            addFeaturePasses(
                composed, fragment,
                pass_names,
                integration_provider,
                provider.reference,
                pass_provenance);
            applyPassOverrides(
                composed, fragment);
            if (dependencies.applied_integration)
                dependencies.applied_integration(provider.reference, fragment);
            addFeatureComputeTasks(
                composed, fragment,
                task_names,
                integration_provider,
                provider.reference,
                pass_provenance);
            appendShaderDefines(
                shader_defines, fragment,
                "render feature integration '" +
                    integration_name + "'");
            if (fragment.contains(
                    "surface_resources")) {
                pending_integration_surface_resources
                    .push_back({
                        fragment,
                        provider.name + "/" +
                            integration_name,
                        provider.reference,
                    });
            }
        }
    }
    // LP-3a: the graph's placed passes join the composed passes before the
    // surface resources pick their consumers, so a render.material_pass the
    // graph put at a preset's or a feature's pass is picked as the preset's
    // own material pass would be (docs/design_graph_loops.md 14.6).
    // LP-0d: first, a graph node that depends on a feature the variant left
    // out is made inactive, so it is neither placed nor picked; the stages
    // after this take the graph without it (active_graph).
    std::vector<RenderGraphDeactivatedNode> deactivated_graph_nodes;
    LoweredRenderGraphDocument active_graph;
    if (common_graph) {
        active_graph = withoutExcludedGraphDependents(composed, lowered_graph, graph_features, deactivated_graph_nodes);
        // A replacement whose node the variant made inactive is not applied;
        // the feature keeps its own pass in that variant.
        std::erase_if(graph_features.replacements, [&](const RenderFeaturePassReplacement &replacement) {
            return std::any_of(deactivated_graph_nodes.begin(), deactivated_graph_nodes.end(),
                               [&](const RenderGraphDeactivatedNode &node) { return node.node == replacement.node; });
        });
        placeRenderGraphPassRuns(composed, active_graph);
    }
    // A feature-owned surface resource selects consumers semantically. Run
    // that selection only after every feature has inserted its passes so the
    // result does not depend on whether a producer such as directional
    // shadows was listed before or after a secondary-view consumer.
    for (const auto &pending :
         pending_surface_resources) {
        applySurfaceResourceContracts(
            composed, pending.feature,
            pending.name,
            pending.reference,
            surface_resource_contracts);
    }
    for (const auto &pending :
         pending_integration_surface_resources) {
        applySurfaceResourceContracts(
            composed, pending.feature,
            pending.name,
            pending.reference,
            surface_resource_contracts);
    }
    resolveInheritedPassBindings(
        composed);

    if (!shader_defines.empty()) {
        composed["shader_defines"] = shader_defines;
    }
    for (auto &target : ensureArray(composed, "render_targets")) {
        if (!target.contains("format_class")) {
            target["format_class"] = inferFormatClass(target);
        }
        if (!target.contains("role")) {
            const auto format_class = target.at("format_class").get<std::string>();
            target["role"] = (format_class == "scene" || format_class == "display") ? "color" : "data";
        }
    }
    retargetSwapchainAliases(composed);
    materializeSnapshots(composed);
    // Every target the rendering section, the preset, the features and the
    // snapshots declare now exists; pass order is not settled yet.
    std::vector<RenderGraphLoopReport> graph_loops;
    // GL-2a: the graph passes and tasks that took the place of feature passes.
    std::map<std::string, std::vector<RenderFeaturePassReplacement>> graph_replacements;
    // RS-6: the passes and tasks the graph's loops expanded.
    std::set<std::string> graph_loop_declarations;
    // LP-0d: pass_replacements name a graph node, so they need a graph.
    if (!common_graph && !graph_features.replacements.empty()) {
        const auto &replacement = graph_features.replacements.front();
        throw GraphRegistryError("graph.node_missing@1", {{"node", replacement.node},
                                                          {"feature", replacement.feature},
                                                          {"pass", replacement.pass}});
    }
    if (common_graph) {
        // LP-0d: which feature declared each pass and task (an integration's
        // are its provider's), and the surface resources a replacement may
        // take over.
        for (const auto &provenance : pass_provenance) {
            if (provenance.source != RenderPipelineProvenanceSource::feature) continue;
            graph_features.feature_passes.emplace(
                provenance.name, provenance.provider_feature.substr(0, provenance.provider_feature.find('/')));
        }
        graph_features.surface_resource_contracts = &surface_resource_contracts;
        // LP-Vb: a graph.loop over a view axis is as long as the axis, which
        // the family's provider reads off what was composed (a directional
        // shadow's cascade_count).
        if (dependencies.view_axis_length)
            graph_features.view_axis_length = [&](std::string_view family, std::string_view axis) {
                const auto inputs =
                    compileRenderViewAxisLengthInputs(resolved_instances, surface_resource_contracts);
                return dependencies.view_axis_length(family, axis, inputs.feature_instances,
                                                     inputs.surface_resource_contracts);
            };
        auto loop_resolution = resolveGraphDocumentAfterComposition(composed, active_graph, graph_features);
        for (const auto &[name, origin] : loop_resolution.passes) graph_loop_declarations.insert(name);
        for (const auto &[name, origin] : loop_resolution.resources) graph_images.insert(name);
        // LP-Vb (docs/design_graph_loops.md 14.7): a loop's copies joined
        // after the surface resources picked their consumers, possibly in the
        // place of feature passes that were consumers (a shadow's receivers).
        // They are picked the same way now, and then inherit bindings as the
        // composed passes did.
        if (!loop_resolution.passes.empty()) {
            std::set<std::string> copies;
            for (const auto &[name, origin] : loop_resolution.passes) copies.insert(name);
            for (const auto *pending_list : {&pending_surface_resources, &pending_integration_surface_resources})
                for (const auto &pending : *pending_list)
                    selectSurfaceResourceConsumersAmong(composed, pending.feature, surface_resource_contracts,
                                                        copies);
            resolveInheritedPassBindings(composed);
        }
        // LP-1a: a loop's copies of a render.image are declared here, after
        // the format classes and roles above were settled; they take theirs
        // the same way.
        for (auto &target : ensureArray(composed, "render_targets")) {
            if (!target.contains("format_class")) {
                target["format_class"] = inferFormatClass(target);
            }
            if (!target.contains("role")) {
                const auto format_class = target.at("format_class").get<std::string>();
                target["role"] = (format_class == "scene" || format_class == "display") ? "color" : "data";
            }
        }
        // Placed graph passes were not in the config the first provenance
        // pass saw; they come from the same authoring as the appended ones.
        for (const auto &run : active_graph.placed_runs) {
            for (const auto &pass : run.passes) {
                pass_provenance.push_back(RenderPassProvenance{
                    .name = requireStringField(pass, "name", "pass"),
                    .source = authored_source,
                });
            }
        }
        // LP-1a: what a loop expanded comes from the same authoring, and
        // says which loop, element and body node it is.
        for (auto &[name, origin] : loop_resolution.passes) {
            pass_provenance.push_back(RenderPassProvenance{
                .name = name,
                .source = authored_source,
                .loop_origin = std::move(origin),
            });
        }
        for (auto &[name, origin] : loop_resolution.resources) {
            resource_provenance.push_back(RenderResourceProvenance{
                .name = name,
                .source = authored_source,
                .loop_origin = std::move(origin),
            });
        }
        graph_loops = std::move(loop_resolution.loops);
        graph_replacements = std::move(loop_resolution.replacements);
    }
    for (auto &pass_set : ensureArray(composed, "rendering_passes")) {
        enforceCanonicalOrder(pass_set.at("passes"));
    }
    enforceTerminalAfterComputeTasks(composed);
    // RS-6: a scene pass of the rendering section or the preset that reads an
    // image the graph's compute task writes waits on that task (RS-3 placed
    // the graph's passes the task waits on before it already).
    std::vector<GraphTaskReaderWait> graph_task_waits;
    if (common_graph) {
        const auto graph_made = [&](const std::string &name) {
            return active_graph.graph_declarations.contains(name) || graph_loop_declarations.contains(name);
        };
        std::set<std::string> graph_task_names;
        for (const auto &task : ensureArray(composed, "compute_tasks"))
            if (const auto name = task.value("name", std::string{}); graph_made(name)) graph_task_names.insert(name);
        graph_task_waits = waitOnGraphTasksInScenePasses(composed, graph_task_names, graph_images,
            [&](const nlohmann::json &pass) {
                const auto name = pass.value("name", std::string{});
                const auto type = pass.value("type", std::string{});
                return !graph_made(name) && !graph_features.feature_passes.contains(name) && type != "ui" &&
                       type != "debug_draw" && type != "gizmo" && type != "debug_text" &&
                       !passWritesSwapchain(pass);
            });
    }
    synchronizeRenderPipelineProvenance(
        composed, pass_provenance,
        resource_provenance);
    for (const auto &wait : graph_task_waits)
        for (auto &provenance : pass_provenance)
            if (provenance.name == wait.pass &&
                std::find(provenance.added_after.begin(), provenance.added_after.end(), wait.task) ==
                    provenance.added_after.end())
                provenance.added_after.push_back(wait.task);
    // GL-2a: which graph node made each surviving pass and task. A
    // declaration of the root or an instance is named after its node's
    // expanded id; a loop's copy finds its body node through the branch its
    // element took. A feature's pass is never a node's, even when a node of
    // the same name was made inactive.
    if (common_graph) {
        std::map<std::pair<std::string, std::int64_t>, std::size_t> element_branches;
        for (const auto &loop : graph_loops)
            for (const auto &element : loop.elements)
                element_branches.emplace(std::pair{loop.node, element.element}, element.branch);
        for (auto &provenance : pass_provenance) {
            if (provenance.source == RenderPipelineProvenanceSource::feature) continue;
            if (provenance.loop_origin) {
                const auto &copy = *provenance.loop_origin;
                const auto branch = element_branches.find(std::pair{copy.loop, copy.element});
                if (branch == element_branches.end()) continue;
                const auto found = active_graph.origins.find(copy.loop + "/$" + std::to_string(branch->second) +
                                                             "/" + copy.node);
                if (found == active_graph.origins.end()) continue;
                auto origin = found->second;
                // The template's prefix, not an instance.
                origin.instance_path.clear();
                provenance.graph_origin = std::move(origin);
            } else if (active_graph.graph_declarations.contains(provenance.name)) {
                if (const auto found = active_graph.origins.find(provenance.name);
                    found != active_graph.origins.end())
                    provenance.graph_origin = found->second;
            }
            if (const auto replaced = graph_replacements.find(provenance.name);
                replaced != graph_replacements.end() && provenance.graph_origin)
                for (const auto &entry : replaced->second)
                    provenance.replaces.push_back({entry.feature, entry.pass});
        }
    }
    auto material_routing = resolveMaterialRoutingTable(composed);
    auto draw_sort = composed.contains("draw_sort")
                         ? composed.at("draw_sort")
                         : nlohmann::json{};
    RenderFeatureComposeResult result;
    result.config = std::move(composed);
    result.shader_defines = std::move(shader_defines);
    result.feature_names = std::move(feature_names);
    result.excluded_feature_names = std::move(excluded_feature_names);
    result.projection_jitter = std::move(projection_jitter);
    result.feature_instances = std::move(resolved_instances);
    result.surface_resource_contracts =
        std::move(surface_resource_contracts);
    result.material_routing = std::move(material_routing);
    result.draw_sort = std::move(draw_sort);
    result.pipeline_preset = preset_resolution.preset;
    result.pass_provenance =
        std::move(pass_provenance);
    result.resource_provenance =
        std::move(resource_provenance);
    result.graph_loops = std::move(graph_loops);
    // LP-3a: each draw_loop becomes the draw-step rule of its material pass.
    if (!active_graph.draw_loops.empty()) {
        auto rules = std::make_shared<MaterialDrawLoopRules>();
        for (auto &loop : active_graph.draw_loops) {
            auto pass = loop.pass;
            rules->passes.insert_or_assign(std::move(pass), makeGraphDrawLoopPassRule(std::move(loop)));
        }
        result.material_draw_loops = std::move(rules);
    }
    result.deactivated_graph_nodes = std::move(deactivated_graph_nodes);
    result.used_features = !feature_instances.empty();
    return result;
}

} // namespace Pelican
