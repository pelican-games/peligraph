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

struct PreparedBehaviorAttachmentEdits::Impl {
    struct PreparedItem {
        BehaviorAttachmentEdit edit;
        std::optional<BehaviorAttachmentArena::Attachment> attachment;
        internal::RawBehaviorParams params;
        internal::BehaviorApplyPreparedParamsFn apply_params = nullptr;
        internal::BehaviorDestroyPreparedParamsFn destroy_params = nullptr;
    };

    BehaviorAttachmentArena *arena = nullptr;
    std::vector<PreparedItem> items;
    bool published = false;

    void releasePreparedParams() noexcept {
        for (auto &item : items) {
            if (item.params.value != nullptr && item.destroy_params != nullptr) {
                item.destroy_params(item.params);
            }
        }
    }

    void releasePreparedAttachments() noexcept {
        for (auto &item : items) {
            if (!item.attachment || item.attachment->instance.behavior == nullptr ||
                item.attachment->destroy == nullptr) {
                continue;
            }
            item.attachment->destroy(item.attachment->instance);
            item.attachment->destroy = nullptr;
        }
    }

    ~Impl() {
        releasePreparedParams();
        releasePreparedAttachments();
    }
};

PreparedBehaviorAttachmentEdits::PreparedBehaviorAttachmentEdits(
    std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

PreparedBehaviorAttachmentEdits::~PreparedBehaviorAttachmentEdits() = default;
PreparedBehaviorAttachmentEdits::PreparedBehaviorAttachmentEdits(
    PreparedBehaviorAttachmentEdits &&) noexcept = default;
PreparedBehaviorAttachmentEdits &PreparedBehaviorAttachmentEdits::operator=(
    PreparedBehaviorAttachmentEdits &&) noexcept = default;

void PreparedBehaviorAttachmentEdits::publish() noexcept {
    if (!impl_ || impl_->published) return;
    auto &arena = *impl_->arena;
    const auto find = [&](const BehaviorAttachmentEdit &edit) {
        return std::find_if(
            arena.attachments.begin(), arena.attachments.end(),
            [&](const BehaviorAttachmentArena::Attachment &attachment) {
                if (edit.identity.handle != invalidBehaviorAttachmentHandle) {
                    return attachment.info.handle == edit.identity.handle;
                }
                return attachment.info.entity == edit.entity &&
                       attachment.info.component_index == edit.component_index;
            });
    };
    for (auto &item : impl_->items) {
        auto &edit = item.edit;
        switch (edit.kind) {
        case BehaviorAttachmentEditKind::attach:
            for (auto &attachment : arena.attachments) {
                if (attachment.info.entity == edit.entity &&
                    attachment.info.component_index >= edit.component_index) {
                    ++attachment.info.component_index;
                }
            }
            arena.attachments.push_back(std::move(*item.attachment));
            item.attachment.reset();
            break;
        case BehaviorAttachmentEditKind::remove: {
            const auto found = find(edit);
            if (found != arena.attachments.end()) {
                arena.destroyInstance(*found, true);
                arena.attachments.erase(found);
            }
            for (auto &attachment : arena.attachments) {
                if (attachment.info.entity == edit.entity &&
                    attachment.info.component_index > edit.component_index) {
                    --attachment.info.component_index;
                }
            }
            break;
        }
        case BehaviorAttachmentEditKind::set_params: {
            const auto found = find(edit);
            if (found != arena.attachments.end()) {
                if (found->instance.params != nullptr && item.params.value != nullptr) {
                    item.apply_params(found->instance.params, item.params);
                }
                found->info.canonical_params = std::move(edit.canonical_params);
                found->raw_component = std::move(edit.raw_component);
            }
            break;
        }
        case BehaviorAttachmentEditKind::insert_component:
            for (auto &attachment : arena.attachments) {
                if (attachment.info.entity == edit.entity &&
                    attachment.info.component_index >= edit.component_index) {
                    ++attachment.info.component_index;
                }
            }
            break;
        case BehaviorAttachmentEditKind::remove_component:
            for (auto &attachment : arena.attachments) {
                if (attachment.info.entity == edit.entity &&
                    attachment.info.component_index > edit.component_index) {
                    --attachment.info.component_index;
                }
            }
            break;
        }
    }
    std::sort(arena.attachments.begin(), arena.attachments.end(),
              [](const auto &left, const auto &right) {
                  return left.info.attachment_seq < right.info.attachment_seq;
              });
    impl_->published = true;
}

void PreparedBehaviorAttachmentEdits::rollback() noexcept {
    if (!impl_ || impl_->published) return;
    impl_->releasePreparedParams();
    impl_->releasePreparedAttachments();
}

void PreparedBehaviorAttachmentEdits::finish() noexcept {
    if (!impl_) return;
    impl_->releasePreparedParams();
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
                .component_index = bound.prepared.component_index,
                .stable_name = bound.prepared.stable_name,
                .canonical_params = bound.prepared.canonical_params,
                .owner = bound.prepared.registration_owner,
                .pending = bound.prepared.pending,
                .active = false,
            },
            .raw_component = std::move(bound.prepared.raw_component),
        });
        if (sequence == std::numeric_limits<std::uint64_t>::max()) {
            next_attachment_seq = 0;
        } else {
            next_attachment_seq = std::max(next_attachment_seq, sequence + 1U);
        }
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

void BehaviorAttachmentArena::activateReadyEditorAttachments() {
    for (auto &attachment : attachments) {
        if (attachment.info.pending || attachment.info.active ||
            attachment.activation_delay != 0) {
            continue;
        }
        try {
            if (attachment.instance.behavior == nullptr) {
                const auto *registration =
                    internal::getBehaviorRegisterer().findByNameAndOwner(
                        attachment.info.stable_name, attachment.info.owner);
                if (registration == nullptr) {
                    throw std::runtime_error(
                        "behavior registration disappeared before editor activation: " +
                        attachment.info.stable_name);
                }
                attachment.destroy = registration->destroy;
                attachment.instance =
                    registration->create(attachment.info.canonical_params);
            }
            invokeInit(attachment);
            attachment.initialized = true;
            attachment.info.active = true;
        } catch (const std::exception &error) {
            destroyInstance(attachment, false);
            if (logger != nullptr) {
                LOG_ERROR(logger, "editor behavior activation failed for type '{}': {}",
                          attachment.info.stable_name, error.what());
            }
        } catch (...) {
            destroyInstance(attachment, false);
            if (logger != nullptr) {
                LOG_ERROR(logger, "editor behavior activation failed for type '{}'",
                          attachment.info.stable_name);
            }
        }
    }
}

void BehaviorAttachmentArena::update(GameContext &ctx) {
    applyDeferredMutations();
    activateReadyEditorAttachments();
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
    for (auto &attachment : attachments) {
        if (!attachment.info.active && attachment.activation_delay != 0) {
            --attachment.activation_delay;
        }
    }
}

void BehaviorAttachmentArena::dispatchEvent(const internal::QueuedEvent &event,
                                            GameContext &ctx) {
    applyDeferredMutations();
    activateReadyEditorAttachments();
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

BehaviorAttachmentIdentity BehaviorAttachmentArena::reserveRuntimeAttachmentIdentity(
    std::uint64_t, std::size_t, std::size_t) {
    constexpr auto max_exact_json_integer = UINT64_C(9007199254740991);
    if (next_handle == 0 || next_handle > max_exact_json_integer) {
        throw std::overflow_error("BehaviorAttachmentHandle space exhausted");
    }
    if (next_attachment_seq == 0 ||
        next_attachment_seq > max_exact_json_integer) {
        throw std::overflow_error("runtime behavior attachment sequence space exhausted");
    }
    return BehaviorAttachmentIdentity{
        .handle = BehaviorAttachmentHandle{next_handle++},
        .attachment_seq = next_attachment_seq++,
    };
}

std::optional<BehaviorAttachmentInfo> BehaviorAttachmentArena::findEditorAttachment(
    GameObjectId entity, std::size_t component_index) const {
    const auto found = std::find_if(
        attachments.begin(), attachments.end(), [&](const Attachment &attachment) {
            return attachment.info.entity == entity &&
                   attachment.info.component_index == component_index;
        });
    return found == attachments.end() ? std::nullopt
                                      : std::optional{found->info};
}

std::unique_ptr<PreparedBehaviorAttachmentEdits>
BehaviorAttachmentArena::prepareEditorEdits(
    std::vector<BehaviorAttachmentEdit> edits) {
    struct SimulatedAttachment {
        BehaviorAttachmentHandle handle = invalidBehaviorAttachmentHandle;
        std::uint64_t attachment_seq = 0;
        GameObjectId entity = invalidGameObjectId;
        std::size_t component_index = 0;
        std::string stable_name;
        internal::RegistrationOwner owner = internal::engineRegistrationOwner;
        bool pending = false;
    };
    std::vector<SimulatedAttachment> simulated;
    simulated.reserve(attachments.size() + edits.size());
    for (const auto &attachment : attachments) {
        simulated.push_back({
            .handle = attachment.info.handle,
            .attachment_seq = attachment.info.attachment_seq,
            .entity = attachment.info.entity,
            .component_index = attachment.info.component_index,
            .stable_name = attachment.info.stable_name,
            .owner = attachment.info.owner,
            .pending = attachment.info.pending,
        });
    }

    auto impl = std::make_unique<PreparedBehaviorAttachmentEdits::Impl>();
    impl->arena = this;
    impl->items.reserve(edits.size());
    const auto find_simulated = [&](const BehaviorAttachmentEdit &edit) {
        return std::find_if(
            simulated.begin(), simulated.end(), [&](const auto &attachment) {
                if (edit.identity.handle != invalidBehaviorAttachmentHandle) {
                    return attachment.handle == edit.identity.handle;
                }
                return attachment.entity == edit.entity &&
                       attachment.component_index == edit.component_index;
            });
    };

    std::size_t attach_count = 0;
    for (auto &edit : edits) {
        PreparedBehaviorAttachmentEdits::Impl::PreparedItem item;
        item.edit = std::move(edit);
        auto &current = item.edit;
        if (current.entity == invalidGameObjectId) {
            throw std::runtime_error("behavior editor attachment requires a live entity");
        }
        switch (current.kind) {
        case BehaviorAttachmentEditKind::attach: {
            if (current.identity.handle == invalidBehaviorAttachmentHandle) {
                throw std::runtime_error("behavior editor attach requires a reserved identity");
            }
            if (std::any_of(simulated.begin(), simulated.end(), [&](const auto &candidate) {
                    return candidate.handle == current.identity.handle ||
                           candidate.attachment_seq == current.identity.attachment_seq;
                })) {
                throw std::runtime_error("behavior editor attach identity is already live");
            }
            const auto *registration =
                internal::getBehaviorRegisterer().findByName(current.stable_name);
            if (registration == nullptr) {
                throw std::runtime_error("behavior registration unavailable: " +
                                         current.stable_name);
            }
            auto instance = registration->create(current.canonical_params);
            item.attachment.emplace(Attachment{
                .info = BehaviorAttachmentInfo{
                    .handle = current.identity.handle,
                    .attachment_seq = current.identity.attachment_seq,
                    .entity = current.entity,
                    .component_index = current.component_index,
                    .stable_name = current.stable_name,
                    .canonical_params = current.canonical_params,
                    .owner = registration->owner,
                    .pending = false,
                    .active = false,
                },
                .raw_component = current.raw_component,
                .instance = instance,
                .destroy = registration->destroy,
                .initialized = false,
                .activation_delay = 1,
            });
            for (auto &candidate : simulated) {
                if (candidate.entity == current.entity &&
                    candidate.component_index >= current.component_index) {
                    ++candidate.component_index;
                }
            }
            simulated.push_back({
                .handle = current.identity.handle,
                .attachment_seq = current.identity.attachment_seq,
                .entity = current.entity,
                .component_index = current.component_index,
                .stable_name = current.stable_name,
                .owner = registration->owner,
            });
            ++attach_count;
            break;
        }
        case BehaviorAttachmentEditKind::remove: {
            const auto found = find_simulated(current);
            if (found == simulated.end() || found->entity != current.entity ||
                found->component_index != current.component_index ||
                (current.identity.attachment_seq != 0 &&
                 found->attachment_seq != current.identity.attachment_seq)) {
                throw std::runtime_error("behavior editor remove target is stale");
            }
            simulated.erase(found);
            for (auto &candidate : simulated) {
                if (candidate.entity == current.entity &&
                    candidate.component_index > current.component_index) {
                    --candidate.component_index;
                }
            }
            break;
        }
        case BehaviorAttachmentEditKind::set_params: {
            const auto found = find_simulated(current);
            if (found == simulated.end() || found->entity != current.entity ||
                found->component_index != current.component_index || found->pending ||
                (current.identity.attachment_seq != 0 &&
                 found->attachment_seq != current.identity.attachment_seq)) {
                throw std::runtime_error("behavior editor params target is stale or pending");
            }
            const auto *registration =
                internal::getBehaviorRegisterer().findByNameAndOwner(
                    found->stable_name, found->owner);
            if (registration == nullptr) {
                throw std::runtime_error("behavior registration owner changed before params apply");
            }
            item.params = registration->prepare_params(current.canonical_params);
            item.apply_params = registration->apply_prepared_params;
            item.destroy_params = registration->destroy_prepared_params;
            break;
        }
        case BehaviorAttachmentEditKind::insert_component:
            for (auto &candidate : simulated) {
                if (candidate.entity == current.entity &&
                    candidate.component_index >= current.component_index) {
                    ++candidate.component_index;
                }
            }
            break;
        case BehaviorAttachmentEditKind::remove_component:
            for (auto &candidate : simulated) {
                if (candidate.entity == current.entity &&
                    candidate.component_index > current.component_index) {
                    --candidate.component_index;
                }
            }
            break;
        }
        impl->items.push_back(std::move(item));
    }
    attachments.reserve(attachments.size() + attach_count);
    return std::unique_ptr<PreparedBehaviorAttachmentEdits>{
        new PreparedBehaviorAttachmentEdits{std::move(impl)}};
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

void BehaviorAttachmentArena::deactivateAllInstances() noexcept {
    for (auto it = attachments.rbegin(); it != attachments.rend(); ++it) {
        destroyInstance(*it, true);
    }
    attachments.clear();
}

void BehaviorAttachmentArena::deactivateAll() noexcept {
    deactivateAllInstances();
    deferred_mutations.clear();
}

void BehaviorAttachmentArena::deactivateAllForTeardown() noexcept {
    deactivateAllInstances();
}

std::size_t BehaviorAttachmentArena::drainDeferredMutationsForTeardown() {
    if (callback_depth != 0) {
        throw std::logic_error(
            "behavior deferred mutations cannot drain while a callback is active");
    }
    std::vector<DeferredMutation> pending;
    pending.swap(deferred_mutations);
    return pending.size();
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

void preDestroyAllBehaviorObjectsForTeardown() noexcept {
    if (auto *arena = FastModuleContainer::tryGet<BehaviorAttachmentArena>()) {
        arena->deactivateAllForTeardown();
    }
}

std::size_t drainDeferredBehaviorMutationsForTeardown() {
    if (auto *arena = FastModuleContainer::tryGet<BehaviorAttachmentArena>()) {
        return arena->drainDeferredMutationsForTeardown();
    }
    return 0;
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
