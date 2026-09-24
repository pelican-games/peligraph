#include "runtimespawn.hpp"

#include "authoringscenedocument.hpp"
#include "basicconfig.hpp"
#include "editorprojectionadapters.hpp"
#include "scene.hpp"
#include "../asset/model.hpp"
#include "../ecs/componentinfo.hpp"
#include "../ecs/core.hpp"
#include "../gamelogic/gamelogicreload.hpp"
#include "../log.hpp"
#include "../phys/physworld.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/gamecontext.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace internal {
// The only place a TransientObjectRef is minted.
struct TransientObjectRefAccess {
    static TransientObjectRef make(SceneAuthority authority,
                                   std::uint64_t boundary,
                                   std::uint32_t index) noexcept {
        return TransientObjectRef{
            ObjectAuthority{.run_nonce = authority.run_nonce,
                            .scene_epoch = authority.scene_epoch},
            boundary, index};
    }
};
} // namespace internal

namespace {

using Json = nlohmann::json;

SceneAuthority authorityOf(const TransientObjectRef &ref) noexcept {
    return SceneAuthority{.run_nonce = ref.authority().run_nonce,
                          .scene_epoch = ref.authority().scene_epoch};
}

[[noreturn]] void refuse(std::string code, const std::string &detail) {
    throw SpawnError{std::move(code), detail};
}

const ResolvedComponent *findComponent(const ResolvedObject &object,
                                       std::string_view name) noexcept {
    for (const auto &component : object.components) {
        if (component.name == name) return &component;
    }
    return nullptr;
}

Json transformJson(const WorldTransform &transform) {
    return Json{
        {"name", "transform"},
        {"pos", {transform.pos.x, transform.pos.y, transform.pos.z}},
        {"rotation",
         {transform.rotation.x, transform.rotation.y, transform.rotation.z,
          transform.rotation.w}},
        {"scale", {transform.scale.x, transform.scale.y, transform.scale.z}},
    };
}

// A prefab's object parameter names an object of the scene. The resolver
// checks that target inside the document it resolves, so the one-object
// document a spawn resolves carries a stand-in for each target the live
// scene declares: its name and the components the parameter requires, as
// the live scene resolved them. A target the scene does not declare gets no
// stand-in and is refused by the resolver, under its own name.
Json objectParameterStandIns(const Json &instance_block,
                             const ResolvedScene &resolved,
                             const ResolvedSceneView &scene) {
    auto stand_ins = Json::array();
    const auto instance =
        resolvePrefabInstance(instance_block, resolved.prefabRegistry());
    std::unordered_set<std::string> added;
    for (const auto &parameter : instance.parameters) {
        if (parameter.kind != PrefabParameterKind::Object ||
            !parameter.value_resolved.is_string()) {
            continue;
        }
        const auto target = parameter.value_resolved.get<std::string>();
        if (!added.insert(target).second) continue;
        const auto object = std::find_if(
            scene.objects.begin(), scene.objects.end(),
            [&](const ResolvedObject &candidate) {
                return candidate.name && *candidate.name == target;
            });
        if (object == scene.objects.end()) continue;
        auto components = Json::array();
        for (const auto &component : object->components) {
            if (std::find(parameter.required_components.begin(),
                          parameter.required_components.end(),
                          component.name) !=
                parameter.required_components.end()) {
                components.push_back(component.effective_json);
            }
        }
        stand_ins.push_back(
            Json{{"name", target}, {"components", std::move(components)}});
    }
    return stand_ins;
}

// Expands one prefab instance through the ResolvedScene seam: a scene
// document holding the instance (and the stand-ins above) resolved with the
// run's prefab registry, bindable provider and defaults, as a scene load
// resolves its own. What comes back is the instance's ResolvedObject.
ResolvedObject expandPrefab(std::string_view prefab, const SpawnParams &params,
                            std::string_view instance_id,
                            const SceneObjectRun &run) {
    Json instance_block{{"ref", std::string{prefab}},
                        {"instance_id", std::string{instance_id}},
                        {"parameters", params.parameters}};
    Json object{{"components", Json::array({transformJson(params.transform)})},
                {"prefab", instance_block}};
    if (params.name) object["name"] = *params.name;
    auto objects = objectParameterStandIns(instance_block, *run.resolved,
                                           *run.scene);
    objects.push_back(std::move(object));
    const Json document{
        {"schema", "pelican.scene"},
        {"version", 2},
        {"scenes", {{run.scene->scene_id, {{"objects", std::move(objects)}}}}},
    };
    const auto authoring =
        AuthoringSceneDocument::load(document.dump(), SceneRevision{1});
    auto resolved = ResolvedSceneResolver::resolve(
        authoring, run.resolved->resolverGeneration(),
        run.resolved->defaults(), run.resolved->prefabRegistry(),
        run.resolved->bindableProvider());
    const auto *scene = resolved.findScene(run.scene->scene_id);
    if (scene == nullptr || scene->objects.empty()) {
        throw std::logic_error("a spawn's prefab expansion lost its object");
    }
    return scene->objects.back();
}

} // namespace

TransientObjectRef RuntimeSpawner::spawn(std::string_view prefab,
                                         const SpawnParams &params) {
    const auto run = GET_MODULE(SceneLoader).objectRun();
    if (run.scene == nullptr || run.resolved == nullptr) {
        refuse("object.spawn_outside_scene@1",
               "no scene is loaded to spawn '" + std::string{prefab} + "' into");
    }

    // Its place: the next boundary of its run, after the spawns already
    // queued for that boundary.
    const std::uint64_t boundary =
        run_tracked && run.authority == run_authority ? boundary_count + 1U : 1U;
    const auto queued = static_cast<std::uint64_t>(std::count_if(
        queue.begin(), queue.end(), [&](const Request &request) {
            return request.kind == RequestKind::spawn &&
                   request.authority == run.authority;
        }));
    if (boundary >= runtime_spawn_boundary_limit ||
        queued >= runtime_spawn_index_limit) {
        refuse("object.spawn_sequence_exhausted@1",
               "boundary " + std::to_string(boundary) + " spawn " +
                   std::to_string(queued) + " is past the attachment identifier");
    }
    const auto index = static_cast<std::uint32_t>(queued);

    // The identity design's name rules: an authored object keeps its name,
    // and two transient objects of one run do not share one.
    if (params.name) {
        const auto &name = *params.name;
        if (name.empty()) {
            refuse("object.spawn_invalid@1",
                   "a spawned object's name, when given, must not be empty");
        }
        if (std::any_of(run.scene->objects.begin(), run.scene->objects.end(),
                        [&](const ResolvedObject &object) {
                            return object.name && *object.name == name;
                        })) {
            refuse("object.name_reserved_by_authored@1",
                   "'" + name + "' is an authored object of scene '" +
                       run.scene->scene_id + "'");
        }
        const auto taken = [&](const std::optional<std::string> &other) {
            return other && *other == name;
        };
        if (std::any_of(live.begin(), live.end(),
                        [&](const LiveObject &object) {
                            return object.authority == run.authority &&
                                   taken(object.name);
                        }) ||
            std::any_of(queue.begin(), queue.end(),
                        [&](const Request &request) {
                            return request.kind == RequestKind::spawn &&
                                   request.authority == run.authority &&
                                   taken(request.name);
                        })) {
            refuse("object.name_duplicate@1",
                   "another spawned object of this scene run is named '" +
                       name + "'");
        }
    }

    const auto instance_id =
        "spawn-" + std::to_string(boundary) + "-" + std::to_string(index);
    ResolvedObject object;
    try {
        object = expandPrefab(prefab, params, instance_id, run);
    } catch (const SpawnError &) {
        throw;
    } catch (const PrefabError &error) {
        refuse(std::string{prefabErrorCodeName(error.code())}, error.what());
    } catch (const std::exception &error) {
        refuse("object.spawn_invalid@1", error.what());
    }

    // PG-1: the gate a scene load and the editor's add_component ask of the
    // same resolved object.
    if (const auto refusal = resolvedObjectFeatureRefusal(object)) {
        refuse(refusal->code, refusal->message);
    }
    for (const auto &component : object.components) {
        if (component.name == "light" || component.name == "camera") {
            refuse("object.spawn_component_unsupported@1",
                   "prefab '" + std::string{prefab} + "' has a " +
                       component.name +
                       ", which a spawned object cannot carry");
        }
        if (component.runtime_resolution_error) {
            refuse("object.spawn_invalid@1",
                   ResolutionFailureException{*component.runtime_resolution_error}
                       .what());
        }
    }

    std::vector<PreparedSceneBehaviorAttachment> behaviors;
    try {
        behaviors = prepareResolvedSceneBehaviorAttachments(
            std::span<const ResolvedObject>{&object, 1},
            configuredGameLogicStatus().loaded
                ? BehaviorRegistryAvailability::active
                : BehaviorRegistryAvailability::dll_unavailable);
    } catch (const BehaviorTypeUnknownError &error) {
        refuse(std::string{BehaviorTypeUnknownError::codeName()}, error.what());
    } catch (const std::exception &error) {
        refuse("object.spawn_invalid@1", error.what());
    }
    if (behaviors.size() > runtime_spawn_attachment_limit) {
        refuse("object.spawn_sequence_exhausted@1",
               "prefab '" + std::string{prefab} + "' has " +
                   std::to_string(behaviors.size()) +
                   " behaviors, more than an attachment identifier holds");
    }

    queue.push_back(Request{
        .kind = RequestKind::spawn,
        .authority = run.authority,
        .boundary = boundary,
        .index = index,
        .name = params.name,
        .identity_name = params.name ? *params.name
                                     : "spawn:" + std::to_string(boundary) +
                                           "." + std::to_string(index),
        .object = std::move(object),
        .behaviors = std::move(behaviors),
    });
    return internal::TransientObjectRefAccess::make(run.authority, boundary,
                                                    index);
}

Resolved<TransientObjectRef>
RuntimeSpawner::destroy(const TransientObjectRef &ref) {
    const auto run = GET_MODULE(SceneLoader).objectRun();
    const auto authority = authorityOf(ref);
    if (ref.boundary() == 0 || authority != run.authority) {
        return ObjectError::stale_authority;
    }
    const auto same = [&](const Request &request) {
        return request.authority == authority &&
               request.boundary == ref.boundary() &&
               request.index == ref.spawnIndex();
    };
    if (std::any_of(queue.begin(), queue.end(), [&](const Request &request) {
            return request.kind == RequestKind::destroy && same(request);
        })) {
        return ObjectError::not_materialized;
    }
    const auto *object = findLive(authority, ref.boundary(), ref.spawnIndex());
    const bool live_now =
        object != nullptr &&
        GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(object->entity);
    const bool queued = std::any_of(
        queue.begin(), queue.end(), [&](const Request &request) {
            return request.kind == RequestKind::spawn && same(request);
        });
    if (!live_now && !queued) return ObjectError::not_materialized;
    queue.push_back(Request{
        .kind = RequestKind::destroy,
        .authority = authority,
        .boundary = ref.boundary(),
        .index = ref.spawnIndex(),
    });
    return ref;
}

Resolved<GameObjectId>
RuntimeSpawner::materialize(const TransientObjectRef &ref) const {
    const auto run = GET_MODULE(SceneLoader).objectRun();
    const auto authority = authorityOf(ref);
    if (ref.boundary() == 0 || authority != run.authority) {
        return ObjectError::stale_authority;
    }
    const auto *object = findLive(authority, ref.boundary(), ref.spawnIndex());
    if (object == nullptr ||
        !GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(object->entity)) {
        return ObjectError::not_materialized;
    }
    return object->entity;
}

const RuntimeSpawner::LiveObject *
RuntimeSpawner::findLive(SceneAuthority authority, std::uint64_t boundary,
                         std::uint32_t index) const noexcept {
    const auto found = std::find_if(
        live.begin(), live.end(), [&](const LiveObject &object) {
            return object.authority == authority &&
                   object.boundary == boundary && object.index == index;
        });
    return found == live.end() ? nullptr : &*found;
}

void RuntimeSpawner::applyBoundary() {
    // An engine without a project has no scene run, and nothing could have
    // been queued (spawn refuses outside a scene).
    const auto *config = FastModuleContainer::tryGet<ProjectBasicConfig>();
    if (config == nullptr) {
        queue.clear();
        return;
    }
    const auto active = config->activeSceneAuthority();
    if (!run_tracked || active != run_authority) {
        // A new scene run: its objects were retired by the load that began
        // it, and its boundaries count from 1.
        run_authority = active;
        run_tracked = true;
        boundary_count = 0;
        std::erase_if(live, [&](const LiveObject &object) {
            return object.authority != active;
        });
    }
    ++boundary_count;
    if (queue.empty()) return;

    std::vector<Request> requests;
    requests.swap(queue);
    for (auto &request : requests) {
        if (request.authority != active) continue;
        try {
            if (request.kind == RequestKind::spawn) {
                if (request.boundary != boundary_count) {
                    throw std::logic_error(
                        "a spawn reached a boundary other than its own");
                }
                materialize(request);
            } else {
                retire(request);
            }
        } catch (const std::exception &error) {
            if (logger != nullptr) {
                LOG_ERROR(logger, "runtime {} of boundary {} index {} failed: {}",
                          request.kind == RequestKind::spawn ? "spawn"
                                                             : "destroy",
                          request.boundary, request.index, error.what());
            }
        }
    }
}

void RuntimeSpawner::materialize(Request &request) {
    auto &object = *request.object;
    const bool has_model = findComponent(object, "simplemodelview") != nullptr;
    const bool has_physics =
        has_model || findComponent(object, "collider") != nullptr;
    const auto *scene = GET_MODULE(SceneLoader).objectRun().scene;
    SceneObjectProjectionAdapter adapter{
        GET_MODULE(ECSCore).getTemplatePublicModule(),
        GET_MODULE(ComponentInfoManager),
        has_model ? &GET_MODULE(ModelAssetContainer) : nullptr,
        has_model ? &GET_MODULE(PolygonInstanceContainer) : nullptr,
        nullptr,
        nullptr,
        has_physics ? FastModuleContainer::tryGet<PhysWorld>() : nullptr,
        scene != nullptr ? scene->scene_id : std::string{},
        AuthoringObjectId{},
        {},
        SceneObjectProjectionParticipants{.renderer = has_model,
                                          .camera = false,
                                          .light = false,
                                          .collider = has_physics},
    };
    // Projection first (ECS entity, renderer instance, collider), behaviors
    // last: onInit sees a complete object, as it does after a scene load.
    try {
        adapter.prepareTransientSpawn(object, request.identity_name);
    } catch (...) {
        adapter.rollback();
        throw;
    }
    adapter.publish();
    const auto entity = adapter.preparedEntity();
    try {
        GET_MODULE(BehaviorAttachmentArena)
            .attachSpawned(entity, std::move(request.behaviors),
                           request.boundary, request.index);
    } catch (...) {
        adapter.rollback();
        throw;
    }
    adapter.finish();
    live.push_back(LiveObject{
        .authority = request.authority,
        .boundary = request.boundary,
        .index = request.index,
        .entity = entity,
        .name = request.name,
    });
}

void RuntimeSpawner::retire(const Request &request) {
    const auto found = std::find_if(
        live.begin(), live.end(), [&](const LiveObject &object) {
            return object.authority == request.authority &&
                   object.boundary == request.boundary &&
                   object.index == request.index;
        });
    // A spawn that failed to materialize has nothing to end.
    if (found == live.end()) return;
    const auto entity = found->entity;
    live.erase(found);
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    if (!ecs.isAlive(entity)) return;

    // The reverse of materialize: behaviors first (onDestroy, last attached
    // first, while the object is still whole), then the collider and the
    // entity.
    GET_MODULE(BehaviorAttachmentArena).preDestroyEntity(entity);
    // Every module reached here exists once a scene has loaded; none is
    // created at a boundary, after the runtime module graph is frozen.
    SceneObjectProjectionAdapter adapter{
        ecs,
        GET_MODULE(ComponentInfoManager),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        FastModuleContainer::tryGet<PhysWorld>(),
        std::string{},
        AuthoringObjectId{},
        {},
        SceneObjectProjectionParticipants{.renderer = false,
                                          .camera = false,
                                          .light = false,
                                          .collider = true},
    };
    try {
        adapter.prepareTransientDestroy(entity);
    } catch (...) {
        adapter.rollback();
        throw;
    }
    adapter.publish();
    adapter.finish();
}

RuntimeSpawner::Stats RuntimeSpawner::stats() const noexcept {
    return Stats{.run = run_authority,
                 .boundary = boundary_count,
                 .queued = queue.size(),
                 .live = live.size()};
}

} // namespace Pelican
