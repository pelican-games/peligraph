#include "editorprojectiontransaction.hpp"

#include "componentcodec.hpp"
#include "basicconfig.hpp"
#include "../../project/sceneformat.hpp"
#include "../geomhelper/geomhelper.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

using Json = nlohmann::json;

std::string objectPath(std::string_view scene_id, std::string_view object_name) {
    return "/scenes/" + std::string{scene_id} + "/objects/" +
           std::string{object_name};
}

std::string objectPath(std::string_view scene_id,
                       const AuthoringObjectView &object) {
    if (object.name) return objectPath(scene_id, *object.name);
    return "/scenes/" + std::string{scene_id} + "/authoring_objects/" +
           std::to_string(object.authoring_object_id.value);
}

[[noreturn]] void projectionError(EditorProjectionErrorCode code,
                                  std::string path,
                                  std::string message) {
    throw EditorProjectionException{code, std::move(path), std::move(message)};
}

Json &requireScene(Json &document, std::string_view scene_id,
                   std::string_view path) {
    auto &scenes = document.at("scenes");
    const auto found = scenes.find(std::string{scene_id});
    if (found == scenes.end()) {
        projectionError(EditorProjectionErrorCode::ObjectNotFound,
                        std::string{path},
                        "authoring scene does not exist: " +
                            std::string{scene_id});
    }
    return *found;
}

const Json &requireScene(const Json &document, std::string_view scene_id,
                         std::string_view path) {
    const auto &scenes = document.at("scenes");
    const auto found = scenes.find(std::string{scene_id});
    if (found == scenes.end()) {
        projectionError(EditorProjectionErrorCode::ObjectNotFound,
                        std::string{path},
                        "authoring scene does not exist: " +
                            std::string{scene_id});
    }
    return *found;
}

Json *findObject(Json &scene, std::string_view name) {
    for (auto &object : scene.at("objects")) {
        if (object.value("name", std::string{}) == name) return &object;
    }
    return nullptr;
}

const Json *findObject(const Json &scene, std::string_view name) {
    for (const auto &object : scene.at("objects")) {
        if (object.value("name", std::string{}) == name) return &object;
    }
    return nullptr;
}

Json *findComponent(Json &object, std::string_view name) {
    for (auto &component : object.at("components")) {
        if (component.value("name", std::string{}) == name) return &component;
    }
    return nullptr;
}

const Json *findComponent(const Json &object, std::string_view name) {
    for (const auto &component : object.at("components")) {
        if (component.value("name", std::string{}) == name) return &component;
    }
    return nullptr;
}

TransformCodecData decodeTransformObject(const Json &object,
                                         std::string_view path) {
    const auto *component = findComponent(object, "transform");
    if (component == nullptr) {
        projectionError(EditorProjectionErrorCode::ComponentNotFound,
                        std::string{path} + "/components/transform",
                        "hierarchy object has no transform component");
    }
    try {
        return std::any_cast<TransformCodecData>(
            requireComponentCodec("transform").decodeAuthored(*component));
    } catch (const EditorProjectionException &) {
        throw;
    } catch (const std::exception &error) {
        projectionError(EditorProjectionErrorCode::TransformNonFinite,
                        std::string{path} + "/components/transform",
                        error.what());
    }
}

bool finite(glm::vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(glm::quat value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

bool finite(const TransformCodecData &value) noexcept {
    return std::isfinite(value.pos.x) && std::isfinite(value.pos.y) &&
           std::isfinite(value.pos.z) && std::isfinite(value.rotation.x) &&
           std::isfinite(value.rotation.y) &&
           std::isfinite(value.rotation.z) &&
           std::isfinite(value.rotation.w) &&
           std::isfinite(value.scale.x) && std::isfinite(value.scale.y) &&
           std::isfinite(value.scale.z);
}

TransformComponent composeWorld(const TransformComponent *parent,
                                const TransformCodecData &local,
                                std::string_view path) {
    glm::vec3 parent_pos{0.0F};
    glm::quat parent_rotation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 parent_scale{1.0F};
    if (parent != nullptr) {
        parent_pos = parent->pos;
        parent_rotation = parent->rotation;
        parent_scale = parent->scale;
    }
    const TransformComponent result{
        .pos = parent_pos +
               parent_rotation * (parent_scale * to_glm(local.pos)),
        .rotation = parent_rotation * to_glm(local.rotation),
        .scale = parent_scale * to_glm(local.scale),
    };
    if (!finite(result.pos) || !finite(result.rotation) ||
        !finite(result.scale)) {
        projectionError(EditorProjectionErrorCode::TransformNonFinite,
                        std::string{path},
                        "transform composition produced a non-finite value");
    }
    return result;
}

struct DocumentTransformNode {
    AuthoringObjectId authoring_object_id{};
    std::string name;
    std::string parent;
    std::string path;
    TransformCodecData local;
    TransformComponent world{};
    std::uint8_t state = 0;
};

using NodeIndex = std::unordered_map<std::string, std::size_t>;

void projectNode(std::size_t index, std::vector<DocumentTransformNode> &nodes,
                 const NodeIndex &indices) {
    auto &node = nodes[index];
    if (node.state == 2) return;
    if (node.state == 1) {
        projectionError(EditorProjectionErrorCode::TransformCycle, node.path,
                        "reparent would create a parent cycle");
    }
    node.state = 1;
    const TransformComponent *parent_world = nullptr;
    if (!node.parent.empty()) {
        const auto parent = indices.find(node.parent);
        if (parent == indices.end()) {
            projectionError(EditorProjectionErrorCode::ObjectNotFound,
                            node.path,
                            "transform parent does not exist: " + node.parent);
        }
        projectNode(parent->second, nodes, indices);
        parent_world = &nodes[parent->second].world;
    }
    node.world = composeWorld(parent_world, node.local, node.path);
    node.state = 2;
}

std::pair<std::vector<DocumentTransformNode>, NodeIndex>
buildTransformNodes(const Json &scene, std::string_view scene_id) {
    std::vector<DocumentTransformNode> nodes;
    NodeIndex indices;
    nodes.reserve(scene.at("objects").size());
    indices.reserve(scene.at("objects").size());
    for (const auto &object : scene.at("objects")) {
        if (findComponent(object, "transform") == nullptr) continue;
        const auto name = object.value("name", std::string{});
        if (name.empty()) continue;
        const auto path = objectPath(scene_id, name);
        const auto [it, inserted] = indices.emplace(name, nodes.size());
        if (!inserted) {
            projectionError(EditorProjectionErrorCode::CommandInvalid, path,
                            "duplicate authoring object name");
        }
        nodes.push_back(DocumentTransformNode{
            .name = name,
            .parent = object.value("parent", std::string{}),
            .path = path,
            .local = decodeTransformObject(object, path),
        });
    }
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        projectNode(index, nodes, indices);
    }
    return {std::move(nodes), std::move(indices)};
}

struct AuthoringTransformNodes {
    std::vector<DocumentTransformNode> nodes;
    NodeIndex by_name;
    std::unordered_map<std::uint64_t, std::size_t> by_id;
};

AuthoringTransformNodes buildAuthoringTransformNodes(
    const AuthoringSceneView &scene) {
    AuthoringTransformNodes result;
    result.nodes.reserve(scene.objects.size());
    result.by_name.reserve(scene.objects.size());
    result.by_id.reserve(scene.objects.size());
    for (const auto &object : scene.objects) {
        if (findComponent(object.authoredJson(), "transform") != nullptr) {
            const auto path = objectPath(scene.scene_id, object);
            const auto index = result.nodes.size();
            if (!result.by_id
                     .emplace(object.authoring_object_id.value, index)
                     .second) {
                projectionError(EditorProjectionErrorCode::CommandInvalid,
                                path,
                                "duplicate authoring object identity");
            }
            const auto name = object.name.value_or(std::string{});
            if (!name.empty() && !result.by_name.emplace(name, index).second) {
                projectionError(EditorProjectionErrorCode::CommandInvalid,
                                path,
                                "duplicate authoring object name");
            }
            result.nodes.push_back(DocumentTransformNode{
                .authoring_object_id = object.authoring_object_id,
                .name = name,
                .parent = object.parent.value_or(std::string{}),
                .path = path,
                .local = decodeTransformObject(object.authoredJson(), path),
            });
        }
    }
    for (std::size_t index = 0; index < result.nodes.size(); ++index) {
        projectNode(index, result.nodes, result.by_name);
    }
    return result;
}

TransformCodecData inverseLocal(const TransformComponent &world,
                                const TransformComponent *parent,
                                std::string_view path) {
    if (parent == nullptr) {
        return TransformCodecData{
            {world.pos.x, world.pos.y, world.pos.z},
            {world.rotation.x, world.rotation.y, world.rotation.z,
             world.rotation.w},
            {world.scale.x, world.scale.y, world.scale.z},
        };
    }
    constexpr float epsilon = std::numeric_limits<float>::epsilon();
    if (std::abs(parent->scale.x) <= epsilon ||
        std::abs(parent->scale.y) <= epsilon ||
        std::abs(parent->scale.z) <= epsilon) {
        projectionError(EditorProjectionErrorCode::TransformZeroParentScale,
                        std::string{path},
                        "preserve-world reparent cannot invert zero parent scale");
    }
    if (!finite(parent->pos) || !finite(parent->rotation) ||
        !finite(parent->scale)) {
        projectionError(EditorProjectionErrorCode::TransformNonFinite,
                        std::string{path},
                        "preserve-world reparent parent transform is non-finite");
    }
    const auto inverse_rotation = glm::inverse(parent->rotation);
    const auto pos =
        (inverse_rotation * (world.pos - parent->pos)) / parent->scale;
    const auto rotation = inverse_rotation * world.rotation;
    const auto scale = world.scale / parent->scale;
    TransformCodecData result{
        {pos.x, pos.y, pos.z},
        {rotation.x, rotation.y, rotation.z, rotation.w},
        {scale.x, scale.y, scale.z},
    };
    if (!finite(result)) {
        projectionError(EditorProjectionErrorCode::TransformNonFinite,
                        std::string{path},
                        "preserve-world reparent produced a non-finite local transform");
    }
    const auto norm_squared = rotation.x * rotation.x +
                              rotation.y * rotation.y +
                              rotation.z * rotation.z +
                              rotation.w * rotation.w;
    if (!std::isfinite(norm_squared) || norm_squared <= epsilon) {
        projectionError(EditorProjectionErrorCode::TransformUnrepresentable,
                        std::string{path},
                        "preserve-world reparent cannot represent a canonical local quaternion");
    }
    const auto recomposed = composeWorld(parent, result, path);
    const auto close = [](float left, float right) {
        const auto scale_value = std::max({1.0F, std::abs(left), std::abs(right)});
        return std::abs(left - right) <= 32.0F *
                                           std::numeric_limits<float>::epsilon() *
                                           scale_value;
    };
    const bool same = close(recomposed.pos.x, world.pos.x) &&
                      close(recomposed.pos.y, world.pos.y) &&
                      close(recomposed.pos.z, world.pos.z) &&
                      close(recomposed.scale.x, world.scale.x) &&
                      close(recomposed.scale.y, world.scale.y) &&
                      close(recomposed.scale.z, world.scale.z) &&
                      close(recomposed.rotation.x, world.rotation.x) &&
                      close(recomposed.rotation.y, world.rotation.y) &&
                      close(recomposed.rotation.z, world.rotation.z) &&
                      close(recomposed.rotation.w, world.rotation.w);
    if (!same) {
        projectionError(EditorProjectionErrorCode::TransformUnrepresentable,
                        std::string{path},
                        "preserve-world reparent is not representable by canonical TRS");
    }
    return result;
}

Json canonicalTransformJson(const TransformCodecData &value) {
    const auto encoded = requireComponentCodec("transform").encodeCanonical(
        ComponentCodecValue{value});
    return Json::parse(encoded.dump());
}

EditorProjectionError errorFromException(EditorProjectionErrorCode fallback,
                                         std::string path,
                                         const std::exception &error) {
    if (const auto *projection =
            dynamic_cast<const EditorProjectionException *>(&error)) {
        return EditorProjectionError{projection->code(),
                                     projection->objectPath(), error.what()};
    }
    return EditorProjectionError{fallback, std::move(path), error.what()};
}

} // namespace

std::string_view editorProjectionAdapterKindName(
    EditorProjectionAdapterKind kind) noexcept {
    switch (kind) {
    case EditorProjectionAdapterKind::EcsExistingValue:
        return "ecs_existing_value";
    case EditorProjectionAdapterKind::EcsArchetype:
        return "ecs_archetype";
    case EditorProjectionAdapterKind::TransformClosure:
        return "transform_closure";
    case EditorProjectionAdapterKind::RendererModel:
        return "renderer_model";
    case EditorProjectionAdapterKind::Camera:
        return "camera";
    case EditorProjectionAdapterKind::Light:
        return "light";
    case EditorProjectionAdapterKind::PhysWorld:
        return "phys_world";
    case EditorProjectionAdapterKind::BehaviorAttachment:
        return "behavior_attachment";
    }
    return "unknown";
}

std::string_view editorProjectionErrorCodeName(
    EditorProjectionErrorCode code) noexcept {
    switch (code) {
    case EditorProjectionErrorCode::StaleRevision:
        return "stale_revision";
    case EditorProjectionErrorCode::CommandInvalid:
        return "projection_command_invalid";
    case EditorProjectionErrorCode::ObjectNotFound:
        return "authoring_object_not_found";
    case EditorProjectionErrorCode::ComponentNotFound:
        return "component_not_found";
    case EditorProjectionErrorCode::CodecMissing:
        return "component_codec_missing";
    case EditorProjectionErrorCode::TransformCycle:
        return "transform_parent_cycle";
    case EditorProjectionErrorCode::TransformZeroParentScale:
        return "transform_zero_parent_scale";
    case EditorProjectionErrorCode::TransformNonFinite:
        return "transform_non_finite";
    case EditorProjectionErrorCode::TransformUnrepresentable:
        return "transform_unrepresentable";
    case EditorProjectionErrorCode::AdapterPrepareFailed:
        return "projection_adapter_prepare_failed";
    case EditorProjectionErrorCode::AdapterPublishFailed:
        return "projection_adapter_publish_failed";
    }
    return "projection_unknown_error";
}

EditorProjectionException::EditorProjectionException(
    EditorProjectionErrorCode code, std::string object_path,
    std::string message)
    : std::runtime_error(std::string{editorProjectionErrorCodeName(code)} +
                         ": " + message),
      code_(code), object_path_(std::move(object_path)) {}

const AuthoringSceneDocument &
ProjectBasicConfigProjectionTarget::projectionDocument() const {
    return config_.sceneDocument();
}

SceneRevision ProjectBasicConfigProjectionTarget::nextProjectionRevision() const {
    (void)config_.sceneDocument();
    if (config_.next_scene_revision ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("SceneRevision space exhausted");
    }
    return SceneRevision{config_.next_scene_revision};
}

void ProjectBasicConfigProjectionTarget::publishProjectionDocument(
    AuthoringSceneDocument &&document) noexcept {
    const auto next_object_id = document.next_authoring_object_id_value_;
    const auto revision = document.revision_.value;
    config_.scene_document->swap(document);
    config_.next_scene_revision = revision + 1;
    config_.next_authoring_object_id = next_object_id;
}

EditorProjectionCommand makeSetComponentValueCommand(
    std::string scene_id, std::string object_name,
    std::string component_name, Json authored_component) {
    const auto path = objectPath(scene_id, object_name) + "/components/" +
                      component_name;
    return EditorProjectionCommand{
        .object_path = path,
        .apply = [scene_id = std::move(scene_id),
                  object_name = std::move(object_name),
                  component_name = std::move(component_name),
                  authored_component = std::move(authored_component),
                  path](Json &document) {
            auto &scene = requireScene(document, scene_id, path);
            auto *object = findObject(scene, object_name);
            if (object == nullptr) {
                projectionError(EditorProjectionErrorCode::ObjectNotFound,
                                path, "authoring object does not exist");
            }
            auto *component = findComponent(*object, component_name);
            if (component == nullptr) {
                projectionError(EditorProjectionErrorCode::ComponentNotFound,
                                path, "component does not exist");
            }
            const auto *codec = findComponentCodec(component_name);
            if (codec == nullptr) {
                projectionError(EditorProjectionErrorCode::CodecMissing, path,
                                "component has no editable codec");
            }
            try {
                const auto decoded = codec->decodeAuthored(authored_component);
                *component = Json::parse(codec->encodeCanonical(decoded).dump());
            } catch (const EditorProjectionException &) {
                throw;
            } catch (const std::exception &error) {
                projectionError(EditorProjectionErrorCode::CommandInvalid,
                                path, error.what());
            }
        },
    };
}

EditorProjectionCommand makeReparentCommand(
    std::string scene_id, std::string object_name,
    std::optional<std::string> new_parent, ReparentPreserve preserve) {
    const auto path = objectPath(scene_id, object_name);
    return EditorProjectionCommand{
        .object_path = path,
        .apply = [scene_id = std::move(scene_id),
                  object_name = std::move(object_name),
                  new_parent = std::move(new_parent), preserve,
                  path](Json &document) {
            auto &scene = requireScene(document, scene_id, path);
            auto *object = findObject(scene, object_name);
            if (object == nullptr) {
                projectionError(EditorProjectionErrorCode::ObjectNotFound,
                                path, "authoring object does not exist");
            }
            if (new_parent && *new_parent == object_name) {
                projectionError(EditorProjectionErrorCode::TransformCycle,
                                path, "an object cannot be its own parent");
            }
            if (new_parent && findObject(scene, *new_parent) == nullptr) {
                projectionError(EditorProjectionErrorCode::ObjectNotFound,
                                path, "new parent does not exist: " +
                                          *new_parent);
            }

            auto [before_nodes, before_indices] =
                buildTransformNodes(scene, scene_id);
            const auto child_before = before_indices.find(object_name);
            if (child_before == before_indices.end()) {
                projectionError(EditorProjectionErrorCode::ComponentNotFound,
                                path + "/components/transform",
                                "reparent requires a transform component");
            }
            const auto old_world = before_nodes[child_before->second].world;

            if (new_parent) {
                (*object)["parent"] = *new_parent;
            } else {
                object->erase("parent");
            }

            // The staged graph itself is the cycle preflight. It also ensures
            // every parent used by a transform has an authored transform.
            auto [after_nodes, after_indices] =
                buildTransformNodes(scene, scene_id);
            const auto child_after = after_indices.at(object_name);
            if (preserve == ReparentPreserve::World) {
                const TransformComponent *parent_world = nullptr;
                if (new_parent) {
                    const auto parent = after_indices.find(*new_parent);
                    if (parent == after_indices.end()) {
                        projectionError(
                            EditorProjectionErrorCode::ComponentNotFound,
                            objectPath(scene_id, *new_parent) +
                                "/components/transform",
                            "new parent has no transform component");
                    }
                    parent_world = &after_nodes[parent->second].world;
                }
                const auto local = inverseLocal(old_world, parent_world, path);
                auto *transform = findComponent(*object, "transform");
                *transform = canonicalTransformJson(local);

                // Re-run the closure after changing local to prove the
                // preserve-world postcondition before any adapter prepare.
                auto [verified_nodes, verified_indices] =
                    buildTransformNodes(scene, scene_id);
                (void)verified_nodes.at(verified_indices.at(object_name));
            } else {
                (void)after_nodes.at(child_after);
            }
        },
    };
}

EditorProjectionCommand makeInsertObjectCommand(
    std::string scene_id, std::size_t declaration_index,
    Json authored_object) {
    const auto path = "/scenes/" + scene_id + "/objects";
    return EditorProjectionCommand{
        .object_path = path,
        .structural_apply =
            [scene_id = std::move(scene_id), declaration_index,
             authored_object = std::move(authored_object)](
                AuthoringSceneDocumentStage &stage) {
                (void)stage.insertObject(scene_id, declaration_index,
                                         authored_object);
            },
    };
}

EditorProjectionCommand makeRemoveObjectCommand(AuthoringObjectId object_id) {
    const auto path = "/authoring_objects/" + std::to_string(object_id.value);
    return EditorProjectionCommand{
        .object_path = path,
        .structural_apply = [object_id](AuthoringSceneDocumentStage &stage) {
            (void)stage.removeObject(object_id);
        },
    };
}

EditorProjectionCommand makeRestoreObjectCommand(AuthoringObjectClosure closure) {
    const auto path = "/authoring_objects/" +
                      std::to_string(closure.authoring_object_id.value);
    return EditorProjectionCommand{
        .object_path = path,
        .structural_apply =
            [closure = std::move(closure)](AuthoringSceneDocumentStage &stage) {
                stage.restoreObject(closure);
            },
    };
}

EditorProjectionCommand makeRenameObjectCommand(
    AuthoringObjectId object_id, std::optional<std::string> name) {
    const auto path = "/authoring_objects/" + std::to_string(object_id.value);
    return EditorProjectionCommand{
        .object_path = path,
        .structural_apply =
            [object_id, name = std::move(name)](
                AuthoringSceneDocumentStage &stage) {
                stage.renameObject(object_id, name);
            },
    };
}

EditorProjectionCommand makeReorderObjectCommand(
    AuthoringObjectId object_id, std::size_t declaration_index) {
    const auto path = "/authoring_objects/" + std::to_string(object_id.value);
    return EditorProjectionCommand{
        .object_path = path,
        .structural_apply = [object_id, declaration_index](
                                AuthoringSceneDocumentStage &stage) {
            stage.reorderObject(object_id, declaration_index);
        },
    };
}

EditorProjectionCallbackAdapter::EditorProjectionCallbackAdapter(
    EditorProjectionAdapterKind kind, std::string name,
    EditorProjectionPublicationMode mode, void *context, Prepare prepare,
    NoexceptAction publish, NoexceptAction rollback,
    NoexceptAction finish) noexcept
    : kind_(kind), name_(std::move(name)), mode_(mode), context_(context),
      prepare_(prepare), publish_(publish), rollback_(rollback), finish_(finish) {}

void EditorProjectionCallbackAdapter::prepare(
    const EditorProjectionPrepareContext &context) {
    if (prepare_ != nullptr) prepare_(context_, context);
}

void EditorProjectionCallbackAdapter::publish() noexcept {
    if (publish_ != nullptr) publish_(context_);
}

void EditorProjectionCallbackAdapter::rollback() noexcept {
    if (rollback_ != nullptr) rollback_(context_);
}

void EditorProjectionCallbackAdapter::finish() noexcept {
    if (finish_ != nullptr) finish_(context_);
}

TransformProjectionAdapter::TransformProjectionAdapter(
    std::vector<TransformProjectionBinding> bindings)
    : bindings_(std::move(bindings)) {
    std::unordered_set<std::string> keys;
    keys.reserve(bindings_.size());
    for (const auto &binding : bindings_) {
        const auto key = binding.scene_id + "\n" +
                         std::to_string(binding.authoring_object_id.value);
        if (binding.scene_id.empty() ||
            binding.authoring_object_id.value == 0 ||
            binding.world == nullptr || binding.local == nullptr) {
            throw std::invalid_argument(
                "transform projection binding is incomplete");
        }
        if (!keys.insert(key).second) {
            throw std::invalid_argument(
                "transform projection binding is duplicated: " + key);
        }
    }
    prepared_.reserve(bindings_.size());
}

void TransformProjectionAdapter::prepare(
    const EditorProjectionPrepareContext &context) {
    prepared_.clear();
    published_ = false;

    const auto document_scenes = context.next_document.query();
    std::unordered_map<std::string, AuthoringTransformNodes> scenes;
    scenes.reserve(bindings_.size());
    for (const auto &binding : bindings_) {
        if (scenes.contains(binding.scene_id)) continue;
        const auto scene = std::find_if(
            document_scenes.begin(), document_scenes.end(),
            [&](const auto &candidate) {
                return candidate.scene_id == binding.scene_id;
            });
        if (scene == document_scenes.end()) {
            projectionError(EditorProjectionErrorCode::ObjectNotFound,
                            "/scenes/" + binding.scene_id,
                            "authoring scene does not exist: " +
                                binding.scene_id);
        }
        scenes.emplace(binding.scene_id,
                       buildAuthoringTransformNodes(*scene));
    }

    std::unordered_map<std::string,
                       std::unordered_map<std::uint64_t, EntityId>>
        entities;
    for (const auto &binding : bindings_) {
        entities[binding.scene_id].emplace(
            binding.authoring_object_id.value, binding.entity);
    }

    for (auto &binding : bindings_) {
        auto &scene = scenes.at(binding.scene_id);
        const auto found =
            scene.by_id.find(binding.authoring_object_id.value);
        if (found == scene.by_id.end()) {
            projectionError(EditorProjectionErrorCode::ComponentNotFound,
                            "/scenes/" + binding.scene_id +
                                "/authoring_objects/" +
                                std::to_string(
                                    binding.authoring_object_id.value) +
                                "/components/transform",
                            "bound runtime object has no authored transform");
        }
        const auto &node = scene.nodes[found->second];
        EntityId parent = invalidEntityId;
        if (!node.parent.empty()) {
            const auto parent_node = scene.by_name.find(node.parent);
            if (parent_node == scene.by_name.end()) {
                projectionError(EditorProjectionErrorCode::ObjectNotFound,
                                node.path,
                                "transform parent does not exist: " +
                                    node.parent);
            }
            const auto parent_entity = entities.at(binding.scene_id).find(
                scene.nodes[parent_node->second]
                    .authoring_object_id.value);
            if (parent_entity == entities.at(binding.scene_id).end()) {
                projectionError(EditorProjectionErrorCode::ObjectNotFound,
                                node.path,
                                "runtime transform parent is not bound: " +
                                    node.parent);
            }
            parent = parent_entity->second;
        }
        prepared_.push_back(PreparedValue{
            .binding = &binding,
            .old_world = *binding.world,
            .old_local = *binding.local,
            .next_world = node.world,
            .next_local = LocalTransformComponent{
                .scale = node.local.scale,
                .rotation = node.local.rotation,
                .pos = node.local.pos,
                .parent = parent,
            },
        });
    }
}

void TransformProjectionAdapter::publish() noexcept {
    for (auto &value : prepared_) {
        *value.binding->local = value.next_local;
        *value.binding->world = value.next_world;
    }
    published_ = true;
}

void TransformProjectionAdapter::rollback() noexcept {
    if (published_) {
        for (auto it = prepared_.rbegin(); it != prepared_.rend(); ++it) {
            *it->binding->local = it->old_local;
            *it->binding->world = it->old_world;
        }
    }
    published_ = false;
    prepared_.clear();
}

void TransformProjectionAdapter::finish() noexcept {
    published_ = false;
    prepared_.clear();
}

EditorProjectionTransaction::EditorProjectionTransaction(
    EditorProjectionDocumentTarget &document_target,
    SceneRevision expected_base_revision,
    EditorProjectionFaultInjector *fault_injector) noexcept
    : document_target_(document_target),
      expected_base_revision_(expected_base_revision),
      fault_injector_(fault_injector) {}

EditorProjectionResult EditorProjectionTransaction::commit(
    std::span<const EditorProjectionCommand> commands,
    std::span<EditorProjectionAdapter *const> adapters) noexcept {
    EditorProjectionResult result;
    std::vector<EditorProjectionAdapter *> prepared;
    std::unordered_set<EditorProjectionAdapter *> published_during_prepare;
    try {
        const auto &base = document_target_.projectionDocument();
        result.base_revision = base.revision();
        if (base.revision() != expected_base_revision_) {
            result.status = EditorProjectionStatus::Rejected;
            result.error = EditorProjectionError{
                EditorProjectionErrorCode::StaleRevision, "/",
                "expected scene revision " +
                    std::to_string(expected_base_revision_.value) +
                    ", current revision is " +
                    std::to_string(base.revision().value)};
            return result;
        }

        auto staged_json = base.rawJson();
        std::optional<AuthoringSceneDocumentStage> structural_stage;
        for (const auto &command : commands) {
            if (!command.apply && !command.structural_apply) {
                projectionError(EditorProjectionErrorCode::CommandInvalid,
                                command.object_path,
                                "projection command has no apply callback");
            }
            if (command.structural_apply) {
                if (!structural_stage) {
                    structural_stage.emplace(base.structuralStage());
                    structural_stage->rawJson() = std::move(staged_json);
                }
                command.structural_apply(*structural_stage);
            } else if (structural_stage) {
                command.apply(structural_stage->rawJson());
            } else {
                command.apply(staged_json);
            }
        }

        std::vector<AuthoringStructuralChange> structural_changes;
        std::vector<AuthoringObjectClosure> removed_objects;
        AuthoringSceneDocument staged;
        if (structural_stage) {
            structural_changes.assign(structural_stage->changes().begin(),
                                      structural_stage->changes().end());
            removed_objects.assign(structural_stage->removedObjects().begin(),
                                   structural_stage->removedObjects().end());
            staged = std::move(*structural_stage)
                         .finish(document_target_.nextProjectionRevision());
        } else {
            staged = base.stage(std::move(staged_json),
                                document_target_.nextProjectionRevision());
        }

        std::unordered_set<EditorProjectionAdapter *> adapter_addresses;
        std::unordered_set<std::string> adapter_names;
        adapter_addresses.reserve(adapters.size());
        adapter_names.reserve(adapters.size());
        for (auto *adapter : adapters) {
            if (adapter == nullptr) {
                projectionError(EditorProjectionErrorCode::CommandInvalid,
                                "/adapters",
                                "projection adapter cannot be null");
            }
            if (adapter->name().empty() ||
                !adapter_addresses.insert(adapter).second ||
                !adapter_names.insert(std::string{adapter->name()}).second) {
                projectionError(EditorProjectionErrorCode::CommandInvalid,
                                "/adapters",
                                "projection adapter name or identity is duplicated");
            }
        }

        prepared.reserve(adapters.size());
        published_during_prepare.reserve(adapters.size());
        const EditorProjectionPrepareContext context{base, staged};
        for (auto *adapter : adapters) {
            prepared.push_back(adapter);
            if (fault_injector_ != nullptr &&
                fault_injector_->shouldFail(EditorProjectionFaultPoint::Prepare,
                                            adapter->name())) {
                projectionError(EditorProjectionErrorCode::AdapterPrepareFailed,
                                "/adapters/" +
                                    std::string{adapter->name()},
                                "injected adapter prepare failure");
            }
            adapter->prepare(context);
            // ECS structural tokens intentionally keep the core's mutation
            // guard until publication. Publish each one before preparing the
            // next structural participant, while retaining its inverse token
            // for aggregate rollback if a later adapter fails.
            if (adapter->kind() == EditorProjectionAdapterKind::EcsArchetype &&
                adapter->publicationMode() ==
                    EditorProjectionPublicationMode::InverseToken) {
                if (fault_injector_ != nullptr &&
                    fault_injector_->shouldFail(
                        EditorProjectionFaultPoint::Publish,
                        adapter->name())) {
                    for (auto it = prepared.rbegin(); it != prepared.rend();
                         ++it) {
                        (*it)->rollback();
                    }
                    result.status = EditorProjectionStatus::Failed;
                    result.error = EditorProjectionError{
                        EditorProjectionErrorCode::AdapterPublishFailed,
                        "/adapters/" + std::string{adapter->name()},
                        "injected inverse-token publish failure"};
                    return result;
                }
                adapter->publish();
                published_during_prepare.insert(adapter);
            }
        }

        for (auto *adapter : adapters) {
            if (published_during_prepare.contains(adapter)) continue;
            if (adapter->publicationMode() ==
                    EditorProjectionPublicationMode::InverseToken &&
                fault_injector_ != nullptr &&
                fault_injector_->shouldFail(EditorProjectionFaultPoint::Publish,
                                            adapter->name())) {
                for (auto it = prepared.rbegin(); it != prepared.rend(); ++it) {
                    (*it)->rollback();
                }
                result.status = EditorProjectionStatus::Failed;
                result.error = EditorProjectionError{
                    EditorProjectionErrorCode::AdapterPublishFailed,
                    "/adapters/" + std::string{adapter->name()},
                    "injected inverse-token publish failure"};
                return result;
            }
            adapter->publish();
        }

        const auto committed_revision = staged.revision();
        document_target_.publishProjectionDocument(std::move(staged));
        for (auto it = prepared.rbegin(); it != prepared.rend(); ++it) {
            (*it)->finish();
        }
        result.status = EditorProjectionStatus::Committed;
        result.committed_revision = committed_revision;
        result.structural_changes = std::move(structural_changes);
        result.removed_objects = std::move(removed_objects);
        return result;
    } catch (const std::exception &error) {
        for (auto it = prepared.rbegin(); it != prepared.rend(); ++it) {
            (*it)->rollback();
        }
        result.status = prepared.empty() ? EditorProjectionStatus::Rejected
                                         : EditorProjectionStatus::Failed;
        result.error = errorFromException(
            prepared.empty() ? EditorProjectionErrorCode::CommandInvalid
                             : EditorProjectionErrorCode::AdapterPrepareFailed,
            prepared.empty() ? "/commands"
                             : "/adapters/" +
                                   std::string{prepared.back()->name()},
            error);
        return result;
    } catch (...) {
        for (auto it = prepared.rbegin(); it != prepared.rend(); ++it) {
            (*it)->rollback();
        }
        result.status = prepared.empty() ? EditorProjectionStatus::Rejected
                                         : EditorProjectionStatus::Failed;
        result.error = EditorProjectionError{
            prepared.empty() ? EditorProjectionErrorCode::CommandInvalid
                             : EditorProjectionErrorCode::AdapterPrepareFailed,
            prepared.empty() ? "/commands"
                             : "/adapters/" +
                                   std::string{prepared.back()->name()},
            "unknown projection transaction failure"};
        return result;
    }
}

} // namespace Pelican
