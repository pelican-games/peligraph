#include "featurecompose.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Pelican {

namespace {

constexpr std::string_view feature_schema = "pelican.render_feature";
constexpr int supported_feature_version = 1;
constexpr int color_format_resolver_version = 2;
constexpr std::string_view runtime_compiler_required_message =
    "render feature には実行時コンパイラが必要です (runtime shader compiler is required)";
constexpr std::array<std::string_view, 7> canonical_anchors = {
    "post_main", "tonemap", "post_ldr", "pelican_ui", "debug_draw", "debug_text", "imgui"};

std::unordered_set<std::string> collectRenderTargetNames(const nlohmann::json &config);

std::string canonicalAnchorNodeName(std::string_view anchor) {
    return "__anchor_" + std::string{anchor};
}

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
    return requireStringField(feature, "name", "render feature");
}

nlohmann::json loadFeatureJson(std::string_view ref,
                               const RenderFeatureComposeDependencies &dependencies) {
    if (!dependencies.load_feature_json) {
        throw std::runtime_error("render feature loader is not configured: " + std::string{ref});
    }
    return nlohmann::json::parse(dependencies.load_feature_json(ref));
}

std::vector<std::string> parseFeatureRefs(const nlohmann::json &config) {
    if (!config.contains("features")) {
        return {};
    }
    const auto &features = config.at("features");
    if (!features.is_array()) {
        throw std::runtime_error("rendering config features must be an array");
    }

    std::vector<std::string> refs;
    refs.reserve(features.size());
    for (const auto &feature_ref : features) {
        if (!feature_ref.is_string()) {
            throw std::runtime_error("rendering config features entries must be strings");
        }
        refs.push_back(feature_ref.get<std::string>());
    }
    return refs;
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
    if (name == "gbuffer_albedo" || name == "g_emissive" || name == "lit_color" ||
        name.rfind("Bloom_", 0) == 0) {
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
        return 3;
    }
    if (type == "debug_draw") {
        return 4;
    }
    if (type == "debug_text") {
        return 5;
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
    const bool bloom = name == "HighLuminanceExtraction" || name == "FinalBloomComposite" ||
                       name.rfind("HorizontalBlur_", 0) == 0 || name.rfind("VerticalBlur_", 0) == 0 ||
                       name.rfind("UpsampleBlend_", 0) == 0;
    if (bloom || passWritesSwapchain(pass)) {
        return hdr_enabled ? 0 : 2;
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
            if (type != "ui" && type != "debug_draw" && type != "debug_text" &&
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
        std::array<nlohmann::json, 7> buckets;
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

void addRenderTargets(nlohmann::json &config, const nlohmann::json &feature,
                      std::unordered_set<std::string> &target_names) {
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
        if (!target_names.insert(name).second) {
            throw std::runtime_error("Render feature render target name collides: " + name);
        }
        targets.push_back(target);
    }
}

void addBuffers(nlohmann::json &config, const nlohmann::json &feature,
                std::unordered_set<std::string> &buffer_names) {
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
        if (!buffer_names.insert(name).second) {
            throw std::runtime_error("Render feature buffer name collides: " + name);
        }
        buffers.push_back(buffer);
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
                field.key() != "width" && field.key() != "height") {
                throw std::runtime_error(
                    "render target override only supports format, usage, width, and height: " + name);
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

struct AnchorMatch {
    nlohmann::json *passes = nullptr;
    size_t index = 0;
};

std::vector<AnchorMatch> findAnchorMatches(nlohmann::json &config, const std::string &anchor_name) {
    std::vector<AnchorMatch> matches;
    auto &rendering_passes = ensureArray(config, "rendering_passes");
    for (auto &pass_set : rendering_passes) {
        auto &passes = pass_set.at("passes");
        for (size_t i = 0; i < passes.size(); ++i) {
            if (passes.at(i).is_object() &&
                passes.at(i).value("type", std::string{}) == "canonical_anchor" &&
                passes.at(i).value("anchor", std::string{}) == anchor_name) {
                matches.push_back(AnchorMatch{&passes, i});
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
                matches.push_back(AnchorMatch{&passes, i});
            }
        }
    }
    return matches;
}

void insertPassAtEnd(nlohmann::json &config, const nlohmann::json &pass) {
    auto &rendering_passes = ensureArray(config, "rendering_passes");
    if (rendering_passes.size() != 1) {
        throw std::runtime_error("render feature insert:end requires exactly one rendering pass");
    }
    auto &passes = rendering_passes.at(0).at("passes");
    passes.push_back(pass);
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
        throw std::runtime_error("render feature pass insert must be before:<pass>, after:<pass>, or end");
    }
    if (anchor.empty()) {
        throw std::runtime_error("render feature pass insert anchor must not be empty");
    }

    auto matches = findAnchorMatches(config, anchor);
    if (matches.empty()) {
        throw std::runtime_error("render feature pass insert anchor was not found: " + anchor);
    }
    if (matches.size() > 1) {
        throw std::runtime_error("render feature pass insert anchor is ambiguous: " + anchor);
    }

    auto &passes = *matches.front().passes;
    auto index = matches.front().index + (after ? 1 : 0);
    const bool canonical_anchor = passes.at(matches.front().index).value("type", std::string{}) ==
                                  "canonical_anchor";
    if (after && canonical_anchor) {
        while (index < passes.size() &&
               passes.at(index).value("type", std::string{}) != "canonical_anchor" &&
               passes.at(index).value("type", std::string{}) != "output_transform") {
            ++index;
        }
    }
    auto anchored_pass = pass;
    const auto dependency_name =
        passes.at(matches.front().index).value("name", std::string{});
    appendStringListValue(anchored_pass, after ? "after" : "before", dependency_name);
    passes.insert(passes.begin() + static_cast<nlohmann::json::difference_type>(index), anchored_pass);
}

void addFeaturePasses(nlohmann::json &config, const nlohmann::json &feature,
                      std::unordered_set<std::string> &pass_names) {
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

        if (insert == "end") {
            insertPassAtEnd(config, *pass_it);
        } else {
            insertPassByAnchor(config, insert, *pass_it);
        }
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

        for (auto field = override_json.begin(); field != override_json.end(); ++field) {
            if (field.key() != "input") {
                throw std::runtime_error("render feature pass override only supports input: " + pass_name);
            }
        }
        if (override_json.contains("input")) {
            for (const auto &input : parseStringArray(override_json, "input",
                                                      "render feature pass override: " + pass_name)) {
                appendStringListValue(*pass, "input", input);
            }
        }
    }
}

void addFeatureComputeTasks(nlohmann::json &config, const nlohmann::json &feature,
                            std::unordered_set<std::string> &task_names) {
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
    }
}

void appendShaderDefines(std::vector<std::string> &defines, const nlohmann::json &json,
                         std::string_view context) {
    if (!json.contains("shader_defines")) {
        return;
    }
    appendUnique(defines, parseStringArray(json, "shader_defines", context));
}

} // namespace

RenderFeatureComposeResult composeRenderFeatureConfig(
    const nlohmann::json &config,
    const RenderFeatureComposeDependencies &dependencies) {
    if (!config.is_object()) {
        throw std::runtime_error("Rendering config must be an object");
    }

    const auto feature_refs = parseFeatureRefs(config);
    std::vector<std::string> shader_defines;
    appendShaderDefines(shader_defines, config, "rendering config");
    if (!feature_refs.empty() && !dependencies.runtime_shader_compiler_enabled) {
        throw std::runtime_error(std::string{runtime_compiler_required_message});
    }

    std::vector<std::pair<std::string, nlohmann::json>> loaded_features;
    std::vector<std::string> feature_names;
    bool hdr_enabled = false;
    loaded_features.reserve(feature_refs.size());
    for (const auto &feature_ref : feature_refs) {
        auto feature = loadFeatureJson(feature_ref, dependencies);
        const auto feature_name = validateFeatureEnvelope(feature, feature_ref);
        appendUnique(feature_names, feature_name);
        hdr_enabled = hdr_enabled || feature_name == "hdr";
        loaded_features.emplace_back(feature_ref, std::move(feature));
    }

    auto composed = config;
    composed.erase("features");
    initializeCanonicalColorPipeline(composed, hdr_enabled);

    auto target_names = collectRenderTargetNames(composed);
    auto pass_names = collectPassNames(composed);
    auto buffer_names = collectBufferNames(composed);
    auto task_names = collectComputeTaskNames(composed);
    for (const auto &[feature_ref, feature] : loaded_features) {
        (void)feature_ref;
        addRenderTargets(composed, feature, target_names);
        addBuffers(composed, feature, buffer_names);
        applyRenderTargetOverrides(composed, feature);
        addFeaturePasses(composed, feature, pass_names);
        applyPassOverrides(composed, feature);
        addFeatureComputeTasks(composed, feature, task_names);
        appendShaderDefines(shader_defines, feature, "render feature");
    }

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
    for (auto &pass_set : ensureArray(composed, "rendering_passes")) {
        enforceCanonicalOrder(pass_set.at("passes"));
    }
    enforceTerminalAfterComputeTasks(composed);
    return RenderFeatureComposeResult{std::move(composed), std::move(shader_defines),
                                      std::move(feature_names), !feature_refs.empty()};
}

} // namespace Pelican
