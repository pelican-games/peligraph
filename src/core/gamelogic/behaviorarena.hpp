#pragma once

#include "../container.hpp"
#include "../userpublic/behavior.hpp"
#include "../userpublic/details/behavior/registerer.hpp"
#include "../loader/resolvedscene.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

class BehaviorCallbackScope;
struct SceneLoadPhaseTrace;

enum class BehaviorRegistryAvailability {
    active,
    dll_unavailable,
};

class BehaviorTypeUnknownError final : public std::runtime_error {
    std::string stable_name_;
    std::optional<AuthoringObjectId> object_id_;

  public:
    static constexpr std::string_view codeName() noexcept {
        return "behavior_type_unknown@1";
    }

    BehaviorTypeUnknownError(
        std::string stable_name, std::string object_name = {},
        std::optional<AuthoringObjectId> object_id = std::nullopt);

    const std::string &stableName() const noexcept { return stable_name_; }
    const std::optional<AuthoringObjectId> &objectId() const noexcept {
        return object_id_;
    }
};

struct PreparedSceneBehaviorAttachment {
    std::size_t object_index = 0;
    std::size_t component_index = 0;
    ResolvedComponentOrigin component_origin;
    std::string object_name;
    std::string stable_name;
    std::string canonical_params;
    nlohmann::json raw_component;
    internal::RegistrationOwner registration_owner = internal::engineRegistrationOwner;
    bool pending = false;
};

struct BoundSceneBehaviorAttachment {
    PreparedSceneBehaviorAttachment prepared;
    GameObjectId entity = invalidGameObjectId;
};

struct BehaviorAttachmentInfo {
    BehaviorAttachmentHandle handle = invalidBehaviorAttachmentHandle;
    std::uint64_t attachment_seq = 0;
    GameObjectId entity = invalidGameObjectId;
    std::size_t component_index = 0;
    ResolvedComponentOrigin component_origin;
    std::size_t scene_object_index = 0;
    bool scene_ordered = false;
    std::string stable_name;
    std::string canonical_params;
    internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    bool pending = false;
    bool active = false;
    bool activation_failed = false;
    std::string activation_error;
    std::uint8_t activation_delay = 0;
    // GP-2: attached by a runtime spawn (GameContext::spawn). Its
    // attachment_seq is runtimeSpawnAttachmentSeq's, and a scene load retires
    // it with the run it was spawned in.
    bool runtime_spawned = false;
};

// Whether a scene reconciliation carries the attachments runtime spawns made
// (an editor edit inside the same scene run) or retires them (a scene load,
// which ends the run).
enum class BehaviorSpawnedAttachments : std::uint8_t {
    carry,
    retire,
};

// GP-2: the runtime half of design_object_behaviors 4-2's attachment
// identity. A scene load packs (object declaration index, component index);
// an editor transaction reserves one from (commit_seq, command index,
// attachment index); a runtime spawn has no commit_seq, and its
// attachment_seq is fixed by
//   (the spawn's boundary in its scene run, its index among that boundary's
//    spawns, the behavior's index among the spawned object's behaviors)
// alone, so the same input gives the same attachment_seq in every run of the
// scene. The value is runtime_spawn_attachment_seq_base plus the tuple
// packed as 28 / 16 / 8 bits: above every scene-load value of a scene with
// fewer than 2^20 objects and every editor reservation, below 2^53 so the
// JSON wire carries it exactly, and ordered by the tuple.
inline constexpr std::uint64_t runtime_spawn_attachment_seq_base =
    std::uint64_t{1} << 52U;
inline constexpr std::uint64_t runtime_spawn_boundary_limit =
    std::uint64_t{1} << 28U;
inline constexpr std::uint64_t runtime_spawn_index_limit =
    std::uint64_t{1} << 16U;
inline constexpr std::uint64_t runtime_spawn_attachment_limit =
    std::uint64_t{1} << 8U;
// Throws std::length_error when a part is past its field.
std::uint64_t runtimeSpawnAttachmentSeq(std::uint64_t boundary,
                                        std::uint64_t spawn_index,
                                        std::uint64_t attachment_index);

struct BehaviorAttachmentIdentity {
    BehaviorAttachmentHandle handle = invalidBehaviorAttachmentHandle;
    std::uint64_t attachment_seq = 0;
};

struct BehaviorArenaIsolationState {
    std::uint64_t next_handle = 0;
    std::uint64_t next_attachment_seq = 0;
    std::size_t deferred_mutation_count = 0;
    std::size_t callback_depth = 0;
    internal::RegistrationOwner callback_owner = internal::engineRegistrationOwner;
};

enum class BehaviorAttachmentEditKind : std::uint8_t {
    attach,
    remove,
    set_params,
    insert_component,
    remove_component,
};

struct BehaviorAttachmentEdit {
    BehaviorAttachmentEditKind kind = BehaviorAttachmentEditKind::set_params;
    GameObjectId entity = invalidGameObjectId;
    std::optional<AuthoringObjectId> authoring_object_id;
    std::size_t component_index = 0;
    ResolvedComponentOrigin component_origin;
    std::size_t scene_object_index = 0;
    bool scene_ordered = false;
    BehaviorAttachmentIdentity identity;
    std::string stable_name;
    std::string canonical_params;
    nlohmann::json raw_component;
};

struct BehaviorAttachmentTransaction;

class PreparedBehaviorAttachmentEdits {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit PreparedBehaviorAttachmentEdits(std::unique_ptr<Impl> impl) noexcept;
    friend class BehaviorAttachmentArena;

  public:
    ~PreparedBehaviorAttachmentEdits();
    PreparedBehaviorAttachmentEdits(PreparedBehaviorAttachmentEdits &&) noexcept;
    PreparedBehaviorAttachmentEdits &operator=(PreparedBehaviorAttachmentEdits &&) noexcept;
    PreparedBehaviorAttachmentEdits(const PreparedBehaviorAttachmentEdits &) = delete;
    PreparedBehaviorAttachmentEdits &operator=(const PreparedBehaviorAttachmentEdits &) = delete;

    void publish() noexcept;
    void rollback() noexcept;
    void finish() noexcept;
};

class PreparedBehaviorSceneReconciliation {
    struct Impl;
    std::unique_ptr<Impl> impl_;

    explicit PreparedBehaviorSceneReconciliation(
        std::unique_ptr<Impl> impl) noexcept;
    friend class BehaviorAttachmentArena;

  public:
    ~PreparedBehaviorSceneReconciliation();
    PreparedBehaviorSceneReconciliation(
        PreparedBehaviorSceneReconciliation &&) noexcept;
    PreparedBehaviorSceneReconciliation &operator=(
        PreparedBehaviorSceneReconciliation &&) noexcept;
    PreparedBehaviorSceneReconciliation(
        const PreparedBehaviorSceneReconciliation &) = delete;
    PreparedBehaviorSceneReconciliation &operator=(
        const PreparedBehaviorSceneReconciliation &) = delete;

    void publish() noexcept;
    void publish(SceneLoadPhaseTrace &phase_trace) noexcept;
    void activate();
    void activate(SceneLoadPhaseTrace &phase_trace);
    void rollback() noexcept;
    void finish() noexcept;
    void finish(SceneLoadPhaseTrace &phase_trace) noexcept;
};

std::uint64_t sceneBehaviorAttachmentSeq(std::size_t object_index,
                                         std::size_t component_index);
std::vector<PreparedSceneBehaviorAttachment> prepareSceneBehaviorAttachments(
    const nlohmann::json &objects, BehaviorRegistryAvailability availability);
std::vector<PreparedSceneBehaviorAttachment>
prepareResolvedSceneBehaviorAttachments(
    std::span<const ResolvedObject> objects,
    BehaviorRegistryAvailability availability);

DECLARE_MODULE(BehaviorAttachmentArena) {
    friend class BehaviorCallbackScope;
    friend class PreparedBehaviorAttachmentEdits;
    friend class PreparedBehaviorSceneReconciliation;
    friend struct BehaviorAttachmentTransaction;

    struct Attachment {
        BehaviorAttachmentInfo info;
        nlohmann::json raw_component;
        internal::RawBehaviorInstance instance;
        internal::BehaviorDestroyFn destroy = nullptr;
        bool initialized = false;
    };

    struct DeferredMutation {
        internal::RegistrationOwner owner = internal::engineRegistrationOwner;
        std::optional<GameObjectId> remove_entity;
        std::function<void()> operation;
    };

    std::vector<Attachment> attachments;
    std::vector<DeferredMutation> deferred_mutations;
    std::uint64_t next_handle = 1;
    std::uint64_t next_attachment_seq = 1;
    std::size_t callback_depth = 0;
    internal::RegistrationOwner callback_owner = internal::engineRegistrationOwner;
    // PM-1b: the edit state's attachments are dormant (see setDormant).
    bool dormant_ = false;

    Attachment *findAttachment(BehaviorAttachmentHandle handle) noexcept;
    const Attachment *findAttachment(BehaviorAttachmentHandle handle) const noexcept;
    void invokeInit(Attachment &attachment);
    void invokeUpdate(Attachment &attachment, GameContext &ctx);
    void invokeEvent(Attachment &attachment, const internal::QueuedEvent &event,
                     GameContext &ctx);
    void destroyInstance(Attachment &attachment, bool invoke_destroy_callback) noexcept;
    void deactivateAllInstances() noexcept;
    void applyDeferredMutations();
    void activateReadyEditorAttachments();

  public:
    BehaviorAttachmentArena() = default;
    ~BehaviorAttachmentArena();

    // PM-1b (docs/design_play_mode.md section 5). While dormant -- the edit
    // state -- an attachment is made with its handle, attachment_seq, type,
    // owner and canonical params, and no instance: no constructor, no onInit,
    // no onUpdate, no event. Scene loads, editor attaches and scene
    // reconciliations make such attachments; set_params replaces the stored
    // params only; update() and dispatchEvent() are shut, so nothing wakes
    // one. A dormant attachment is not a pending one (pending is "no DLL"):
    // the editor treats it as the live target of its edits. Leaving dormancy
    // wakes nothing by itself; the next scene load (the play session's start)
    // makes instances and runs onInit. Either change drops the deferred object
    // mutations game code queued.
    void setDormant(bool dormant) noexcept;
    bool isDormant() const noexcept { return dormant_; }

    void publishSceneAttachments(std::vector<BoundSceneBehaviorAttachment> prepared);
    void activatePublished();
    void update(GameContext &ctx);
    void dispatchEvent(const internal::QueuedEvent &event, GameContext &ctx);

    void preDestroyEntity(GameObjectId entity) noexcept;
    void deactivateAll() noexcept;
    void deactivateAllForTeardown() noexcept;
    std::size_t drainDeferredMutationsForTeardown();
    void releaseOwner(internal::RegistrationOwner owner) noexcept;

    BehaviorAttachmentIdentity reserveRuntimeAttachmentIdentity(
        std::uint64_t commit_seq, std::size_t command_index,
        std::size_t attachment_index);
    std::optional<BehaviorAttachmentInfo> findEditorAttachment(
        GameObjectId entity,
        std::size_t authored_component_occurrence) const;
    std::unique_ptr<PreparedBehaviorAttachmentEdits>
    prepareEditorEdits(std::vector<BehaviorAttachmentEdit> edits,
                       BehaviorRegistryAvailability availability);
    std::unique_ptr<PreparedBehaviorSceneReconciliation>
    prepareSceneReconciliation(
        std::vector<BoundSceneBehaviorAttachment> desired,
        const OriginTransitionMap &origin_transition = {},
        BehaviorSpawnedAttachments spawned = BehaviorSpawnedAttachments::carry);

    // GP-2: attaches the behaviors of one runtime spawn to its entity, which
    // must already be live, and runs their onInit in attachment_seq order --
    // the activation barrier of a scene load, for one object. `prepared` is
    // the spawned object's behaviors in component order; the i-th gets
    // runtimeSpawnAttachmentSeq(boundary, spawn_index, i). When an onInit
    // throws, the ones already initialized get onDestroy (last first), every
    // attachment this call added is removed, deferred mutations queued by
    // those callbacks are dropped, and the exception propagates.
    void attachSpawned(GameObjectId entity,
                       std::vector<PreparedSceneBehaviorAttachment> prepared,
                       std::uint64_t boundary, std::uint64_t spawn_index);

    void deferCreateObject(LocalTransformComponent transform);
    void deferCreateSpriteObject(LocalTransformComponent transform,
                                 SpriteViewComponent sprite);
    bool deferRemoveObject(GameObjectId entity);
    bool callbacksActive() const noexcept { return callback_depth != 0; }
    std::size_t deferredMutationCountForTesting() const noexcept {
        return deferred_mutations.size();
    }

    std::vector<BehaviorAttachmentInfo> snapshot() const;
    BehaviorArenaIsolationState isolationState() const noexcept {
        return {
            .next_handle = next_handle,
            .next_attachment_seq = next_attachment_seq,
            .deferred_mutation_count = deferred_mutations.size(),
            .callback_depth = callback_depth,
            .callback_owner = callback_owner,
        };
    }
    std::size_t liveInstanceCount(internal::RegistrationOwner owner) const noexcept;
};

namespace internal {

enum class BehaviorObjectRemovalRoute {
    proceed,
    deferred,
};

BehaviorObjectRemovalRoute routeBehaviorObjectRemoval(GameObjectId entity);
void preDestroyAllBehaviorObjects() noexcept;
void preDestroyAllBehaviorObjectsForTeardown() noexcept;
std::size_t drainDeferredBehaviorMutationsForTeardown();
bool behaviorCallbackActive() noexcept;
void releaseBehaviorOwner(RegistrationOwner owner) noexcept;

} // namespace internal

} // namespace Pelican
