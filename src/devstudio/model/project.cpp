#include "project.hpp"

#include "projectformat.hpp"
#include "sceneformat.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace PelicanStudio {
namespace {

struct ProjectLocation {
    std::filesystem::path root;
    std::filesystem::path file;
};

ProjectLocation locateProject(const std::filesystem::path &project_path) {
    if (project_path.empty()) {
        throw std::invalid_argument("project path must not be empty");
    }

    std::error_code error;
    if (std::filesystem::is_directory(project_path, error) && !error) {
        const auto root = std::filesystem::canonical(project_path);
        const auto file = root / "project.json";
        if (!std::filesystem::is_regular_file(file, error) || error) {
            throw std::runtime_error("project directory has no project.json: " +
                                     root.string());
        }
        return {root, file};
    }

    error.clear();
    if (std::filesystem::is_regular_file(project_path, error) && !error) {
        if (project_path.filename() != "project.json") {
            throw std::runtime_error(
                "project file must be named project.json: " +
                project_path.string());
        }
        const auto file = std::filesystem::canonical(project_path);
        return {file.parent_path(), file};
    }

    throw std::runtime_error(
        "project path must name a directory or project.json file: " +
        project_path.string());
}

std::string readTextFile(const std::filesystem::path &path,
                         std::string_view label) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("could not open " + std::string{label} +
                                 ": " + path.string());
    }
    std::string contents{std::istreambuf_iterator<char>{stream},
                         std::istreambuf_iterator<char>{}};
    if (stream.bad()) {
        throw std::runtime_error("could not read " + std::string{label} +
                                 ": " + path.string());
    }
    return contents;
}

std::uint64_t exactUnsigned(const nlohmann::json &value,
                            std::string_view field, bool nonzero = false) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(std::string{field} +
                                     " must be non-negative");
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        throw std::runtime_error(std::string{field} +
                                 " must be an integer token");
    }
    if (nonzero && result == 0) {
        throw std::runtime_error(std::string{field} + " must be non-zero");
    }
    return result;
}

const std::string &requiredNonemptyString(const nlohmann::json &object,
                                          std::string_view field) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        throw std::runtime_error("scene_tree requires non-empty string " +
                                 std::string{field});
    }
    return found->get_ref<const std::string &>();
}

} // namespace

ProjectOutlinerModel
ProjectOutlinerModel::open(const std::filesystem::path &project_path) {
    const auto location = locateProject(project_path);
    auto parsed_project = Pelican::parseProjectEnvelopeText(
        readTextFile(location.file, "project.json"));

    ProjectOutlinerModel model;
    model.project_root_ = location.root;
    model.project_name_ = std::move(parsed_project.envelope.name);
    model.warnings_ = std::move(parsed_project.warnings);
    return model;
}

bool ProjectOutlinerModel::updateSceneTree(
    const nlohmann::json &scene_tree) {
    if (!scene_tree.is_object()) {
        throw std::runtime_error("scene_tree result must be an object");
    }
    const auto revision_value = scene_tree.find("scene_revision");
    if (revision_value == scene_tree.end()) {
        throw std::runtime_error("scene_tree requires scene_revision");
    }
    const auto revision = exactUnsigned(*revision_value, "scene_revision",
                                        true);
    const auto scene_id = requiredNonemptyString(scene_tree, "scene_id");
    const auto objects_value = scene_tree.find("objects");
    if (objects_value == scene_tree.end() || !objects_value->is_array()) {
        throw std::runtime_error("scene_tree requires objects array");
    }

    if (scene_revision_ && *scene_revision_ == revision &&
        std::any_of(scenes_.begin(), scenes_.end(),
                    [&scene_id](const auto &scene) {
                        return scene.scene_id == scene_id;
                    })) {
        return false;
    }

    struct RpcObject {
        std::size_t declaration_index = 0;
        std::uint64_t authoring_object_id = 0;
        std::string name;
        std::optional<std::string> parent;
    };
    std::vector<std::optional<RpcObject>> ordered(objects_value->size());
    std::unordered_map<std::string, std::size_t> named_indices;
    for (const auto &value : *objects_value) {
        if (!value.is_object()) {
            throw std::runtime_error("scene_tree object entry must be an object");
        }
        const auto index_value = value.find("declaration_index");
        const auto id_value = value.find("authoring_object_id");
        if (index_value == value.end() || id_value == value.end()) {
            throw std::runtime_error(
                "scene_tree object requires declaration_index and authoring_object_id");
        }
        const auto index64 = exactUnsigned(*index_value,
                                           "declaration_index");
        if (index64 >= ordered.size()) {
            throw std::runtime_error(
                "scene_tree declaration indices must be contiguous");
        }
        const auto index = static_cast<std::size_t>(index64);
        if (ordered[index]) {
            throw std::runtime_error(
                "scene_tree declaration_index is duplicated");
        }
        RpcObject object{
            .declaration_index = index,
            .authoring_object_id = exactUnsigned(
                *id_value, "authoring_object_id", true),
        };
        if (const auto name = value.find("name"); name != value.end()) {
            if (!name->is_string() || name->get_ref<const std::string &>().empty()) {
                throw std::runtime_error(
                    "scene_tree object name must be a non-empty string");
            }
            object.name = name->get<std::string>();
            if (!named_indices.emplace(object.name, index).second) {
                throw std::runtime_error(
                    "scene_tree object name is duplicated");
            }
        }
        if (const auto parent = value.find("parent"); parent != value.end()) {
            if (!parent->is_string() ||
                parent->get_ref<const std::string &>().empty()) {
                throw std::runtime_error(
                    "scene_tree object parent must be a non-empty string");
            }
            object.parent = parent->get<std::string>();
        }
        ordered[index] = std::move(object);
    }

    OutlinerScene projected{.scene_id = scene_id};
    projected.objects.reserve(ordered.size());
    projected.root_declaration_indices.reserve(ordered.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        if (!ordered[index]) {
            throw std::runtime_error(
                "scene_tree declaration indices must be contiguous");
        }
        const auto &source = *ordered[index];
        OutlinerObject object{
            .key = {scene_id, index},
            .display_name = Pelican::runtimeObjectIdentityName(
                scene_id, source.authoring_object_id, source.name),
        };
        if (source.parent) {
            const auto parent = named_indices.find(*source.parent);
            if (parent == named_indices.end()) {
                throw std::runtime_error(
                    "scene_tree parent does not name an object");
            }
            object.parent_declaration_index = parent->second;
        }
        projected.objects.push_back(std::move(object));
    }
    for (std::size_t index = 0; index < projected.objects.size(); ++index) {
        const auto parent =
            projected.objects[index].parent_declaration_index;
        if (parent) {
            projected.objects[*parent].child_declaration_indices.push_back(index);
        } else {
            projected.root_declaration_indices.push_back(index);
        }
    }

    if (!scene_revision_ || *scene_revision_ != revision) {
        scenes_.clear();
        scene_revision_ = revision;
    }
    const auto existing = std::find_if(
        scenes_.begin(), scenes_.end(), [&scene_id](const auto &scene) {
            return scene.scene_id == scene_id;
        });
    if (existing == scenes_.end()) {
        scenes_.push_back(std::move(projected));
    } else {
        *existing = std::move(projected);
    }
    return true;
}

const OutlinerObject *
ProjectOutlinerModel::findObject(const OutlinerObjectKey &key) const noexcept {
    for (const auto &scene : scenes_) {
        if (scene.scene_id != key.scene_id ||
            key.declaration_index >= scene.objects.size()) {
            continue;
        }
        const auto &object = scene.objects[key.declaration_index];
        if (object.key == key) {
            return &object;
        }
    }
    return nullptr;
}

} // namespace PelicanStudio
