#include "renderfeatureoverlay.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

constexpr std::string_view overlay_schema =
    "pelican.render_feature_overlay";
constexpr int supported_overlay_version = 1;

void requireOnlyOverlayKeys(const nlohmann::json &document,
                            std::string_view reference) {
    for (auto field = document.begin(); field != document.end(); ++field) {
        if (field.key() != "schema" && field.key() != "version" &&
            field.key() != "name" && field.key() != "features") {
            throw std::runtime_error(
                "render feature overlay '" + std::string{reference} +
                "' has unknown key '" + field.key() + "'");
        }
    }
}

void validateFeatureEntry(const nlohmann::json &entry,
                          std::string_view reference) {
    if (entry.is_string()) {
        if (entry.get_ref<const std::string &>().empty()) {
            throw std::runtime_error(
                "render feature overlay '" + std::string{reference} +
                "' contains an empty feature reference");
        }
        return;
    }
    if (!entry.is_object()) {
        throw std::runtime_error(
            "render feature overlay '" + std::string{reference} +
            "' features entries must be strings or {ref, parameters} objects");
    }
    for (auto field = entry.begin(); field != entry.end(); ++field) {
        if (field.key() != "ref" && field.key() != "parameters") {
            throw std::runtime_error(
                "render feature overlay '" + std::string{reference} +
                "' feature instance has unknown key '" + field.key() + "'");
        }
    }
    if (!entry.contains("ref") || !entry.at("ref").is_string() ||
        entry.at("ref").get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            "render feature overlay '" + std::string{reference} +
            "' feature instance requires a non-empty string ref");
    }
    if (entry.contains("parameters") && !entry.at("parameters").is_object()) {
        throw std::runtime_error(
            "render feature overlay '" + std::string{reference} +
            "' feature instance parameters must be an object");
    }
}

nlohmann::json loadOverlay(std::string_view reference,
                           const RenderFeatureOverlayLoader &loader) {
    std::string bytes;
    try {
        bytes = loader(reference);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "failed to load render feature overlay '" +
            std::string{reference} + "': " + error.what());
    }

    nlohmann::json document;
    try {
        document = nlohmann::json::parse(bytes);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "failed to parse render feature overlay '" +
            std::string{reference} + "': " + error.what());
    }
    if (!document.is_object()) {
        throw std::runtime_error(
            "render feature overlay must be an object: " +
            std::string{reference});
    }
    requireOnlyOverlayKeys(document, reference);
    if (document.value("schema", std::string{}) != overlay_schema) {
        throw std::runtime_error(
            "render feature overlay schema must be '" +
            std::string{overlay_schema} + "': " + std::string{reference});
    }
    if (!document.contains("version") ||
        !document.at("version").is_number_integer() ||
        document.at("version").get<int>() != supported_overlay_version) {
        throw std::runtime_error(
            "render feature overlay version must be exactly 1: " +
            std::string{reference});
    }
    if (!document.contains("name") || !document.at("name").is_string() ||
        document.at("name").get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            "render feature overlay requires a non-empty string name: " +
            std::string{reference});
    }
    if (!document.contains("features") ||
        !document.at("features").is_array()) {
        throw std::runtime_error(
            "render feature overlay requires a features array: " +
            std::string{reference});
    }
    for (const auto &entry : document.at("features")) {
        validateFeatureEntry(entry, reference);
    }
    return document;
}

} // namespace

nlohmann::json applyRenderFeatureOverlays(
    const nlohmann::json &authored_config,
    std::span<const std::string> overlay_references,
    const RenderFeatureOverlayLoader &load_overlay_json) {
    if (!authored_config.is_object()) {
        throw std::runtime_error("Rendering config must be an object");
    }
    if (overlay_references.empty()) {
        return authored_config;
    }
    if (!load_overlay_json) {
        throw std::runtime_error("render feature overlay loader is unavailable");
    }

    auto result = authored_config;
    if (!result.contains("features")) {
        result["features"] = nlohmann::json::array();
    }
    if (!result.at("features").is_array()) {
        throw std::runtime_error("rendering config features must be an array");
    }
    auto &features = result.at("features");
    for (const auto &reference : overlay_references) {
        if (reference.empty()) {
            throw std::runtime_error(
                "render feature overlay reference must not be empty");
        }
        const auto overlay = loadOverlay(reference, load_overlay_json);
        for (const auto &feature : overlay.at("features")) {
            if (std::find(features.begin(), features.end(), feature) ==
                features.end()) {
                features.push_back(feature);
            }
        }
    }
    return result;
}

} // namespace Pelican
