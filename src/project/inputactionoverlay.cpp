#include "inputactionoverlay.hpp"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace Pelican {
namespace {

constexpr std::string_view overlay_schema =
    "pelican.input_action_overlay";
constexpr int supported_overlay_version = 1;
constexpr std::string_view actions_schema = "pelican.input_actions";
constexpr int supported_actions_version = 1;

void requireOnlyOverlayKeys(const nlohmann::json &document,
                            std::string_view reference) {
    for (auto field = document.begin(); field != document.end(); ++field) {
        if (field.key() != "schema" && field.key() != "version" &&
            field.key() != "name" && field.key() != "actions" &&
            field.key() != "profile") {
            throw std::runtime_error(
                "input action overlay '" + std::string{reference} +
                "' has unknown key '" + field.key() + "'");
        }
    }
}

std::string loadDocument(std::string_view reference,
                         std::string_view kind,
                         const InputActionOverlayLoader &loader) {
    try {
        return loader(reference);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "failed to load " + std::string{kind} + " '" +
            std::string{reference} + "': " + error.what());
    }
}

nlohmann::json parseDocument(std::string_view bytes,
                             std::string_view kind,
                             std::string_view reference) {
    try {
        return nlohmann::json::parse(bytes);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "failed to parse " + std::string{kind} + " '" +
            std::string{reference} + "': " + error.what());
    }
}

nlohmann::json loadOverlay(std::string_view reference,
                           const InputActionOverlayLoader &loader) {
    const auto bytes = loadDocument(reference, "input action overlay", loader);
    auto document = parseDocument(bytes, "input action overlay", reference);
    if (!document.is_object()) {
        throw std::runtime_error(
            "input action overlay must be an object: " +
            std::string{reference});
    }
    requireOnlyOverlayKeys(document, reference);
    if (document.value("schema", std::string{}) != overlay_schema) {
        throw std::runtime_error(
            "input action overlay schema must be '" +
            std::string{overlay_schema} + "': " + std::string{reference});
    }
    if (!document.contains("version") ||
        !document.at("version").is_number_integer() ||
        document.at("version").get<int>() != supported_overlay_version) {
        throw std::runtime_error(
            "input action overlay version must be exactly 1: " +
            std::string{reference});
    }
    if (!document.contains("name") || !document.at("name").is_string() ||
        document.at("name").get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            "input action overlay requires a non-empty string name: " +
            std::string{reference});
    }
    if (!document.contains("actions") ||
        !document.at("actions").is_string() ||
        document.at("actions").get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            "input action overlay requires a non-empty string actions reference: " +
            std::string{reference});
    }
    if (document.contains("profile") &&
        (!document.at("profile").is_string() ||
         document.at("profile").get_ref<const std::string &>().empty())) {
        throw std::runtime_error(
            "input action overlay profile must be a non-empty string reference: " +
            std::string{reference});
    }
    return document;
}

void validateActionsEnvelope(const nlohmann::json &document,
                             std::string_view reference) {
    if (!document.is_object()) {
        throw std::runtime_error(
            "input actions document must be an object: " +
            std::string{reference});
    }
    if (document.value("schema", std::string{}) != actions_schema) {
        throw std::runtime_error(
            "input actions schema must be '" + std::string{actions_schema} +
            "': " + std::string{reference});
    }
    if (!document.contains("version") ||
        !document.at("version").is_number_integer() ||
        document.at("version").get<int>() != supported_actions_version) {
        throw std::runtime_error(
            "input actions version must be exactly 1: " +
            std::string{reference});
    }
    if (!document.contains("action_sets") ||
        !document.at("action_sets").is_array()) {
        throw std::runtime_error(
            "input actions requires an action_sets array: " +
            std::string{reference});
    }
}

std::vector<std::string> collectNames(
    const nlohmann::json &document,
    std::string_view overlay_reference,
    std::unordered_set<std::string> &action_set_names,
    std::unordered_set<std::string> &action_names) {
    std::vector<std::string> added_set_names;
    for (const auto &set : document.at("action_sets")) {
        if (!set.is_object() || !set.contains("name") ||
            !set.at("name").is_string()) {
            throw std::runtime_error(
                "input action overlay '" + std::string{overlay_reference} +
                "' contains an action set without a string name");
        }
        const auto set_name = set.at("name").get<std::string>();
        if (!action_set_names.insert(set_name).second) {
            throw std::runtime_error(
                "input action overlay '" + std::string{overlay_reference} +
                "' action set name collision: " + set_name);
        }
        added_set_names.push_back(set_name);
        if (!set.contains("actions") || !set.at("actions").is_array()) {
            throw std::runtime_error(
                "input action overlay '" + std::string{overlay_reference} +
                "' action set '" + set_name + "' requires an actions array");
        }
        for (const auto &action : set.at("actions")) {
            if (!action.is_object() || !action.contains("name") ||
                !action.at("name").is_string()) {
                throw std::runtime_error(
                    "input action overlay '" +
                    std::string{overlay_reference} + "' action set '" +
                    set_name + "' contains an action without a string name");
            }
            const auto action_name = action.at("name").get<std::string>();
            if (!action_names.insert(action_name).second) {
                throw std::runtime_error(
                    "input action overlay '" +
                    std::string{overlay_reference} +
                    "' action name collision: " + action_name);
            }
        }
    }
    return added_set_names;
}

} // namespace

InputActionOverlayApplication applyInputActionOverlays(
    const std::optional<std::string> &authored_actions_json,
    std::span<const std::string> overlay_references,
    const InputActionOverlayLoader &load_overlay_json) {
    if (overlay_references.empty()) {
        return {.effective_actions_json = authored_actions_json};
    }
    if (!load_overlay_json) {
        throw std::runtime_error("input action overlay loader is unavailable");
    }

    auto effective = authored_actions_json
                         ? parseDocument(*authored_actions_json,
                                         "authored input actions", "project")
                         : nlohmann::json{{"schema", actions_schema},
                                          {"version", supported_actions_version},
                                          {"action_sets", nlohmann::json::array()}};
    validateActionsEnvelope(effective, "project");

    std::unordered_set<std::string> action_set_names;
    std::unordered_set<std::string> action_names;
    (void)collectNames(effective, "project", action_set_names, action_names);
    std::unordered_set<std::string> overlay_names;

    InputActionOverlayApplication result;
    for (const auto &reference : overlay_references) {
        if (reference.empty()) {
            throw std::runtime_error(
                "input action overlay reference must not be empty");
        }
        const auto overlay = loadOverlay(reference, load_overlay_json);
        const auto name = overlay.at("name").get<std::string>();
        if (!overlay_names.insert(name).second) {
            throw std::runtime_error(
                "input action overlay name collision: " + name);
        }

        const auto actions_reference =
            overlay.at("actions").get<std::string>();
        auto actions_json = loadDocument(
            actions_reference, "input action overlay actions", load_overlay_json);
        const auto actions = parseDocument(
            actions_json, "input action overlay actions", actions_reference);
        validateActionsEnvelope(actions, actions_reference);
        auto added_set_names = collectNames(
            actions, reference, action_set_names, action_names);
        for (const auto &set : actions.at("action_sets")) {
            effective.at("action_sets").push_back(set);
        }

        std::optional<std::string> profile_json;
        if (overlay.contains("profile")) {
            const auto profile_reference =
                overlay.at("profile").get<std::string>();
            profile_json = loadDocument(
                profile_reference, "input action overlay profile",
                load_overlay_json);
        }
        result.overlays.push_back(AppliedInputActionOverlay{
            .name = name,
            .reference = reference,
            .actions_json = std::move(actions_json),
            .profile_json = std::move(profile_json),
            .action_set_names = std::move(added_set_names),
        });
    }
    result.effective_actions_json = effective.dump();
    return result;
}

} // namespace Pelican
