#include "project.hpp"

#include "projectformat.hpp"
#include "projectpathresolver.hpp"
#include "sceneformat.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
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

std::string sceneDataReference(const Pelican::ProjectEnvelope &project) {
    if (!project.basic_config.is_object()) {
        throw std::runtime_error("project.json basic_config must be an object");
    }
    const auto found = project.basic_config.find("scene_data_json");
    if (found == project.basic_config.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        throw std::runtime_error(
            "project.json requires string basic_config.scene_data_json");
    }
    return found->get<std::string>();
}

std::vector<OutlinerScene>
buildScenes(const Pelican::SceneFormatDocument &document) {
    std::vector<OutlinerScene> result;
    result.reserve(document.scenes.size());

    for (const auto &[scene_id, authored_scene] : document.scenes.items()) {
        const auto &authored_objects = authored_scene.at("objects");
        OutlinerScene scene;
        scene.scene_id = scene_id;
        scene.objects.reserve(authored_objects.size());
        scene.root_declaration_indices.reserve(authored_objects.size());

        std::unordered_map<std::string, std::size_t> named_object_indices;
        named_object_indices.reserve(authored_objects.size());
        for (std::size_t index = 0; index < authored_objects.size(); ++index) {
            const auto name = authored_objects.at(index).find("name");
            if (name != authored_objects.at(index).end()) {
                named_object_indices.emplace(name->get<std::string>(), index);
            }
        }

        for (std::size_t index = 0; index < authored_objects.size(); ++index) {
            if (index == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error(
                    "scene declaration index exceeds identity number space");
            }
            const auto &authored_object = authored_objects.at(index);
            const auto name = authored_object.find("name");
            const std::string authored_name =
                name == authored_object.end() ? std::string{}
                                              : name->get<std::string>();

            OutlinerObject object{
                .key = {scene_id, index},
                .display_name = Pelican::runtimeObjectIdentityName(
                    scene_id, static_cast<std::uint64_t>(index) + 1,
                    authored_name),
            };
            if (const auto parent = authored_object.find("parent");
                parent != authored_object.end()) {
                object.parent_declaration_index =
                    named_object_indices.at(parent->get<std::string>());
            }
            scene.objects.push_back(std::move(object));
        }

        for (std::size_t index = 0; index < scene.objects.size(); ++index) {
            const auto parent = scene.objects[index].parent_declaration_index;
            if (parent) {
                scene.objects[*parent].child_declaration_indices.push_back(index);
            } else {
                scene.root_declaration_indices.push_back(index);
            }
        }
        result.push_back(std::move(scene));
    }
    return result;
}

} // namespace

ProjectOutlinerModel
ProjectOutlinerModel::open(const std::filesystem::path &project_path) {
    const auto location = locateProject(project_path);
    auto parsed_project = Pelican::parseProjectEnvelopeText(
        readTextFile(location.file, "project.json"));

    Pelican::ProjectPathResolver resolver;
    resolver.setup(location.root, false, parsed_project.envelope);
    const auto scene_text =
        resolver.loadText(sceneDataReference(parsed_project.envelope));
    auto scene_document = Pelican::normalizeSceneDataJson(
        nlohmann::json::parse(scene_text));

    ProjectOutlinerModel model;
    model.project_root_ = resolver.projectRoot();
    model.project_name_ = std::move(parsed_project.envelope.name);
    model.scenes_ = buildScenes(scene_document);
    model.warnings_ = std::move(parsed_project.warnings);
    model.warnings_.insert(model.warnings_.end(),
                           std::make_move_iterator(scene_document.warnings.begin()),
                           std::make_move_iterator(scene_document.warnings.end()));
    return model;
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
