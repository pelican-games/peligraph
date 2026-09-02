#include "editorprojectionplan.hpp"

#include <algorithm>
#include <bit>
#include <concepts>
#include <limits>
#include <type_traits>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

void hashBytes(std::uint64_t &hash, std::string_view bytes) noexcept {
    for (const unsigned char value : bytes) {
        hash ^= value;
        hash *= fnv_prime;
    }
    hash ^= 0xffU;
    hash *= fnv_prime;
}

template <class Value>
void hashScalar(std::uint64_t &hash, Value value) noexcept {
    static_assert(std::is_integral_v<Value> || std::is_enum_v<Value>);
    if constexpr (std::is_enum_v<Value>) {
        using Unsigned =
            std::make_unsigned_t<std::underlying_type_t<Value>>;
        auto bits = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(bits); ++index) {
            hash ^= static_cast<std::uint8_t>(bits & 0xffU);
            hash *= fnv_prime;
            bits >>= 8U;
        }
    } else {
        using Unsigned = std::make_unsigned_t<Value>;
        auto bits = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(bits); ++index) {
            hash ^= static_cast<std::uint8_t>(bits & 0xffU);
            hash *= fnv_prime;
            bits >>= 8U;
        }
    }
}

[[noreturn]] void planFailure(std::string name, std::string message) {
    throw EditorProjectionPlanError{std::move(name), std::move(message)};
}

AuthoringObjectId objectId(const Json &operation,
                           std::string_view field) {
    if (!operation.contains(field) || !operation.at(field).is_number_unsigned()) {
        planFailure("editor_projection_invalid_request@1",
                    "canonical editor operation has no unsigned object id");
    }
    return AuthoringObjectId{operation.at(field).get<std::uint64_t>()};
}

std::string stringField(const Json &operation, std::string_view field) {
    if (!operation.contains(field) || !operation.at(field).is_string()) {
        planFailure("editor_projection_invalid_request@1",
                    "canonical editor operation has no string field: " +
                        std::string{field});
    }
    return operation.at(field).get<std::string>();
}

const ResolvedObject *findObject(const SceneProjectionState &state,
                                 AuthoringObjectId object_id,
                                 std::string *scene_id = nullptr) noexcept {
    for (const auto &scene : state.resolved().scenes()) {
        const auto found = std::find_if(
            scene.objects.begin(), scene.objects.end(),
            [object_id](const ResolvedObject &object) {
                return object.authoring_object_id == object_id;
            });
        if (found != scene.objects.end()) {
            if (scene_id != nullptr) *scene_id = scene.scene_id;
            return &*found;
        }
    }
    return nullptr;
}

std::string requireObjectScene(const SceneProjectionState &state,
                               AuthoringObjectId object_id) {
    std::string scene_id;
    if (findObject(state, object_id, &scene_id) == nullptr) {
        planFailure("editor_projection_object_missing@1",
                    "editor projection object is absent from projection state: " +
                        std::to_string(object_id.value));
    }
    return scene_id;
}

std::pair<std::string, std::string> freezeBehavior(
    const Json &component) {
    if (!component.is_object() ||
        component.value("name", std::string{}) != "behavior" ||
        !component.contains("type") || !component.at("type").is_string()) {
        planFailure("editor_projection_invalid_request@1",
                    "canonical behavior component is malformed");
    }
    const auto stable_name = component.at("type").get<std::string>();
    const Json params =
        component.contains("params")
            ? Json(component.at("params"))
            : Json::object();
    const auto canonical_params = params.dump();
    Json canonical{{"name", "behavior"},
                   {"type", stable_name},
                   {"params", params}};
    return {canonical.dump(), canonical_params};
}

std::string freezeCodecComponent(const Json &component) {
    return component.dump();
}

std::vector<AuthoringObjectId> objectIds(const Json &operation) {
    if (!operation.contains("object_ids") ||
        !operation.at("object_ids").is_array()) {
        planFailure("editor_projection_invalid_request@1",
                    "canonical lifecycle operation has no object_ids array");
    }
    std::vector<AuthoringObjectId> result;
    result.reserve(operation.at("object_ids").size());
    for (const auto &value : operation.at("object_ids")) {
        if (!value.is_number_unsigned()) {
            planFailure("editor_projection_invalid_request@1",
                        "canonical lifecycle object id is not unsigned");
        }
        result.push_back(AuthoringObjectId{value.get<std::uint64_t>()});
    }
    return result;
}

EditorCanonicalOperation canonicalOperation(
    const Json &wire, const SceneProjectionState &state) {
    const auto op = stringField(wire, "op");
    if (op == "set_component_value") {
        const auto object_id = objectId(wire, "object_id");
        const auto component = stringField(wire, "component_slot");
        const auto scene_id = wire.contains("scene_id")
                                  ? stringField(wire, "scene_id")
                                  : requireObjectScene(state, object_id);
        if (!wire.contains("authored_component")) {
            planFailure("editor_projection_invalid_request@1",
                        "set operation has no authored component");
        }
        std::string canonical;
        std::optional<std::string> behavior_params;
        if (component == "behavior") {
            auto value = freezeBehavior(wire.at("authored_component"));
            canonical = std::move(value.first);
            behavior_params = std::move(value.second);
        } else {
            canonical = freezeCodecComponent(wire.at("authored_component"));
        }
        return EditorSetComponentValueOperation{
            .object_id = object_id,
            .scene_id = scene_id,
            .component = component,
            .attachment_index = wire.contains("attachment_index")
                                    ? std::optional<std::size_t>{
                                          wire.at("attachment_index")
                                              .get<std::size_t>()}
                                    : std::nullopt,
            .canonical_component = std::move(canonical),
            .canonical_behavior_params = std::move(behavior_params),
            .wire = wire,
        };
    }
    if (op == "add_component") {
        const auto object_id = objectId(wire, "object_id");
        const auto component = stringField(wire, "component_slot");
        const auto scene_id = wire.contains("scene_id")
                                  ? stringField(wire, "scene_id")
                                  : requireObjectScene(state, object_id);
        if (!wire.contains("component")) {
            planFailure("editor_projection_invalid_request@1",
                        "add operation has no component value");
        }
        std::string canonical;
        std::optional<std::string> behavior_params;
        if (component == "behavior") {
            auto value = freezeBehavior(wire.at("component"));
            canonical = std::move(value.first);
            behavior_params = std::move(value.second);
        } else {
            canonical = freezeCodecComponent(wire.at("component"));
        }
        return EditorAddComponentOperation{
            .object_id = object_id,
            .scene_id = scene_id,
            .component = component,
            .component_index = wire.at("component_index").get<std::size_t>(),
            .canonical_component = std::move(canonical),
            .canonical_behavior_params = std::move(behavior_params),
            .wire = wire,
        };
    }
    if (op == "remove_component") {
        const auto object_id = objectId(wire, "object_id");
        return EditorRemoveComponentOperation{
            .object_id = object_id,
            .scene_id = wire.contains("scene_id")
                            ? stringField(wire, "scene_id")
                            : requireObjectScene(state, object_id),
            .component = stringField(wire, "component_slot"),
            .component_index = wire.contains("component_index")
                                   ? wire.at("component_index")
                                         .get<std::size_t>()
                                   : 0,
            .wire = wire,
        };
    }
    if (op == "reparent") {
        const auto object_id = objectId(wire, "object_id");
        return EditorReparentOperation{
            .object_id = object_id,
            .scene_id = wire.contains("scene_id")
                            ? stringField(wire, "scene_id")
                            : requireObjectScene(state, object_id),
            .wire = wire,
        };
    }
    if (op == "spawn") {
        return EditorSpawnOperation{
            .object_id = objectId(wire, "object_id"),
            .scene_id = stringField(wire, "scene_id"),
            .wire = wire,
        };
    }
    if (op == "destroy") {
        return EditorDestroyOperation{.object_ids = objectIds(wire),
                                      .wire = wire};
    }
    if (op == "remove_objects") {
        return EditorRemoveObjectsOperation{.object_ids = objectIds(wire),
                                            .wire = wire};
    }
    if (op == "restore_objects") {
        if (!wire.contains("closures") || !wire.at("closures").is_array()) {
            planFailure("editor_projection_invalid_request@1",
                        "restore operation has no closures array");
        }
        std::vector<EditorRestoreObject> objects;
        objects.reserve(wire.at("closures").size());
        for (const auto &closure : wire.at("closures")) {
            objects.push_back(EditorRestoreObject{
                .object_id = objectId(closure, "authoring_object_id"),
                .scene_id = stringField(closure, "scene_id"),
            });
        }
        return EditorRestoreObjectsOperation{.objects = std::move(objects),
                                             .wire = wire};
    }
    if (op == "set_prefab_parameter" || op == "unset_prefab_parameter") {
        const auto object_id = objectId(wire, "object");
        const auto scene_id = requireObjectScene(state, object_id);
        if (op == "set_prefab_parameter") {
            return EditorSetPrefabParameterOperation{object_id, scene_id, wire};
        }
        return EditorUnsetPrefabParameterOperation{object_id, scene_id, wire};
    }
    if (op == "unpack_prefab" || op == "restore_prefab") {
        const auto object_id = objectId(wire, "object");
        const auto scene_id = wire.contains("scene_id")
                                  ? stringField(wire, "scene_id")
                                  : requireObjectScene(state, object_id);
        const auto transition = originTransitionMapFromJson(
            wire.at("origin_transition"));
        if (op == "unpack_prefab") {
            return EditorUnpackPrefabOperation{object_id, scene_id,
                                               transition, wire};
        }
        return EditorRestorePrefabOperation{object_id, scene_id,
                                            transition, wire};
    }
    planFailure("editor_projection_invalid_request@1",
                "unknown canonical editor operation: " + op);
}

EditorProjectionRouteKind policyRoute(EditorProjectionPolicy policy) {
    switch (policy) {
    case EditorProjectionPolicy::TransformClosure:
        return EditorProjectionRouteKind::TransformClosure;
    case EditorProjectionPolicy::RendererModel:
        return EditorProjectionRouteKind::RendererModel;
    case EditorProjectionPolicy::EcsCodec:
        return EditorProjectionRouteKind::EcsCodec;
    case EditorProjectionPolicy::Camera:
        return EditorProjectionRouteKind::Camera;
    case EditorProjectionPolicy::Light:
        return EditorProjectionRouteKind::Light;
    case EditorProjectionPolicy::Collider:
        return EditorProjectionRouteKind::Collider;
    }
    throw std::logic_error("unknown editor projection policy");
}

std::size_t codecIndex(const EditorProjectionContext &context,
                       std::string_view component) {
    const auto catalog = context.codecCatalog();
    const auto found = std::find_if(
        catalog.begin(), catalog.end(), [&](const ComponentCodec &codec) {
            return codec.name == component;
        });
    if (found == catalog.end()) {
        planFailure("editor_projection_codec_missing@1",
                    "component codec is absent from context snapshot: " +
                        std::string{component});
    }
    return static_cast<std::size_t>(found - catalog.begin());
}

void appendPolicyRoute(EditorProjectionPlan &plan, std::size_t operation_index,
                       std::string scene_id, AuthoringObjectId object_id,
                       std::string component) {
    const auto index = codecIndex(plan.context, component);
    const auto &codec = plan.context.codecCatalog()[index];
    if (plan.context.mode() == EditorProjectionMode::Preview &&
        codec.editor_policy != EditorProjectionPolicy::TransformClosure &&
        codec.editor_policy != EditorProjectionPolicy::Light) {
        planFailure("editor_preview_unsupported@1",
                    "component has no live preview projection policy: " +
                        component);
    }
    plan.routes.push_back(EditorProjectionRoute{
        .kind = policyRoute(codec.editor_policy),
        .operation_index = operation_index,
        .scene_id = std::move(scene_id),
        .object_id = object_id,
        .component = std::move(component),
        .codec_index = index,
    });
}

void appendLifecycleRoutes(EditorProjectionPlan &plan,
                           std::size_t operation_index,
                           std::string scene_id,
                           AuthoringObjectId object_id) {
    constexpr EditorProjectionRouteKind kinds[]{
        EditorProjectionRouteKind::ObjectLifecycle,
        EditorProjectionRouteKind::LifecycleRenderer,
        EditorProjectionRouteKind::LifecycleCamera,
        EditorProjectionRouteKind::LifecycleLight,
        EditorProjectionRouteKind::LifecycleCollider,
        EditorProjectionRouteKind::LifecycleBehavior,
    };
    for (const auto kind : kinds) {
        plan.routes.push_back(EditorProjectionRoute{
            .kind = kind,
            .operation_index = operation_index,
            .scene_id = scene_id,
            .object_id = object_id,
        });
    }
}

void appendPrefabRoutes(EditorProjectionPlan &plan,
                        std::size_t operation_index,
                        std::string scene_id,
                        AuthoringObjectId object_id,
                        bool full_component_projection,
                        std::optional<OriginTransitionMap> transition =
                            std::nullopt) {
    plan.routes.push_back(EditorProjectionRoute{
        .kind = EditorProjectionRouteKind::Prefab,
        .operation_index = operation_index,
        .scene_id = scene_id,
        .object_id = object_id,
        .full_component_projection = full_component_projection,
    });
    if (full_component_projection) {
        for (std::size_t index = 0;
             index < plan.context.codecCatalog().size(); ++index) {
            const auto &codec = plan.context.codecCatalog()[index];
            plan.routes.push_back(EditorProjectionRoute{
                .kind = policyRoute(codec.editor_policy),
                .operation_index = operation_index,
                .scene_id = scene_id,
                .object_id = object_id,
                .component = std::string{codec.name},
                .codec_index = index,
                .full_component_projection = true,
            });
        }
        constexpr EditorProjectionRouteKind participant_kinds[]{
            EditorProjectionRouteKind::PrefabRenderer,
            EditorProjectionRouteKind::PrefabCamera,
            EditorProjectionRouteKind::PrefabLight,
            EditorProjectionRouteKind::PrefabCollider,
        };
        for (const auto kind : participant_kinds) {
            plan.routes.push_back(EditorProjectionRoute{
                .kind = kind,
                .operation_index = operation_index,
                .scene_id = scene_id,
                .object_id = object_id,
                .full_component_projection = true,
            });
        }
    }
    plan.routes.push_back(EditorProjectionRoute{
        .kind = EditorProjectionRouteKind::PrefabBehavior,
        .operation_index = operation_index,
        .scene_id = scene_id,
        .object_id = object_id,
        .full_component_projection = full_component_projection,
    });
    if (transition) {
        plan.routes.push_back(EditorProjectionRoute{
            .kind = EditorProjectionRouteKind::OriginTransition,
            .operation_index = operation_index,
            .scene_id = std::move(scene_id),
            .object_id = object_id,
            .origin_transition = std::move(*transition),
        });
    }
}

} // namespace

EditorCanonicalOperation freezeEditorCanonicalOperation(
    const nlohmann::ordered_json &prepared_wire,
    const SceneProjectionState &projection_state) {
    return canonicalOperation(prepared_wire, projection_state);
}

std::vector<EditorCanonicalOperation> freezeEditorCanonicalOperations(
    std::span<const nlohmann::ordered_json> prepared_wire,
    const SceneProjectionState &projection_state) {
    std::vector<EditorCanonicalOperation> result;
    result.reserve(prepared_wire.size());
    for (const auto &wire : prepared_wire) {
        result.push_back(canonicalOperation(wire, projection_state));
    }
    return result;
}

const nlohmann::ordered_json &editorCanonicalOperationWire(
    const EditorCanonicalOperation &operation) noexcept {
    return std::visit([](const auto &typed) -> const nlohmann::ordered_json & {
        return typed.wire;
    }, operation);
}

EditorProjectionContext::EditorProjectionContext(
    std::string active_scene_id, EditorProjectionMode mode,
    std::vector<ComponentCodec> codec_catalog,
    EditorLayoutSemanticMode layout_mode,
    std::uint64_t layout_generation,
    EditorBehaviorAvailability behavior_availability,
    std::vector<EditorBehaviorPolicy> behavior_registry,
    std::uint64_t behavior_generation,
    ComponentCodecCanonicalDefaults canonical_defaults)
    : active_scene_id_{std::move(active_scene_id)}, mode_{mode},
      codec_catalog_{std::move(codec_catalog)}, layout_mode_{layout_mode},
      layout_generation_{layout_generation},
      behavior_availability_{behavior_availability},
      behavior_registry_{std::move(behavior_registry)},
      behavior_generation_{behavior_generation},
      canonical_defaults_{canonical_defaults} {
    std::sort(codec_catalog_.begin(), codec_catalog_.end(),
              [](const ComponentCodec &left, const ComponentCodec &right) {
                  return left.name < right.name;
              });
    if (std::adjacent_find(codec_catalog_.begin(), codec_catalog_.end(),
                           [](const ComponentCodec &left,
                              const ComponentCodec &right) {
                               return left.name == right.name;
                           }) != codec_catalog_.end()) {
        throw std::invalid_argument(
            "editor projection codec catalog contains duplicate names");
    }
    for (const auto &codec : codec_catalog_) {
        const bool erased = codec.editor_prepare != nullptr &&
                            codec.editor_swap != nullptr;
        if ((codec.editor_policy == EditorProjectionPolicy::EcsCodec) !=
            erased) {
            throw std::invalid_argument(
                "EcsCodec editor policy requires prepare and swap hooks");
        }
    }
    std::sort(behavior_registry_.begin(), behavior_registry_.end(),
              [](const EditorBehaviorPolicy &left,
                 const EditorBehaviorPolicy &right) {
                  return left.stable_name < right.stable_name;
              });
    if (layout_generation_ != editorProjectionCodecGeneration(
                                  codec_catalog_, layout_mode_,
                                  canonical_defaults_)) {
        throw std::invalid_argument(
            "editor projection layout generation does not match snapshot");
    }
    if (behavior_generation_ != editorProjectionBehaviorGeneration(
                                    behavior_registry_,
                                    behavior_availability_)) {
        throw std::invalid_argument(
            "editor projection behavior generation does not match snapshot");
    }
}

const ComponentCodec *EditorProjectionContext::findCodec(
    std::string_view name) const noexcept {
    const auto found = std::lower_bound(
        codec_catalog_.begin(), codec_catalog_.end(), name,
        [](const ComponentCodec &codec, std::string_view value) {
            return codec.name < value;
        });
    return found != codec_catalog_.end() && found->name == name ? &*found
                                                                : nullptr;
}

const EditorBehaviorPolicy *EditorProjectionContext::findBehavior(
    std::string_view stable_name) const noexcept {
    const auto found = std::lower_bound(
        behavior_registry_.begin(), behavior_registry_.end(), stable_name,
        [](const EditorBehaviorPolicy &policy, std::string_view value) {
            return policy.stable_name < value;
        });
    return found != behavior_registry_.end() &&
                   found->stable_name == stable_name
               ? &*found
               : nullptr;
}

std::uint64_t editorProjectionCodecGeneration(
    std::span<const ComponentCodec> codecs,
    EditorLayoutSemanticMode layout_mode,
    ComponentCodecCanonicalDefaults defaults) noexcept {
    std::vector<const ComponentCodec *> ordered;
    ordered.reserve(codecs.size());
    for (const auto &codec : codecs) ordered.push_back(&codec);
    std::sort(ordered.begin(), ordered.end(), [](const auto *left,
                                                 const auto *right) {
        return left->name < right->name;
    });
    std::uint64_t hash = fnv_offset;
    hashScalar(hash, layout_mode);
    hashScalar(hash, std::bit_cast<std::uint32_t>(defaults.collider_radius));
    for (const auto *codec : ordered) {
        hashBytes(hash, codec->name);
        hashScalar(hash, codec->runtime_kind);
        hashScalar(hash, codec->editor_policy);
        hashScalar(hash,
                   static_cast<std::uint8_t>(codec->editor_prepare != nullptr));
        hashScalar(hash,
                   static_cast<std::uint8_t>(codec->editor_swap != nullptr));
    }
    return hash;
}

std::uint64_t editorProjectionBehaviorGeneration(
    std::span<const EditorBehaviorPolicy> registry,
    EditorBehaviorAvailability availability) noexcept {
    std::vector<const EditorBehaviorPolicy *> ordered;
    ordered.reserve(registry.size());
    for (const auto &entry : registry) ordered.push_back(&entry);
    std::sort(ordered.begin(), ordered.end(), [](const auto *left,
                                                 const auto *right) {
        return left->stable_name < right->stable_name;
    });
    std::uint64_t hash = fnv_offset;
    hashScalar(hash, availability);
    for (const auto *entry : ordered) {
        hashBytes(hash, entry->stable_name);
        hashScalar(hash, entry->schema_version);
        hashScalar(hash, entry->owner_generation);
        hashScalar(hash,
                   static_cast<std::uint8_t>(entry->canonicalize != nullptr));
    }
    return hash;
}

EditorProjectionPlanError::EditorProjectionPlanError(std::string name,
                                                     std::string message)
    : std::runtime_error{name + ": " + message}, name_{std::move(name)} {}

EditorProjectionPlan::EditorProjectionPlan(
    SceneRevision base_revision_value, EditorProjectionContext context_value,
    bool restore_committed_value)
    : base_revision{base_revision_value}, context{std::move(context_value)},
      restore_committed{restore_committed_value} {}

EditorProjectionPlan planEditorProjection(
    const EditorProjectionPlanningRequest &request,
    const SceneProjectionState &projection_state,
    const EditorProjectionContext &context) {
    EditorProjectionPlan plan{request.base_revision, context,
                              request.restore_committed};
    plan.commands.assign(request.commands.begin(), request.commands.end());
    plan.command_paths.reserve(request.commands.size());
    for (const auto &command : request.commands) {
        plan.command_paths.push_back(command.object_path);
    }

    if (request.restore_committed) {
        if (context.mode() != EditorProjectionMode::Preview ||
            !request.operations.empty() || !request.commands.empty()) {
            planFailure("editor_projection_invalid_request@1",
                        "restore_committed is an empty preview operation");
        }
        for (std::size_t index = 0; index < context.codecCatalog().size();
             ++index) {
            const auto &codec = context.codecCatalog()[index];
            if (codec.editor_policy !=
                    EditorProjectionPolicy::TransformClosure &&
                codec.editor_policy != EditorProjectionPolicy::Light) {
                continue;
            }
            plan.routes.push_back(EditorProjectionRoute{
                .kind = policyRoute(codec.editor_policy),
                .scene_id = context.activeSceneId(),
                .component = std::string{codec.name},
                .codec_index = index,
            });
        }
        return plan;
    }

    (void)projection_state;
    plan.operations.assign(request.operations.begin(), request.operations.end());

    for (std::size_t operation_index = 0;
         operation_index < plan.operations.size(); ++operation_index) {
        std::visit(
            [&](const auto &operation) {
                using Operation = std::remove_cvref_t<decltype(operation)>;
                if constexpr (std::same_as<
                                  Operation,
                                  EditorSetComponentValueOperation>) {
                    if (operation.component == "behavior") {
                        if (context.mode() == EditorProjectionMode::Preview) {
                            planFailure("editor_preview_unsupported@1",
                                        "behavior has no live preview policy");
                        }
                        plan.routes.push_back(EditorProjectionRoute{
                            .kind = EditorProjectionRouteKind::BehaviorBatch,
                            .operation_index = operation_index,
                            .scene_id = operation.scene_id,
                            .object_id = operation.object_id,
                            .component = operation.component,
                        });
                    } else {
                        appendPolicyRoute(plan, operation_index,
                                          operation.scene_id,
                                          operation.object_id,
                                          operation.component);
                    }
                } else if constexpr (
                    std::same_as<Operation, EditorAddComponentOperation> ||
                    std::same_as<Operation,
                                 EditorRemoveComponentOperation>) {
                    if (context.mode() == EditorProjectionMode::Preview) {
                        planFailure("editor_preview_unsupported@1",
                                    "component lifecycle has no live preview policy");
                    }
                    if (operation.component == "behavior") {
                        plan.routes.push_back(EditorProjectionRoute{
                            .kind = EditorProjectionRouteKind::BehaviorBatch,
                            .operation_index = operation_index,
                            .scene_id = operation.scene_id,
                            .object_id = operation.object_id,
                            .component = operation.component,
                        });
                    } else {
                        appendPolicyRoute(plan, operation_index,
                                          operation.scene_id,
                                          operation.object_id,
                                          operation.component);
                        plan.routes.push_back(EditorProjectionRoute{
                            .kind =
                                EditorProjectionRouteKind::ComponentLifecycle,
                            .operation_index = operation_index,
                            .scene_id = operation.scene_id,
                            .object_id = operation.object_id,
                            .component = operation.component,
                            .codec_index = codecIndex(context,
                                                      operation.component),
                        });
                    }
                } else if constexpr (
                    std::same_as<Operation, EditorReparentOperation>) {
                    if (context.mode() == EditorProjectionMode::Preview) {
                        planFailure("editor_preview_unsupported@1",
                                    "reparent has no live preview policy");
                    }
                    const auto transform = std::find_if(
                        context.codecCatalog().begin(),
                        context.codecCatalog().end(),
                        [](const ComponentCodec &codec) {
                            return codec.editor_policy ==
                                   EditorProjectionPolicy::TransformClosure;
                        });
                    if (transform == context.codecCatalog().end()) {
                        planFailure("editor_projection_codec_missing@1",
                                    "transform closure policy is absent");
                    }
                    const auto index = static_cast<std::size_t>(
                        transform - context.codecCatalog().begin());
                    plan.routes.push_back(EditorProjectionRoute{
                        .kind = EditorProjectionRouteKind::TransformClosure,
                        .operation_index = operation_index,
                        .scene_id = operation.scene_id,
                        .object_id = operation.object_id,
                        .component = std::string{transform->name},
                        .codec_index = index,
                    });
                    plan.routes.push_back(EditorProjectionRoute{
                        .kind = EditorProjectionRouteKind::Reparent,
                        .operation_index = operation_index,
                        .scene_id = operation.scene_id,
                        .object_id = operation.object_id,
                    });
                } else if constexpr (
                    std::same_as<Operation, EditorSpawnOperation>) {
                    if (context.mode() == EditorProjectionMode::Preview) {
                        planFailure("editor_preview_unsupported@1",
                                    "spawn has no live preview policy");
                    }
                    appendLifecycleRoutes(plan, operation_index,
                                          operation.scene_id,
                                          operation.object_id);
                } else if constexpr (
                    std::same_as<Operation, EditorDestroyOperation> ||
                    std::same_as<Operation, EditorRemoveObjectsOperation>) {
                    if (context.mode() == EditorProjectionMode::Preview) {
                        planFailure("editor_preview_unsupported@1",
                                    "object removal has no live preview policy");
                    }
                    for (const auto object_id : operation.object_ids) {
                        appendLifecycleRoutes(
                            plan, operation_index,
                            requireObjectScene(projection_state, object_id),
                            object_id);
                    }
                } else if constexpr (
                    std::same_as<Operation,
                                 EditorRestoreObjectsOperation>) {
                    if (context.mode() == EditorProjectionMode::Preview) {
                        planFailure("editor_preview_unsupported@1",
                                    "object restore has no live preview policy");
                    }
                    for (const auto &object : operation.objects) {
                        appendLifecycleRoutes(plan, operation_index,
                                              object.scene_id,
                                              object.object_id);
                    }
                } else if constexpr (
                    std::same_as<Operation,
                                 EditorSetPrefabParameterOperation> ||
                    std::same_as<Operation,
                                 EditorUnsetPrefabParameterOperation>) {
                    if (context.mode() == EditorProjectionMode::Preview) {
                        planFailure("editor_preview_unsupported@1",
                                    "prefab parameter has no live preview policy");
                    }
                    appendPrefabRoutes(plan, operation_index,
                                       operation.scene_id,
                                       operation.object_id, true);
                } else if constexpr (
                    std::same_as<Operation, EditorUnpackPrefabOperation> ||
                    std::same_as<Operation,
                                 EditorRestorePrefabOperation>) {
                    if (context.mode() == EditorProjectionMode::Preview) {
                        planFailure("editor_preview_unsupported@1",
                                    "prefab lifecycle has no live preview policy");
                    }
                    appendPrefabRoutes(plan, operation_index,
                                       operation.scene_id,
                                       operation.object_id, false,
                                       operation.origin_transition);
                }
            },
            plan.operations[operation_index]);
    }
    return plan;
}

} // namespace Pelican
