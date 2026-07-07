#include "featurecompose.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Pelican {

namespace {

constexpr std::string_view feature_schema = "pelican.render_feature";
constexpr int supported_feature_version = 1;
constexpr std::string_view runtime_compiler_required_message =
    "render feature には実行時コンパイラが必要です (runtime shader compiler is required)";

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
            if (field.key() != "format" && field.key() != "usage") {
                throw std::runtime_error("render target override only supports format and usage: " + name);
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
    auto anchored_pass = pass;
    appendStringListValue(anchored_pass, after ? "after" : "before", anchor);
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
    if (feature_refs.empty()) {
        return RenderFeatureComposeResult{config, std::move(shader_defines), {}, false};
    }
    if (!dependencies.runtime_shader_compiler_enabled) {
        throw std::runtime_error(std::string{runtime_compiler_required_message});
    }

    auto composed = config;
    composed.erase("features");

    auto target_names = collectRenderTargetNames(composed);
    auto pass_names = collectPassNames(composed);
    auto buffer_names = collectBufferNames(composed);
    auto task_names = collectComputeTaskNames(composed);
    std::vector<std::string> feature_names;

    for (const auto &feature_ref : feature_refs) {
        auto feature = loadFeatureJson(feature_ref, dependencies);
        appendUnique(feature_names, validateFeatureEnvelope(feature, feature_ref));
        addRenderTargets(composed, feature, target_names);
        addBuffers(composed, feature, buffer_names);
        applyRenderTargetOverrides(composed, feature);
        addFeaturePasses(composed, feature, pass_names);
        addFeatureComputeTasks(composed, feature, task_names);
        appendShaderDefines(shader_defines, feature, "render feature");
    }

    if (!shader_defines.empty()) {
        composed["shader_defines"] = shader_defines;
    }
    return RenderFeatureComposeResult{std::move(composed), std::move(shader_defines),
                                      std::move(feature_names), true};
}

} // namespace Pelican
