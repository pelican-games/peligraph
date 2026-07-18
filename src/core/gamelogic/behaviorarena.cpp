#include "behaviorarena.hpp"

#include "../ecs/core.hpp"
#include "../log.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../userpublic/gameobjects.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Pelican {

class BehaviorCallbackScope {
    BehaviorAttachmentArena &arena;
    internal::RegistrationOwner previous_owner;
    internal::ScopedRegistrationOwner owner_scope;

  public:
    BehaviorCallbackScope(BehaviorAttachmentArena &value, internal::RegistrationOwner owner)
        : arena(value), previous_owner(arena.callback_owner), owner_scope(owner) {
        ++arena.callback_depth;
        arena.callback_owner = owner;
    }

    ~BehaviorCallbackScope() {
        arena.callback_owner = previous_owner;
        --arena.callback_depth;
    }
};

namespace {

struct BehaviorSystem {
    void update(GameContext &ctx) {
        GET_MODULE(BehaviorAttachmentArena).update(ctx);
    }

    void dispatchQueuedEvent(const internal::QueuedEvent &event, GameContext &ctx) {
        GET_MODULE(BehaviorAttachmentArena).dispatchEvent(event, ctx);
    }
};

struct BehaviorSystemRegistration {
    BehaviorSystemRegistration() {
        internal::getGameSystemRegisterer().registerSystem<BehaviorSystem>(
            std::string{behaviorSystemName}, behaviorSystemOrder, {});
    }
};

const BehaviorSystemRegistration behavior_system_registration;

} // namespace

std::uint64_t sceneBehaviorAttachmentSeq(std::size_t object_index,
                                         std::size_t component_index) {
    constexpr auto max_part = std::numeric_limits<std::uint32_t>::max();
    if (object_index > max_part || component_index > max_part) {
        throw std::length_error("scene behavior attachment tuple exceeds 32-bit sequence fields");
    }
    return (static_cast<std::uint64_t>(object_index) << 32u) |
           static_cast<std::uint64_t>(component_index);
}

std::vector<PreparedSceneBehaviorAttachment> prepareSceneBehaviorAttachments(
    const nlohmann::json &objects, BehaviorRegistryAvailability availability) {
    if (!objects.is_array()) {
        throw std::runtime_error("scene objects must be an array before behavior lowering");
    }

    std::vector<PreparedSceneBehaviorAttachment> prepared;
    const auto &registry = internal::getBehaviorRegisterer();
    for (std::size_t object_index = 0; object_index < objects.size(); ++object_index) {
        const auto &object = objects.at(object_index);
        const auto object_name = object.value("name", std::string{});
        const auto &components = object.at("components");
        for (std::size_t component_index = 0; component_index < components.size();
             ++component_index) {
            const auto &component = components.at(component_index);
            if (component.at("name").get<std::string>() != "behavior") {
                continue;
            }
            const auto type_it = component.find("type");
            if (type_it == component.end() || !type_it->is_string() ||
                type_it->get_ref<const std::string &>().empty()) {
                throw std::runtime_error("behavior on object '" +
                                         (object_name.empty() ? std::string{"<unnamed>"}
                                                              : object_name) +
                                         "' requires a non-empty string type");
            }
            const auto stable_name = type_it->get<std::string>();
            const auto *registration = registry.findByName(stable_name);
            if (registration == nullptr) {
                if (availability == BehaviorRegistryAvailability::active) {
                    throw std::runtime_error("Unknown behavior type '" + stable_name +
                                             "' on object '" +
                                             (object_name.empty() ? std::string{"<unnamed>"}
                                                                  : object_name) +
                                             "'");
                }
                if (logger != nullptr) {
                    LOG_WARNING(logger,
                                "behavior type '{}' on object '{}' is pending because the game-logic DLL is unavailable",
                                stable_name,
                                object_name.empty() ? std::string{"<unnamed>"} : object_name);
                }
                prepared.push_back(PreparedSceneBehaviorAttachment{
                    .object_index = object_index,
                    .component_index = component_index,
                    .object_name = object_name,
                    .stable_name = stable_name,
                    .canonical_params = {},
                    .raw_component = component,
                    .registration_owner = internal::engineRegistrationOwner,
                    .pending = true,
                });
                continue;
            }

            const auto params_it = component.find("params");
            const nlohmann::json empty_params = nlohmann::json::object();
            const auto &params = params_it == component.end() ? empty_params : *params_it;
            prepared.push_back(PreparedSceneBehaviorAttachment{
                .object_index = object_index,
                .component_index = component_index,
                .object_name = object_name,
                .stable_name = stable_name,
                .canonical_params = registration->canonicalize_params(params),
                .raw_component = component,
                .registration_owner = registration->owner,
                .pending = false,
            });
        }
    }
    return prepared;
}

BehaviorContext::BehaviorContext(GameObjectId self, BehaviorAttachmentHandle attachment,
                                 std::uint64_t sequence, void *params,
                                 const std::type_info *params_type_info) noexcept
    : self_id(self), attachment_handle(attachment), attachment_sequence(sequence),
      params_value(params), params_type(params_type_info) {}

GameObjectId BehaviorContext::createObject(const LocalTransformComponent &transform) const {
    GET_MODULE(BehaviorAttachmentArena).deferCreateObject(transform);
    return invalidGameObjectId;
}

GameObjectId BehaviorContext::createSpriteObject(const LocalTransformComponent &transform,
                                                 const SpriteViewComponent &sprite) const {
    GET_MODULE(BehaviorAttachmentArena).deferCreateSpriteObject(transform, sprite);
    return invalidGameObjectId;
}

bool BehaviorContext::removeObject(GameObjectId id) const {
    return GameObjects::remove(id);
}

BehaviorAttachmentArena::Attachment *BehaviorAttachmentArena::findAttachment(
    BehaviorAttachmentHandle handle) noexcept {
    const auto found = std::find_if(attachments.begin(), attachments.end(),
                                    [handle](const Attachment &attachment) {
                                        return attachment.info.handle == handle;
                                    });
    return found == attachments.end() ? nullptr : &*found;
}

const BehaviorAttachmentArena::Attachment *BehaviorAttachmentArena::findAttachment(
    BehaviorAttachmentHandle handle) const noexcept {
    const auto found = std::find_if(attachments.begin(), attachments.end(),
                                    [handle](const Attachment &attachment) {
                                        return attachment.info.handle == handle;
                                    });
    return found == attachments.end() ? nullptr : &*found;
}

BehaviorAttachmentArena::~BehaviorAttachmentArena() {
    deactivateAll();
}

void BehaviorAttachmentArena::publishSceneAttachments(
    std::vector<BoundSceneBehaviorAttachment> prepared) {
    if (!attachments.empty()) {
        throw std::logic_error("behavior scene attachments must be deactivated before publication");
    }
    std::vector<Attachment> next;
    next.reserve(prepared.size());
    for (auto &bound : prepared) {
        if (bound.entity == invalidGameObjectId) {
            throw std::logic_error("behavior attachment publication requires a live entity");
        }
        if (next_handle == 0) {
            throw std::overflow_error("BehaviorAttachmentHandle space exhausted");
        }
        const auto handle = BehaviorAttachmentHandle{next_handle++};
        const auto sequence = sceneBehaviorAttachmentSeq(bound.prepared.object_index,
                                                         bound.prepared.component_index);
        next.push_back(Attachment{
            .info = BehaviorAttachmentInfo{
                .handle = handle,
                .attachment_seq = sequence,
                .entity = bound.entity,
                .stable_name = bound.prepared.stable_name,
                .canonical_params = bound.prepared.canonical_params,
                .owner = bound.prepared.registration_owner,
                .pending = bound.prepared.pending,
                .active = false,
            },
            .raw_component = std::move(bound.prepared.raw_component),
        });
    }
    std::sort(next.begin(), next.end(), [](const Attachment &left, const Attachment &right) {
        return left.info.attachment_seq < right.info.attachment_seq;
    });
    attachments.swap(next);
}

void BehaviorAttachmentArena::invokeInit(Attachment &attachment) {
    BehaviorContext ctx{attachment.info.entity, attachment.info.handle,
                        attachment.info.attachment_seq, attachment.instance.params,
                        attachment.instance.params_type};
    BehaviorCallbackScope callback{*this, attachment.info.owner};
    attachment.instance.behavior->onInit(ctx);
}

void BehaviorAttachmentArena::invokeUpdate(Attachment &attachment, GameContext &) {
    BehaviorContext ctx{attachment.info.entity, attachment.info.handle,
                        attachment.info.attachment_seq, attachment.instance.params,
                        attachment.instance.params_type};
    BehaviorCallbackScope callback{*this, attachment.info.owner};
    attachment.instance.behavior->onUpdate(ctx);
}

void BehaviorAttachmentArena::invokeEvent(Attachment &attachment,
                                          const internal::QueuedEvent &event,
                                          GameContext &) {
    const auto *registration = internal::getBehaviorRegisterer().findByNameAndOwner(
        attachment.info.stable_name, attachment.info.owner);
    if (registration == nullptr) {
        throw std::logic_error("active behavior registration disappeared before event dispatch");
    }
    const auto handler = std::find_if(
        registration->event_handlers.begin(), registration->event_handlers.end(),
        [&](const internal::BehaviorEventHandlerRegistration &candidate) {
            return candidate.event_type == event.type;
        });
    if (handler == registration->event_handlers.end()) {
        return;
    }
    BehaviorContext ctx{attachment.info.entity, attachment.info.handle,
                        attachment.info.attachment_seq, attachment.instance.params,
                        attachment.instance.params_type};
    BehaviorCallbackScope callback{*this, attachment.info.owner};
    handler->dispatch(*attachment.instance.behavior, event.payload.get(), ctx);
}

void BehaviorAttachmentArena::destroyInstance(Attachment &attachment,
                                              bool invoke_destroy_callback) noexcept {
    if (attachment.instance.behavior == nullptr) {
        attachment.initialized = false;
        attachment.info.active = false;
        return;
    }
    if (invoke_destroy_callback && attachment.initialized) {
        BehaviorContext ctx{attachment.info.entity, attachment.info.handle,
                            attachment.info.attachment_seq, attachment.instance.params,
                            attachment.instance.params_type};
        try {
            BehaviorCallbackScope callback{*this, attachment.info.owner};
            attachment.instance.behavior->onDestroy(ctx);
        } catch (...) {
            if (logger != nullptr) {
                LOG_ERROR(logger, "noexcept behavior onDestroy escaped for type '{}'",
                          attachment.info.stable_name);
            }
        }
    }
    if (attachment.destroy != nullptr) {
        attachment.destroy(attachment.instance);
    }
    attachment.initialized = false;
    attachment.info.active = false;
    attachment.destroy = nullptr;
}

void BehaviorAttachmentArena::activatePublished() {
    const auto deferred_base = deferred_mutations.size();
    std::vector<std::size_t> activated;
    activated.reserve(attachments.size());
    try {
        for (std::size_t index = 0; index < attachments.size(); ++index) {
            auto &attachment = attachments[index];
            if (attachment.info.pending) {
                continue;
            }
            const auto *registration = internal::getBehaviorRegisterer().findByNameAndOwner(
                attachment.info.stable_name, attachment.info.owner);
            if (registration == nullptr) {
                throw std::runtime_error("behavior registration disappeared before activation: " +
                                         attachment.info.stable_name);
            }
            attachment.destroy = registration->destroy;
            attachment.instance = registration->create(attachment.info.canonical_params);
            try {
                invokeInit(attachment);
            } catch (...) {
                destroyInstance(attachment, false);
                throw;
            }
            attachment.initialized = true;
            attachment.info.active = true;
            activated.push_back(index);
        }
    } catch (...) {
        for (auto it = activated.rbegin(); it != activated.rend(); ++it) {
            destroyInstance(attachments[*it], true);
        }
        deferred_mutations.resize(deferred_base);
        attachments.clear();
        throw;
    }
}

void BehaviorAttachmentArena::applyDeferredMutations() {
    if (callback_depth != 0 || deferred_mutations.empty()) {
        return;
    }
    std::vector<DeferredMutation> pending;
    pending.swap(deferred_mutations);
    for (auto &mutation : pending) {
        mutation.operation();
    }
}

void BehaviorAttachmentArena::update(GameContext &ctx) {
    applyDeferredMutations();
    std::vector<BehaviorAttachmentHandle> snapshot_handles;
    snapshot_handles.reserve(attachments.size());
    for (const auto &attachment : attachments) {
        if (attachment.info.active) {
            snapshot_handles.push_back(attachment.info.handle);
        }
    }
    for (const auto handle : snapshot_handles) {
        auto *attachment = findAttachment(handle);
        if (attachment == nullptr || !attachment->info.active) {
            continue;
        }
        const auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
        if (!ecs.isAlive(attachment->info.entity)) {
            continue;
        }
        invokeUpdate(*attachment, ctx);
    }
}

void BehaviorAttachmentArena::dispatchEvent(const internal::QueuedEvent &event,
                                            GameContext &ctx) {
    applyDeferredMutations();
    std::vector<BehaviorAttachmentHandle> snapshot_handles;
    snapshot_handles.reserve(attachments.size());
    for (const auto &attachment : attachments) {
        if (attachment.info.active) {
            snapshot_handles.push_back(attachment.info.handle);
        }
    }
    for (const auto handle : snapshot_handles) {
        auto *attachment = findAttachment(handle);
        if (attachment == nullptr || !attachment->info.active) {
            continue;
        }
        if (!GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(attachment->info.entity)) {
            continue;
        }
        invokeEvent(*attachment, event, ctx);
    }
}

void BehaviorAttachmentArena::preDestroyEntity(GameObjectId entity) noexcept {
    for (auto it = attachments.rbegin(); it != attachments.rend(); ++it) {
        if (it->info.entity == entity) {
            destroyInstance(*it, true);
        }
    }
    std::erase_if(attachments,
                  [entity](const Attachment &attachment) { return attachment.info.entity == entity; });
}

void BehaviorAttachmentArena::deactivateAll() noexcept {
    for (auto it = attachments.rbegin(); it != attachments.rend(); ++it) {
        destroyInstance(*it, true);
    }
    attachments.clear();
    deferred_mutations.clear();
}

void BehaviorAttachmentArena::releaseOwner(internal::RegistrationOwner owner) noexcept {
    for (auto it = attachments.rbegin(); it != attachments.rend(); ++it) {
        if (it->info.owner == owner) {
            destroyInstance(*it, true);
        }
    }
    std::erase_if(attachments, [owner](const Attachment &attachment) {
        return attachment.info.owner == owner;
    });
    std::erase_if(deferred_mutations, [owner](const DeferredMutation &mutation) {
        return mutation.owner == owner;
    });
}

void BehaviorAttachmentArena::deferCreateObject(LocalTransformComponent transform) {
    const auto owner = callback_owner;
    deferred_mutations.push_back(DeferredMutation{
        .owner = owner,
        .operation = [transform] {
            GameContext ctx;
            (void)ctx.createObject(transform);
        },
    });
}

void BehaviorAttachmentArena::deferCreateSpriteObject(LocalTransformComponent transform,
                                                      SpriteViewComponent sprite) {
    sprite.validate();
    const auto owner = callback_owner;
    deferred_mutations.push_back(DeferredMutation{
        .owner = owner,
        .operation = [transform, sprite = std::move(sprite)] {
            GameContext ctx;
            (void)ctx.createSpriteObject(transform, sprite);
        },
    });
}

bool BehaviorAttachmentArena::deferRemoveObject(GameObjectId entity) {
    const auto duplicate = std::find_if(
        deferred_mutations.begin(), deferred_mutations.end(),
        [entity](const DeferredMutation &mutation) {
            return mutation.remove_entity.has_value() && *mutation.remove_entity == entity;
        });
    if (duplicate != deferred_mutations.end()) {
        return false;
    }
    const auto owner = callback_owner;
    deferred_mutations.push_back(DeferredMutation{
        .owner = owner,
        .remove_entity = entity,
        .operation = [entity] { (void)GameObjects::remove(entity); },
    });
    return true;
}

std::vector<BehaviorAttachmentInfo> BehaviorAttachmentArena::snapshot() const {
    std::vector<BehaviorAttachmentInfo> result;
    result.reserve(attachments.size());
    for (const auto &attachment : attachments) {
        result.push_back(attachment.info);
    }
    return result;
}

std::size_t BehaviorAttachmentArena::liveInstanceCount(
    internal::RegistrationOwner owner) const noexcept {
    return static_cast<std::size_t>(std::count_if(
        attachments.begin(), attachments.end(), [owner](const Attachment &attachment) {
            return attachment.info.owner == owner && attachment.info.active;
        }));
}

namespace internal {

BehaviorObjectRemovalRoute routeBehaviorObjectRemoval(GameObjectId entity) {
    auto *arena = FastModuleContainer::tryGet<BehaviorAttachmentArena>();
    if (arena == nullptr) {
        return BehaviorObjectRemovalRoute::proceed;
    }
    if (arena->callbacksActive()) {
        (void)arena->deferRemoveObject(entity);
        return BehaviorObjectRemovalRoute::deferred;
    }
    arena->preDestroyEntity(entity);
    return BehaviorObjectRemovalRoute::proceed;
}

void preDestroyAllBehaviorObjects() noexcept {
    if (auto *arena = FastModuleContainer::tryGet<BehaviorAttachmentArena>()) {
        arena->deactivateAll();
    }
}

bool behaviorCallbackActive() noexcept {
    const auto *arena = FastModuleContainer::tryGet<BehaviorAttachmentArena>();
    return arena != nullptr && arena->callbacksActive();
}

void releaseBehaviorOwner(RegistrationOwner owner) noexcept {
    if (auto *arena = FastModuleContainer::tryGet<BehaviorAttachmentArena>()) {
        arena->releaseOwner(owner);
    }
}

} // namespace internal

} // namespace Pelican
