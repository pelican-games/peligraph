#include "authoringscenedocument.hpp"

#include "../../project/sceneformat.hpp"

#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace Pelican {

std::string runtimeObjectIdentityName(std::string_view scene_id,
                                      AuthoringObjectId object_id,
                                      std::string_view authored_name) {
    if (!authored_name.empty()) return std::string{authored_name};
    if (scene_id.empty() || object_id.value == 0) {
        throw std::invalid_argument(
            "runtime object identity requires a scene and authoring object id");
    }
    return "pelican://scene/" + std::string{scene_id} +
           "/authoring-object/" + std::to_string(object_id.value);
}

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

AuthoringSceneDocument AuthoringSceneDocument::stage(
    nlohmann::json raw_document, SceneRevision revision) const {
    if (revision.value == 0 || revision.value <= revision_.value) {
        throw std::invalid_argument(
            "staged AuthoringSceneDocument revision must increase");
    }

    auto validated = normalizeSceneDataJson(raw_document);
    const auto &base_scenes = raw_document_.at("scenes");
    const auto &next_scenes = raw_document.at("scenes");
    if (next_scenes.size() != scene_metadata_.size()) {
        throw std::invalid_argument(
            "projection transaction cannot add or remove scenes");
    }
    for (const auto &scene : scene_metadata_) {
        const auto found = next_scenes.find(scene.id);
        if (found == next_scenes.end() ||
            found->at("objects").size() != scene.objects.size()) {
            throw std::invalid_argument(
                "projection transaction cannot add, remove, or reorder authoring objects");
        }
        const auto &base_objects = base_scenes.at(scene.id).at("objects");
        const auto &next_objects = found->at("objects");
        for (std::size_t index = 0; index < base_objects.size(); ++index) {
            const auto base_name = base_objects[index].find("name");
            const auto next_name = next_objects[index].find("name");
            if ((base_name == base_objects[index].end()) !=
                    (next_name == next_objects[index].end()) ||
                (base_name != base_objects[index].end() &&
                 *base_name != *next_name)) {
                throw std::invalid_argument(
                    "projection transaction cannot add, remove, rename, or reorder authoring objects");
            }
        }
    }

    auto result = *this;
    result.revision_ = revision;
    result.raw_document_ = std::move(raw_document);
    result.warnings_ = std::move(validated.warnings);
    return result;
}

AuthoringSceneDocumentStage AuthoringSceneDocument::structuralStage() const {
    return AuthoringSceneDocumentStage{*this};
}

AuthoringSceneDocumentStage::AuthoringSceneDocumentStage(
    const AuthoringSceneDocument &base)
    : base_(base) {}

std::pair<std::size_t, std::size_t>
AuthoringSceneDocumentStage::requireObjectLocation(
    AuthoringObjectId object_id) const {
    if (object_id.value == 0) {
        throw std::invalid_argument("AuthoringObjectId must be non-zero");
    }
    for (std::size_t scene_index = 0;
         scene_index < base_.scene_metadata_.size(); ++scene_index) {
        const auto &objects = base_.scene_metadata_[scene_index].objects;
        for (std::size_t object_index = 0; object_index < objects.size();
             ++object_index) {
            if (objects[object_index].id == object_id) {
                return {scene_index, object_index};
            }
        }
    }
    throw std::invalid_argument("authoring object does not exist: " +
                                std::to_string(object_id.value));
}

std::size_t AuthoringSceneDocumentStage::requireSceneIndex(
    std::string_view scene_id) const {
    for (std::size_t index = 0; index < base_.scene_metadata_.size(); ++index) {
        if (base_.scene_metadata_[index].id == scene_id) return index;
    }
    throw std::invalid_argument("authoring scene does not exist: " +
                                std::string{scene_id});
}

bool AuthoringSceneDocumentStage::containsObjectId(
    AuthoringObjectId object_id) const noexcept {
    for (const auto &scene : base_.scene_metadata_) {
        for (const auto &object : scene.objects) {
            if (object.id == object_id) return true;
        }
    }
    return false;
}

std::optional<std::size_t> AuthoringSceneDocumentStage::findObjectIndex(
    std::size_t scene_index, AuthoringObjectId object_id) const noexcept {
    const auto &scene = base_.scene_metadata_[scene_index];
    for (std::size_t index = 0; index < scene.objects.size(); ++index) {
        if (scene.objects[index].id == object_id) return index;
    }
    return std::nullopt;
}

AuthoringObjectId AuthoringSceneDocumentStage::insertObject(
    std::string_view scene_id, std::size_t declaration_index,
    nlohmann::json authored_object) {
    const auto scene_index = requireSceneIndex(scene_id);
    auto &metadata = base_.scene_metadata_[scene_index];
    auto &objects = base_.raw_document_.at("scenes")
                        .at(metadata.id)
                        .at("objects");
    if (declaration_index > metadata.objects.size()) {
        throw std::out_of_range("authoring object insertion index is out of range");
    }
    if (base_.next_authoring_object_id_value_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("AuthoringObjectId space exhausted");
    }
    const AuthoringObjectId id{base_.next_authoring_object_id_value_++};
    objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(declaration_index),
                   std::move(authored_object));
    metadata.objects.insert(
        metadata.objects.begin() + static_cast<std::ptrdiff_t>(declaration_index),
        AuthoringSceneDocument::ObjectMetadata{id});
    changes_.push_back({
        .kind = AuthoringStructuralChangeKind::Insert,
        .authoring_object_id = id,
        .scene_id = metadata.id,
        .previous_declaration_index = declaration_index,
        .declaration_index = declaration_index,
    });
    return id;
}

AuthoringObjectClosure AuthoringSceneDocumentStage::removeObject(
    AuthoringObjectId object_id) {
    const auto [scene_index, object_index] = requireObjectLocation(object_id);
    auto &metadata = base_.scene_metadata_[scene_index];
    auto &objects = base_.raw_document_.at("scenes")
                        .at(metadata.id)
                        .at("objects");
    AuthoringObjectClosure closure{
        .authoring_object_id = object_id,
        .scene_id = metadata.id,
        .declaration_index = object_index,
        .previous_object_id = object_index == 0
                                  ? std::nullopt
                                  : std::optional{metadata.objects[object_index - 1].id},
        .next_object_id = object_index + 1 == metadata.objects.size()
                              ? std::nullopt
                              : std::optional{metadata.objects[object_index + 1].id},
        .authored_json = objects.at(object_index),
    };
    objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(object_index));
    metadata.objects.erase(metadata.objects.begin() +
                           static_cast<std::ptrdiff_t>(object_index));
    changes_.push_back({
        .kind = AuthoringStructuralChangeKind::Remove,
        .authoring_object_id = object_id,
        .scene_id = metadata.id,
        .previous_declaration_index = object_index,
        .declaration_index = object_index,
    });
    removed_objects_.push_back(closure);
    return closure;
}

void AuthoringSceneDocumentStage::restoreObject(
    const AuthoringObjectClosure &closure) {
    if (closure.authoring_object_id.value == 0) {
        throw std::invalid_argument("destroy closure has an invalid AuthoringObjectId");
    }
    if (containsObjectId(closure.authoring_object_id)) {
        throw std::invalid_argument("destroy closure AuthoringObjectId is already live");
    }
    const auto scene_index = requireSceneIndex(closure.scene_id);
    auto &metadata = base_.scene_metadata_[scene_index];
    auto &objects = base_.raw_document_.at("scenes")
                        .at(metadata.id)
                        .at("objects");
    const auto previous = closure.previous_object_id
                              ? findObjectIndex(scene_index, *closure.previous_object_id)
                              : std::nullopt;
    const auto next = closure.next_object_id
                          ? findObjectIndex(scene_index, *closure.next_object_id)
                          : std::nullopt;
    if (previous && next && *previous >= *next) {
        throw std::invalid_argument(
            "destroy closure declaration interval has been inverted");
    }
    std::size_t index = std::min(closure.declaration_index,
                                 metadata.objects.size());
    if (next) {
        index = *next;
    } else if (previous) {
        index = *previous + 1;
    }
    objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(index),
                   closure.authored_json);
    metadata.objects.insert(
        metadata.objects.begin() + static_cast<std::ptrdiff_t>(index),
        AuthoringSceneDocument::ObjectMetadata{closure.authoring_object_id});
    changes_.push_back({
        .kind = AuthoringStructuralChangeKind::Restore,
        .authoring_object_id = closure.authoring_object_id,
        .scene_id = metadata.id,
        .previous_declaration_index = closure.declaration_index,
        .declaration_index = index,
    });
}

void AuthoringSceneDocumentStage::renameObject(
    AuthoringObjectId object_id, std::optional<std::string> name) {
    const auto [scene_index, object_index] = requireObjectLocation(object_id);
    auto &metadata = base_.scene_metadata_[scene_index];
    auto &object = base_.raw_document_.at("scenes")
                       .at(metadata.id)
                       .at("objects")
                       .at(object_index);
    if (name) {
        object["name"] = std::move(*name);
    } else {
        object.erase("name");
    }
    changes_.push_back({
        .kind = AuthoringStructuralChangeKind::Rename,
        .authoring_object_id = object_id,
        .scene_id = metadata.id,
        .previous_declaration_index = object_index,
        .declaration_index = object_index,
    });
}

void AuthoringSceneDocumentStage::reorderObject(
    AuthoringObjectId object_id, std::size_t declaration_index) {
    const auto [scene_index, object_index] = requireObjectLocation(object_id);
    auto &metadata = base_.scene_metadata_[scene_index];
    if (declaration_index >= metadata.objects.size()) {
        throw std::out_of_range("authoring object reorder index is out of range");
    }
    if (declaration_index == object_index) return;
    auto &objects = base_.raw_document_.at("scenes")
                        .at(metadata.id)
                        .at("objects");
    auto object_json = std::move(objects.at(object_index));
    const auto object_metadata = metadata.objects[object_index];
    objects.erase(objects.begin() + static_cast<std::ptrdiff_t>(object_index));
    metadata.objects.erase(metadata.objects.begin() +
                           static_cast<std::ptrdiff_t>(object_index));
    objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(declaration_index),
                   std::move(object_json));
    metadata.objects.insert(
        metadata.objects.begin() + static_cast<std::ptrdiff_t>(declaration_index),
        object_metadata);
    changes_.push_back({
        .kind = AuthoringStructuralChangeKind::Reorder,
        .authoring_object_id = object_id,
        .scene_id = metadata.id,
        .previous_declaration_index = object_index,
        .declaration_index = declaration_index,
    });
}

AuthoringSceneDocument AuthoringSceneDocumentStage::finish(
    SceneRevision revision) && {
    if (revision.value == 0 || revision.value <= base_.revision_.value) {
        throw std::invalid_argument(
            "staged AuthoringSceneDocument revision must increase");
    }
    auto validated = normalizeSceneDataJson(base_.raw_document_);
    const auto &scenes = base_.raw_document_.at("scenes");
    if (scenes.size() != base_.scene_metadata_.size()) {
        throw std::invalid_argument(
            "projection transaction cannot add or remove scenes");
    }
    for (const auto &scene : base_.scene_metadata_) {
        const auto found = scenes.find(scene.id);
        if (found == scenes.end() ||
            found->at("objects").size() != scene.objects.size()) {
            throw std::logic_error(
                "authoring structural stage metadata is out of sync");
        }
    }
    base_.revision_ = revision;
    base_.warnings_ = std::move(validated.warnings);
    return std::move(base_);
}

void AuthoringSceneDocument::swap(AuthoringSceneDocument &other) noexcept {
    using std::swap;
    swap(revision_, other.revision_);
    raw_document_.swap(other.raw_document_);
    warnings_.swap(other.warnings_);
    scene_metadata_.swap(other.scene_metadata_);
    swap(next_authoring_object_id_value_, other.next_authoring_object_id_value_);
}

} // namespace Pelican
