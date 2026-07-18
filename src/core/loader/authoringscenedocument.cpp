#include "authoringscenedocument.hpp"

#include "../../project/sceneformat.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

AuthoringSceneDocument AuthoringSceneDocument::load(std::string_view scene_v1_bytes, SceneRevision revision,
                                                    std::uint64_t first_authoring_object_id) {
    if (revision.value == 0) {
        throw std::invalid_argument("AuthoringSceneDocument revision must be non-zero");
    }
    if (first_authoring_object_id == 0) {
        throw std::invalid_argument("AuthoringObjectId allocation must start at a non-zero value");
    }

    AuthoringSceneDocument document;
    document.revision_ = revision;
    document.raw_document_ = nlohmann::json::parse(scene_v1_bytes);

    // Validation deliberately runs before identity allocation. The raw tree is
    // retained as the authoring authority; normalizeSceneDataJson's scene copy
    // is not cached a second time.
    auto validated = normalizeSceneDataJson(document.raw_document_);
    document.warnings_ = std::move(validated.warnings);

    std::uint64_t next_object_id = first_authoring_object_id;
    const auto &scenes = document.raw_document_.at("scenes");
    document.scene_metadata_.reserve(scenes.size());
    for (auto scene_it = scenes.begin(); scene_it != scenes.end(); ++scene_it) {
        SceneMetadata scene_metadata;
        scene_metadata.id = scene_it.key();
        const auto &objects = scene_it.value().at("objects");
        scene_metadata.objects.reserve(objects.size());
        for (std::size_t object_index = 0; object_index < objects.size(); ++object_index) {
            if (next_object_id == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("AuthoringObjectId space exhausted");
            }
            scene_metadata.objects.push_back(ObjectMetadata{AuthoringObjectId{next_object_id}});
            ++next_object_id;
        }
        document.scene_metadata_.push_back(std::move(scene_metadata));
    }
    document.next_authoring_object_id_value_ = next_object_id;
    return document;
}

std::size_t AuthoringSceneDocument::objectCount() const noexcept {
    std::size_t count = 0;
    for (const auto &scene : scene_metadata_) {
        count += scene.objects.size();
    }
    return count;
}

std::vector<AuthoringSceneView> AuthoringSceneDocument::query() const {
    std::vector<AuthoringSceneView> result;
    result.reserve(scene_metadata_.size());
    const auto &scenes = raw_document_.at("scenes");
    for (const auto &scene_metadata : scene_metadata_) {
        const auto &scene_json = scenes.at(scene_metadata.id);
        const auto &objects_json = scene_json.at("objects");

        AuthoringSceneView scene_view;
        scene_view.scene_id = scene_metadata.id;
        scene_view.authored_json = &scene_json;
        scene_view.objects.reserve(scene_metadata.objects.size());
        for (std::size_t object_index = 0; object_index < scene_metadata.objects.size(); ++object_index) {
            const auto &object_json = objects_json.at(object_index);
            AuthoringObjectView object_view;
            object_view.authoring_object_id = scene_metadata.objects[object_index].id;
            object_view.declaration_index = object_index;
            if (const auto name = object_json.find("name"); name != object_json.end()) {
                object_view.name = name->get<std::string>();
            }
            if (const auto parent = object_json.find("parent"); parent != object_json.end()) {
                object_view.parent = parent->get<std::string>();
            }
            object_view.authored_json = &object_json;

            const auto &components_json = object_json.at("components");
            object_view.components.reserve(components_json.size());
            for (std::size_t component_index = 0; component_index < components_json.size(); ++component_index) {
                object_view.components.push_back(AuthoringComponentView{
                    .declaration_index = component_index,
                    .authored_json = &components_json.at(component_index),
                    .codec = componentCodecQueryMetadata(
                        components_json.at(component_index).at("name").get_ref<const std::string &>()),
                });
            }
            scene_view.objects.push_back(std::move(object_view));
        }
        result.push_back(std::move(scene_view));
    }
    return result;
}

std::string AuthoringSceneDocument::encodeSemantic() const {
    // nlohmann::json's default object type has a stable lexicographic key
    // order. Array declaration order and JSON scalar types remain untouched.
    return raw_document_.dump();
}

} // namespace Pelican
