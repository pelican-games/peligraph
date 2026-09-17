#include "rendercomponent.hpp"
#include "renderconfigdocument.hpp"

#include <algorithm>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;

std::string readBytes(const std::filesystem::path &path,
                      const std::string &code) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw RenderComponentError(
            code, "could not open render component file: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path &path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw RenderComponentError(
            "render_component_write_failed",
            "could not write render component file: " + path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw RenderComponentError(
            "render_component_write_failed",
            "could not finish render component file: " + path.string());
    }
}

// Every string value in the document, wherever it sits.  A field-by-field
// parse loses shader_assets, ray_tracing hit groups, integrations and
// pass_overrides; this cannot.
void forEachString(const Json &value,
                   const std::function<void(const std::string &)> &visit) {
    if (value.is_string()) {
        visit(value.get_ref<const std::string &>());
        return;
    }
    if (value.is_object() || value.is_array()) {
        for (const auto &child : value) forEachString(child, visit);
    }
}

void collectTargetNames(const Json &value, std::set<std::string> &names) {
    if (value.is_string()) {
        names.insert(value.get<std::string>());
        return;
    }
    if (value.is_object()) {
        const auto target = value.find("target");
        if (target != value.end() && target->is_string())
            names.insert(target->get<std::string>());
        return;
    }
    if (value.is_array())
        for (const auto &entry : value) collectTargetNames(entry, names);
}

void collectPassTargets(const Json &fragment, std::set<std::string> &names) {
    const auto entries = fragment.find("passes");
    if (entries != fragment.end() && entries->is_array()) {
        for (const auto &entry : *entries) {
            if (!entry.is_object()) continue;
            const auto pass = entry.find("pass");
            if (pass == entry.end() || !pass->is_object()) continue;
            if (const auto input = pass->find("input"); input != pass->end())
                collectTargetNames(*input, names);
            if (const auto output = pass->find("output");
                output != pass->end() && output->is_object()) {
                for (const auto &[field, value] : output->items()) {
                    (void)field;
                    collectTargetNames(value, names);
                }
            }
        }
    }
    const auto overrides = fragment.find("pass_overrides");
    if (overrides != fragment.end() && overrides->is_object()) {
        for (const auto &[name, value] : overrides->items()) {
            (void)name;
            if (!value.is_object()) continue;
            if (const auto input = value.find("input"); input != value.end())
                collectTargetNames(*input, names);
            if (const auto output = value.find("output");
                output != value.end() && output->is_object()) {
                for (const auto &[field, entry] : output->items()) {
                    (void)field;
                    collectTargetNames(entry, names);
                }
            }
        }
    }
}

std::string temporaryDirectoryName(const std::filesystem::path &destination) {
#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif
    return destination.filename().string() + ".tmp-" + std::to_string(pid);
}

} // namespace

RenderComponentError::RenderComponentError(std::string code,
                                           const std::string &message)
    : std::runtime_error(message), code_{std::move(code)} {}

std::vector<std::string> renderComponentRequiredTargets(const Json &feature) {
    std::set<std::string> used;
    collectPassTargets(feature, used);
    const auto integrations = feature.find("integrations");
    if (integrations != feature.end() && integrations->is_array()) {
        for (const auto &integration : *integrations) {
            if (!integration.is_object()) continue;
            const auto fragment = integration.find("fragment");
            if (fragment != integration.end() && fragment->is_object())
                collectPassTargets(*fragment, used);
        }
    }
    const auto overrides = feature.find("render_target_overrides");
    if (overrides != feature.end() && overrides->is_object())
        for (const auto &[name, value] : overrides->items()) {
            (void)value;
            used.insert(name);
        }

    std::set<std::string> declared;
    const auto targets = feature.find("render_targets");
    if (targets != feature.end() && targets->is_array())
        for (const auto &target : *targets) {
            if (target.is_string()) declared.insert(target.get<std::string>());
            else if (target.is_object() && target.contains("name") &&
                     target.at("name").is_string())
                declared.insert(target.at("name").get<std::string>());
        }

    std::vector<std::string> required;
    for (const auto &name : used)
        if (!declared.contains(name) && !name.empty())
            required.push_back(name);
    return required;
}

RenderComponent buildRenderComponent(std::string_view feature_bytes,
                                     std::string_view reference,
                                     std::string_view project) {
    if (reference.starts_with("engine://")) {
        throw RenderComponentError(
            "render_component_engine_reference",
            "engine features are already shared; export a project derivative instead: " +
                std::string{reference});
    }
    Json feature;
    try {
        feature = Json::parse(feature_bytes);
    } catch (const Json::exception &error) {
        throw RenderComponentError("render_component_invalid",
                                   std::string{"feature document is not valid JSON: "} +
                                       error.what());
    }
    if (!feature.is_object() ||
        feature.value("schema", std::string{}) != "pelican.render_feature") {
        throw RenderComponentError(
            "render_component_invalid",
            "a render component is one pelican.render_feature document");
    }
    const auto name = feature.value("name", std::string{});
    if (name.empty() ||
        name.find_first_of("/\\:.") != std::string::npos) {
        throw RenderComponentError(
            "render_component_invalid",
            "the feature needs a name usable as one directory name: " + name);
    }
    forEachString(feature, [&](const std::string &value) {
        if (value.starts_with("project://")) {
            throw RenderComponentError(
                "render_component_project_reference",
                "the feature still points into its project and cannot be shared: " +
                    value);
        }
    });

    RenderComponent component;
    component.name = name;
    component.feature_bytes = std::string{feature_bytes};
    component.feature_digest = renderConfigSourceDigest(feature_bytes);
    component.required_render_targets = renderComponentRequiredTargets(feature);
    nlohmann::ordered_json manifest{
        {"schema", std::string{renderComponentSchema}},
        {"version", renderComponentVersion},
        {"name", name},
        {"feature", std::string{renderComponentFeatureFile}},
        {"sha256", component.feature_digest},
        {"source",
         {{"project", std::string{project}},
          {"reference", std::string{reference}}}},
        {"requires",
         {{"render_targets", component.required_render_targets}}},
    };
    component.manifest_bytes = manifest.dump(2) + "\n";
    return component;
}

void writeRenderComponent(const std::filesystem::path &destination,
                          const RenderComponent &component) {
    std::error_code error;
    if (std::filesystem::exists(destination, error)) {
        throw RenderComponentError(
            "render_component_destination_exists",
            "the component directory already exists: " + destination.string());
    }
    const auto staging =
        destination.parent_path() / temporaryDirectoryName(destination);
    std::filesystem::remove_all(staging, error);
    std::filesystem::create_directories(staging, error);
    if (error) {
        throw RenderComponentError(
            "render_component_write_failed",
            "could not create the component staging directory: " +
                staging.string());
    }
    try {
        // feature.json first: the manifest is the completion marker, and the
        // directory only becomes visible under its real name once both are
        // written.
        writeBytes(staging / renderComponentFeatureFile,
                   component.feature_bytes);
        writeBytes(staging / renderComponentManifestFile,
                   component.manifest_bytes);
        std::filesystem::rename(staging, destination, error);
        if (error) {
            throw RenderComponentError(
                "render_component_write_failed",
                "could not publish the component directory: " +
                    destination.string() + ": " + error.message());
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }
}

RenderComponent readRenderComponent(const std::filesystem::path &source) {
    std::error_code error;
    if (!std::filesystem::is_directory(source, error) || error) {
        throw RenderComponentError(
            "render_component_invalid",
            "the component source is not a directory: " + source.string());
    }
    const auto manifest_bytes =
        readBytes(source / renderComponentManifestFile,
                  "render_component_invalid");
    Json manifest;
    try {
        manifest = Json::parse(manifest_bytes);
    } catch (const Json::exception &manifest_error) {
        throw RenderComponentError(
            "render_component_invalid",
            std::string{"component.json is not valid JSON: "} +
                manifest_error.what());
    }
    if (!manifest.is_object() ||
        manifest.value("schema", std::string{}) !=
            std::string{renderComponentSchema} ||
        manifest.value("version", 0) != renderComponentVersion) {
        throw RenderComponentError(
            "render_component_invalid",
            "component.json must be a pelican.render_component version 1 document");
    }
    const auto feature_file =
        manifest.value("feature", std::string{renderComponentFeatureFile});
    if (feature_file.find('/') != std::string::npos ||
        feature_file.find('\\') != std::string::npos ||
        feature_file.empty()) {
        throw RenderComponentError(
            "render_component_invalid",
            "component.json feature must name one file in the component: " +
                feature_file);
    }
    const auto digest = manifest.value("sha256", std::string{});
    const auto name = manifest.value("name", std::string{});
    if (digest.size() != 64 || name.empty()) {
        throw RenderComponentError(
            "render_component_invalid",
            "component.json requires a name and a sha256 digest");
    }
    RenderComponent component;
    component.name = name;
    component.manifest_bytes = manifest_bytes;
    component.feature_bytes =
        readBytes(source / feature_file, "render_component_invalid");
    component.feature_digest =
        renderConfigSourceDigest(component.feature_bytes);
    if (component.feature_digest != digest) {
        throw RenderComponentError(
            "render_component_digest_mismatch",
            "the component feature does not match the digest it was published with");
    }
    const auto requires_entry = manifest.find("requires");
    if (requires_entry != manifest.end()) {
        if (!requires_entry->is_object()) {
            throw RenderComponentError("render_component_invalid",
                                       "component.json requires must be an object");
        }
        const auto targets = requires_entry->find("render_targets");
        if (targets != requires_entry->end()) {
            if (!targets->is_array()) {
                throw RenderComponentError(
                    "render_component_invalid",
                    "component.json requires.render_targets must be an array");
            }
            for (const auto &target : *targets) {
                if (!target.is_string()) {
                    throw RenderComponentError(
                        "render_component_invalid",
                        "component.json requires.render_targets must hold names");
                }
                component.required_render_targets.push_back(
                    target.get<std::string>());
            }
        }
    }
    return component;
}

} // namespace Pelican
