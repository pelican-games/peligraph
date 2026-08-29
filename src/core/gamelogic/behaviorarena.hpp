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
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

class BehaviorCallbackScope;

enum class BehaviorRegistryAvailability {
    active,
    dll_unavailable,
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
};

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
    std::size_t component_index = 0;
    ResolvedComponentOrigin component_origin;
    std::size_t scene_object_index = 0;
    bool scene_ordered = false;
    BehaviorAttachmentIdentity identity;
    std::string stable_name;
    std::string canonical_params;
    nlohmann::json raw_component;
};

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

    struct Attachment {
        BehaviorAttachmentInfo info;
        nlohmann::json raw_component;
        internal::RawBehaviorInstance instance;
        internal::BehaviorDestroyFn destroy = nullptr;
        bool initialized = false;
        std::uint8_t activation_delay = 0;
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
        GameObjectId entity, std::size_t component_index) const;
    std::unique_ptr<PreparedBehaviorAttachmentEdits>
    prepareEditorEdits(std::vector<BehaviorAttachmentEdit> edits);

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
