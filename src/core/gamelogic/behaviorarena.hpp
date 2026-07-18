#pragma once

#include "../container.hpp"
#include "../userpublic/behavior.hpp"
#include "../userpublic/details/behavior/registerer.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
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
    std::string stable_name;
    std::string canonical_params;
    internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    bool pending = false;
    bool active = false;
};

std::uint64_t sceneBehaviorAttachmentSeq(std::size_t object_index,
                                         std::size_t component_index);
std::vector<PreparedSceneBehaviorAttachment> prepareSceneBehaviorAttachments(
    const nlohmann::json &objects, BehaviorRegistryAvailability availability);

DECLARE_MODULE(BehaviorAttachmentArena) {
    friend class BehaviorCallbackScope;

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
    std::size_t callback_depth = 0;
    internal::RegistrationOwner callback_owner = internal::engineRegistrationOwner;

    Attachment *findAttachment(BehaviorAttachmentHandle handle) noexcept;
    const Attachment *findAttachment(BehaviorAttachmentHandle handle) const noexcept;
    void invokeInit(Attachment &attachment);
    void invokeUpdate(Attachment &attachment, GameContext &ctx);
    void invokeEvent(Attachment &attachment, const internal::QueuedEvent &event,
                     GameContext &ctx);
    void destroyInstance(Attachment &attachment, bool invoke_destroy_callback) noexcept;
    void applyDeferredMutations();

  public:
    BehaviorAttachmentArena() = default;
    ~BehaviorAttachmentArena();

    void publishSceneAttachments(std::vector<BoundSceneBehaviorAttachment> prepared);
    void activatePublished();
    void update(GameContext &ctx);
    void dispatchEvent(const internal::QueuedEvent &event, GameContext &ctx);

    void preDestroyEntity(GameObjectId entity) noexcept;
    void deactivateAll() noexcept;
    void releaseOwner(internal::RegistrationOwner owner) noexcept;

    void deferCreateObject(LocalTransformComponent transform);
    void deferCreateSpriteObject(LocalTransformComponent transform,
                                 SpriteViewComponent sprite);
    bool deferRemoveObject(GameObjectId entity);
    bool callbacksActive() const noexcept { return callback_depth != 0; }

    std::vector<BehaviorAttachmentInfo> snapshot() const;
    std::size_t liveInstanceCount(internal::RegistrationOwner owner) const noexcept;
};

namespace internal {

enum class BehaviorObjectRemovalRoute {
    proceed,
    deferred,
};

BehaviorObjectRemovalRoute routeBehaviorObjectRemoval(GameObjectId entity);
void preDestroyAllBehaviorObjects() noexcept;
bool behaviorCallbackActive() noexcept;
void releaseBehaviorOwner(RegistrationOwner owner) noexcept;

} // namespace internal

} // namespace Pelican
