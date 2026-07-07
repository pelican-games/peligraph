#include "sceneformat.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace Pelican {

namespace {

constexpr std::string_view scene_schema = "pelican.scene";
constexpr int supported_scene_version = 1;

bool isR7Identifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    for (const char ch : value) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_lower = ch >= 'a' && ch <= 'z';
        if (!is_digit && !is_upper && !is_lower && ch != '_') {
            return false;
        }
    }
    return true;
}

void requireIdentifier(std::string_view value, std::string_view kind, std::string_view context) {
    if (!isR7Identifier(value)) {
        throw std::runtime_error(std::string{kind} + " must match [a-zA-Z0-9_]: " + std::string{value} +
                                 std::string{context});
    }
}

std::string sceneContext(const std::string &scene_id) {
    return " in scene '" + scene_id + "'";
}

std::string objectContext(const std::string &scene_id, const std::string &object_name) {
    const auto display_name = object_name.empty() ? std::string{"<unnamed>"} : object_name;
    return " on object '" + display_name + "' in scene '" + scene_id + "'";
}

void validateEnvelope(const nlohmann::json &document) {
    if (document.value("schema", std::string{}) != scene_schema) {
        throw std::runtime_error("scene data schema is not supported");
    }
    if (!document.contains("version") || !document.at("version").is_number_integer()) {
        throw std::runtime_error("scene data requires numeric version");
    }
    const auto version = document.at("version").get<int>();
    if (version > supported_scene_version) {
        throw std::runtime_error("scene data version is newer than this engine supports");
    }
    if (!document.contains("scenes") || !document.at("scenes").is_object()) {
        throw std::runtime_error("scene data requires scenes object");
    }
}

nlohmann::json legacyLightToObject(const nlohmann::json &light, const std::string &scene_id) {
    if (!light.is_object()) {
        throw std::runtime_error("legacy light entries must be objects" + sceneContext(scene_id));
    }

    nlohmann::json component = light;
    nlohmann::json object = nlohmann::json::object();

    if (const auto name = light.find("name"); name != light.end()) {
        if (!name->is_string()) {
            throw std::runtime_error("legacy light name must be a string" + sceneContext(scene_id));
        }
        object["name"] = name->get<std::string>();
    }

    component.erase("name");
    component["name"] = "light";
    object["components"] = nlohmann::json::array({component});
    return object;
}

nlohmann::json normalizeScene(const nlohmann::json &scene, const std::string &scene_id,
                              std::vector<std::string> &warnings) {
    if (!scene.is_object()) {
        throw std::runtime_error("scene entry must be an object" + sceneContext(scene_id));
    }

    nlohmann::json normalized = scene;
    if (!normalized.contains("objects") || !normalized.at("objects").is_array()) {
        throw std::runtime_error("scene requires objects array" + sceneContext(scene_id));
    }

    if (const auto lights = normalized.find("lights"); lights != normalized.end()) {
        if (!lights->is_array()) {
            throw std::runtime_error("legacy lights must be an array" + sceneContext(scene_id));
        }
        warnings.push_back("legacy lights section converted to light components in scene '" + scene_id + "'");
        for (const auto &light : *lights) {
            normalized["objects"].push_back(legacyLightToObject(light, scene_id));
        }
        normalized.erase("lights");
    }

    std::unordered_set<std::string> object_names;
    for (const auto &object : normalized.at("objects")) {
        if (!object.is_object()) {
            throw std::runtime_error("scene objects entries must be objects" + sceneContext(scene_id));
        }

        std::string object_name;
        if (const auto name = object.find("name"); name != object.end()) {
            if (!name->is_string()) {
                throw std::runtime_error("object name must be a string" + sceneContext(scene_id));
            }
            object_name = name->get<std::string>();
            requireIdentifier(object_name, "object name", sceneContext(scene_id));
            if (!object_names.insert(object_name).second) {
                throw std::runtime_error("duplicate object name '" + object_name + "'" + sceneContext(scene_id));
            }
        }

        if (!object.contains("components") || !object.at("components").is_array()) {
            throw std::runtime_error("object requires components array" + objectContext(scene_id, object_name));
        }

        for (const auto &component : object.at("components")) {
            if (!component.is_object()) {
                throw std::runtime_error("component entries must be objects" + objectContext(scene_id, object_name));
            }
            if (!component.contains("name") || !component.at("name").is_string()) {
                throw std::runtime_error("component requires string name" + objectContext(scene_id, object_name));
            }
            const auto component_name = component.at("name").get<std::string>();
            requireIdentifier(component_name, "component name", objectContext(scene_id, object_name));
        }
    }

    return normalized;
}

} // namespace

SceneFormatDocument normalizeSceneDataJson(const nlohmann::json &scene_data) {
    if (!scene_data.is_object()) {
        throw std::runtime_error("scene data must be an object");
    }

    SceneFormatDocument document;
    nlohmann::json scenes;
    if (scene_data.contains("schema")) {
        validateEnvelope(scene_data);
        scenes = scene_data.at("scenes");
    } else {
        document.warnings.push_back("legacy scene data without schema; treating top-level keys as scenes");
        scenes = scene_data;
    }

    document.scenes = nlohmann::json::object();
    for (auto it = scenes.begin(); it != scenes.end(); ++it) {
        const auto scene_id = it.key();
        requireIdentifier(scene_id, "scene id", "");
        document.scenes[scene_id] = normalizeScene(it.value(), scene_id, document.warnings);
    }
    return document;
}

} // namespace Pelican
