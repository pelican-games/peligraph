#include "sceneformat.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
        throw std::runtime_error(
            "scene data schema is not supported; use schema 'pelican.scene' with version 1 and a 'scenes' object");
    }
    if (!document.contains("version") || !document.at("version").is_number_integer()) {
        throw std::runtime_error("scene data requires numeric version");
    }
    const auto version = document.at("version").get<int>();
    if (version != supported_scene_version) {
        throw std::runtime_error("scene data version must be exactly 1");
    }
    if (!document.contains("scenes") || !document.at("scenes").is_object()) {
        throw std::runtime_error("scene data requires scenes object");
    }
}

nlohmann::json normalizeScene(const nlohmann::json &scene, const std::string &scene_id) {
    if (!scene.is_object()) {
        throw std::runtime_error("scene entry must be an object" + sceneContext(scene_id));
    }

    nlohmann::json normalized = scene;
    if (!normalized.contains("objects") || !normalized.at("objects").is_array()) {
        throw std::runtime_error("scene requires objects array" + sceneContext(scene_id));
    }

    if (const auto lights = normalized.find("lights"); lights != normalized.end()) {
        throw std::runtime_error("scene field 'lights' is not supported in v1; use named objects with a 'light' "
                                 "component" + sceneContext(scene_id));
    }

    std::unordered_map<std::string, size_t> object_name_counts;
    std::vector<std::string> object_names;
    std::vector<std::string> parent_names;
    object_names.reserve(normalized.at("objects").size());
    parent_names.reserve(normalized.at("objects").size());
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
            ++object_name_counts[object_name];
        }

        std::string parent_name;
        if (const auto parent = object.find("parent"); parent != object.end()) {
            if (!parent->is_string()) {
                throw std::runtime_error("object parent must be a string" + objectContext(scene_id, object_name));
            }
            parent_name = parent->get<std::string>();
            requireIdentifier(parent_name, "object parent", objectContext(scene_id, object_name));
        }
        object_names.push_back(object_name);
        parent_names.push_back(parent_name);

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

    // A referenced duplicate is more actionable than the generic duplicate-name
    // diagnostic: the parent reference cannot select one of the candidates.
    for (size_t i = 0; i < parent_names.size(); ++i) {
        const auto &parent_name = parent_names[i];
        if (parent_name.empty()) {
            continue;
        }
        const auto found = object_name_counts.find(parent_name);
        if (found == object_name_counts.end()) {
            throw std::runtime_error("unknown parent '" + parent_name + "'" +
                                     objectContext(scene_id, object_names[i]));
        }
        if (found->second != 1) {
            throw std::runtime_error("ambiguous parent '" + parent_name + "' refers to " +
                                     std::to_string(found->second) + " objects" +
                                     objectContext(scene_id, object_names[i]));
        }
    }

    for (const auto &[name, count] : object_name_counts) {
        if (count > 1) {
            throw std::runtime_error("duplicate object name '" + name + "'" + sceneContext(scene_id));
        }
    }

    std::unordered_map<std::string, size_t> object_index_by_name;
    for (size_t i = 0; i < object_names.size(); ++i) {
        if (!object_names[i].empty()) {
            object_index_by_name.emplace(object_names[i], i);
        }
    }

    // Edges point from child to parent. A three-state DFS reports the exact
    // participating names instead of only saying that some cycle exists.
    std::vector<uint8_t> state(object_names.size(), 0);
    std::vector<size_t> stack;
    const auto visit = [&](auto &&self, size_t object_index) -> void {
        state[object_index] = 1;
        stack.push_back(object_index);
        const auto &parent_name = parent_names[object_index];
        if (!parent_name.empty()) {
            const auto parent_index = object_index_by_name.at(parent_name);
            if (state[parent_index] == 0) {
                self(self, parent_index);
            } else if (state[parent_index] == 1) {
                const auto cycle_begin = std::find(stack.begin(), stack.end(), parent_index);
                std::string cycle;
                for (auto it = cycle_begin; it != stack.end(); ++it) {
                    if (!cycle.empty()) {
                        cycle += " -> ";
                    }
                    cycle += object_names[*it];
                }
                cycle += " -> " + object_names[parent_index];
                throw std::runtime_error("parent cycle in scene '" + scene_id + "': " + cycle);
            }
        }
        stack.pop_back();
        state[object_index] = 2;
    };
    for (size_t i = 0; i < object_names.size(); ++i) {
        if (state[i] == 0) {
            visit(visit, i);
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
    validateEnvelope(scene_data);
    const auto &scenes = scene_data.at("scenes");

    document.scenes = nlohmann::json::object();
    for (auto it = scenes.begin(); it != scenes.end(); ++it) {
        const auto scene_id = it.key();
        requireIdentifier(scene_id, "scene id", "");
        document.scenes[scene_id] = normalizeScene(it.value(), scene_id);
    }
    return document;
}

std::string runtimeObjectIdentityName(std::string_view scene_id,
                                      std::uint64_t object_number,
                                      std::string_view authored_name) {
    if (!authored_name.empty()) {
        return std::string{authored_name};
    }
    if (scene_id.empty() || object_number == 0) {
        throw std::invalid_argument(
            "runtime object identity requires a scene and non-zero object number");
    }
    return "pelican://scene/" + std::string{scene_id} +
           "/authoring-object/" + std::to_string(object_number);
}

} // namespace Pelican
