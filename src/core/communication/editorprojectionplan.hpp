#pragma once

#include "../loader/componentcodec.hpp"
#include "../loader/editorprojectiontransaction.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {

enum class EditorProjectionMode : std::uint8_t {
    Committed,
    Preview,
};

enum class EditorBehaviorAvailability : std::uint8_t {
    Active,
    DllUnavailable,
};

enum class EditorLayoutSemanticMode : std::uint8_t {
    Unconfigured,
    Legacy,
    Layout,
};

struct EditorBehaviorPolicy {
    using Canonicalize = std::string (*)(const nlohmann::json &);

    std::string stable_name;
    std::uint32_t schema_version = 0;
    Canonicalize canonicalize = nullptr;
    std::uint64_t owner_generation = 0;

    friend bool operator==(const EditorBehaviorPolicy &,
                           const EditorBehaviorPolicy &) = default;
};

// A complete value snapshot captured once at the production composition
// boundary.  There are intentionally no mutators: planning can neither query
// nor refresh an ambient module, registry, or thread-local default.
class EditorProjectionContext final {
    std::string active_scene_id_;
    EditorProjectionMode mode_ = EditorProjectionMode::Committed;
    std::vector<ComponentCodec> codec_catalog_;
    EditorLayoutSemanticMode layout_mode_ =
        EditorLayoutSemanticMode::Unconfigured;
    std::uint64_t layout_generation_ = 0;
    EditorBehaviorAvailability behavior_availability_ =
        EditorBehaviorAvailability::DllUnavailable;
    std::vector<EditorBehaviorPolicy> behavior_registry_;
    std::uint64_t behavior_generation_ = 0;
    ComponentCodecCanonicalDefaults canonical_defaults_{};

  public:
    EditorProjectionContext(
        std::string active_scene_id, EditorProjectionMode mode,
        std::vector<ComponentCodec> codec_catalog,
        EditorLayoutSemanticMode layout_mode,
        std::uint64_t layout_generation,
        EditorBehaviorAvailability behavior_availability,
        std::vector<EditorBehaviorPolicy> behavior_registry,
        std::uint64_t behavior_generation,
        ComponentCodecCanonicalDefaults canonical_defaults);

    const std::string &activeSceneId() const noexcept {
        return active_scene_id_;
    }
    EditorProjectionMode mode() const noexcept { return mode_; }
    std::span<const ComponentCodec> codecCatalog() const noexcept {
        return codec_catalog_;
    }
    EditorLayoutSemanticMode layoutMode() const noexcept {
        return layout_mode_;
    }
    std::uint64_t layoutGeneration() const noexcept {
        return layout_generation_;
    }
    EditorBehaviorAvailability behaviorAvailability() const noexcept {
        return behavior_availability_;
    }
    std::span<const EditorBehaviorPolicy> behaviorRegistry() const noexcept {
        return behavior_registry_;
    }
    std::uint64_t behaviorGeneration() const noexcept {
        return behavior_generation_;
    }
    ComponentCodecCanonicalDefaults canonicalDefaults() const noexcept {
        return canonical_defaults_;
    }

    const ComponentCodec *findCodec(std::string_view name) const noexcept;
    const EditorBehaviorPolicy *findBehavior(
        std::string_view stable_name) const noexcept;

    friend bool operator==(const EditorProjectionContext &,
                           const EditorProjectionContext &) = default;
};

std::uint64_t editorProjectionCodecGeneration(
    std::span<const ComponentCodec> codecs,
    EditorLayoutSemanticMode layout_mode,
    ComponentCodecCanonicalDefaults defaults) noexcept;
std::uint64_t editorProjectionBehaviorGeneration(
    std::span<const EditorBehaviorPolicy> registry,
    EditorBehaviorAvailability availability) noexcept;

class EditorProjectionPlanError final : public std::runtime_error {
    std::string name_;
    std::string message_;

  public:
    EditorProjectionPlanError(std::string name, std::string message);
    const std::string &name() const noexcept { return name_; }
    const std::string &message() const noexcept { return message_; }
};

struct EditorProjectionRecoverable {
    std::string name;
    std::string message;

    friend bool operator==(const EditorProjectionRecoverable &,
                           const EditorProjectionRecoverable &) = default;
};

using ProjectionPlanFailure =
    std::variant<EditorProjectionRecoverable, UserDataViolation>;

struct EditorSetComponentValueOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    std::string component;
    std::optional<std::size_t> attachment_index;
    std::string canonical_component;
    std::optional<std::string> canonical_behavior_params;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorSetComponentValueOperation &,
                           const EditorSetComponentValueOperation &) = default;
};

struct EditorAddComponentOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    std::string component;
    std::size_t component_index = 0;
    std::string canonical_component;
    std::optional<std::string> canonical_behavior_params;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorAddComponentOperation &,
                           const EditorAddComponentOperation &) = default;
};

struct EditorRemoveComponentOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    std::string component;
    std::size_t component_index = 0;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorRemoveComponentOperation &,
                           const EditorRemoveComponentOperation &) = default;
};

struct EditorReparentOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorReparentOperation &,
                           const EditorReparentOperation &) = default;
};

struct EditorSpawnOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorSpawnOperation &,
                           const EditorSpawnOperation &) = default;
};

struct EditorDestroyOperation {
    std::vector<AuthoringObjectId> object_ids;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorDestroyOperation &,
                           const EditorDestroyOperation &) = default;
};

struct EditorRemoveObjectsOperation {
    std::vector<AuthoringObjectId> object_ids;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorRemoveObjectsOperation &,
                           const EditorRemoveObjectsOperation &) = default;
};

struct EditorRestoreObject {
    AuthoringObjectId object_id{};
    std::string scene_id;
    friend bool operator==(const EditorRestoreObject &,
                           const EditorRestoreObject &) = default;
};

struct EditorRestoreObjectsOperation {
    std::vector<EditorRestoreObject> objects;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorRestoreObjectsOperation &,
                           const EditorRestoreObjectsOperation &) = default;
};

struct EditorSetPrefabParameterOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorSetPrefabParameterOperation &,
                           const EditorSetPrefabParameterOperation &) = default;
};

struct EditorUnsetPrefabParameterOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorUnsetPrefabParameterOperation &,
                           const EditorUnsetPrefabParameterOperation &) = default;
};

struct EditorUnpackPrefabOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    OriginTransitionMap origin_transition;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorUnpackPrefabOperation &,
                           const EditorUnpackPrefabOperation &) = default;
};

struct EditorRestorePrefabOperation {
    AuthoringObjectId object_id{};
    std::string scene_id;
    OriginTransitionMap origin_transition;
    nlohmann::ordered_json wire;
    friend bool operator==(const EditorRestorePrefabOperation &,
                           const EditorRestorePrefabOperation &) = default;
};

using EditorCanonicalOperation = std::variant<
    EditorSetComponentValueOperation, EditorAddComponentOperation,
    EditorRemoveComponentOperation, EditorReparentOperation,
    EditorSpawnOperation, EditorDestroyOperation,
    EditorRemoveObjectsOperation, EditorRestoreObjectsOperation,
    EditorSetPrefabParameterOperation, EditorUnsetPrefabParameterOperation,
    EditorUnpackPrefabOperation, EditorRestorePrefabOperation>;

// Terminal guard for the `if constexpr` chains that hand-expand the twelve
// alternatives above.  Without an `else` an alternative added to the variant
// compiles and is silently ignored at run time; with
// `else static_assert(editorCanonicalOperationUnhandled<Operation>)` every such
// chain has to name the new alternative before the build succeeds.  Branches
// that deliberately do nothing for a group of alternatives list that group
// explicitly rather than falling through.
template <class Operation>
inline constexpr bool editorCanonicalOperationUnhandled = false;

// Converts journal-prepared wire into the typed operation shared by replay,
// planning, and execution.  The wire value is already canonical here; this
// function only freezes it and derives stable routing fields.  In particular,
// it never consults a codec catalog, behavior registry, module container, or
// thread-local default.
EditorCanonicalOperation freezeEditorCanonicalOperation(
    const nlohmann::ordered_json &prepared_wire,
    const SceneProjectionState &projection_state);
std::vector<EditorCanonicalOperation> freezeEditorCanonicalOperations(
    std::span<const nlohmann::ordered_json> prepared_wire,
    const SceneProjectionState &projection_state);
const nlohmann::ordered_json &editorCanonicalOperationWire(
    const EditorCanonicalOperation &operation) noexcept;

enum class EditorProjectionRouteKind : std::uint8_t {
    DocumentOnly,
    TransformClosure,
    RendererModel,
    EcsCodec,
    Camera,
    Light,
    Collider,
    ComponentLifecycle,
    Reparent,
    ObjectLifecycle,
    LifecycleRenderer,
    LifecycleCamera,
    LifecycleLight,
    LifecycleCollider,
    LifecycleBehavior,
    BehaviorBatch,
    Prefab,
    PrefabRenderer,
    PrefabCamera,
    PrefabLight,
    PrefabCollider,
    PrefabBehavior,
    OriginTransition,
};

struct EditorProjectionRoute {
    EditorProjectionRouteKind kind =
        EditorProjectionRouteKind::TransformClosure;
    std::size_t operation_index = 0;
    std::string scene_id;
    AuthoringObjectId object_id{};
    std::string component;
    std::optional<std::size_t> codec_index;
    bool full_component_projection = false;
    OriginTransitionMap origin_transition;

    friend bool operator==(const EditorProjectionRoute &,
                           const EditorProjectionRoute &) = default;
};

struct EditorProjectionPlanningRequest {
    SceneRevision base_revision{};
    std::span<const EditorProjectionCommand> commands;
    std::span<const EditorCanonicalOperation> operations;
    bool restore_committed = false;
};

struct EditorProjectionPlan {
    SceneRevision base_revision{};
    EditorProjectionContext context;
    bool restore_committed = false;
    std::vector<EditorProjectionCommand> commands;
    std::vector<std::string> command_paths;
    std::vector<EditorCanonicalOperation> operations;
    std::vector<EditorProjectionRoute> routes;

    EditorProjectionPlan(SceneRevision base_revision,
                         EditorProjectionContext context,
                         bool restore_committed);

    friend bool operator==(const EditorProjectionPlan &left,
                           const EditorProjectionPlan &right) {
        return left.base_revision == right.base_revision &&
               left.context == right.context &&
               left.restore_committed == right.restore_committed &&
               left.command_paths == right.command_paths &&
               left.operations == right.operations &&
               left.routes == right.routes;
    }
};

ResolutionResult<EditorProjectionPlan, ProjectionPlanFailure>
planEditorProjection(
    const EditorProjectionPlanningRequest &request,
    const SceneProjectionState &projection_state,
    const EditorProjectionContext &context);

} // namespace Pelican
