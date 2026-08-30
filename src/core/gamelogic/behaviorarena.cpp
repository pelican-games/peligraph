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
                    .component_origin = ResolvedComponentOrigin{
                        AuthoredComponentOrigin{component_index}},
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
                .component_origin = ResolvedComponentOrigin{
                    AuthoredComponentOrigin{component_index}},
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

std::vector<PreparedSceneBehaviorAttachment>
prepareResolvedSceneBehaviorAttachments(
    std::span<const ResolvedObject> objects,
    BehaviorRegistryAvailability availability) {
    std::vector<PreparedSceneBehaviorAttachment> prepared;
    const auto &registry = internal::getBehaviorRegisterer();
    for (std::size_t object_index = 0; object_index < objects.size();
         ++object_index) {
        const auto &object = objects[object_index];
        const auto object_name = object.name.value_or(std::string{});
        for (const auto &component : object.components) {
            if (component.name != "behavior") continue;
            if (component.runtime_resolution_error) {
                throw std::runtime_error(
                    "behavior_resolution_error on object '" +
                    (object_name.empty() ? std::string{"<unnamed>"}
                                         : object_name) +
                    "': " + *component.runtime_resolution_error);
            }
            const auto stable_name =
                component.effective_json.value("type", std::string{});
            if (stable_name.empty()) {
                throw std::runtime_error(
                    "behavior on object '" +
                    (object_name.empty() ? std::string{"<unnamed>"}
                                         : object_name) +
                    "' requires a non-empty string type");
            }
            const auto *registration = registry.findByName(stable_name);
            if (registration == nullptr) {
                if (availability == BehaviorRegistryAvailability::active) {
                    throw std::runtime_error(
                        "Unknown behavior type '" + stable_name +
                        "' on object '" +
                        (object_name.empty() ? std::string{"<unnamed>"}
                                             : object_name) +
                        "'");
                }
                if (logger != nullptr) {
                    LOG_WARNING(
                        logger,
                        "behavior type '{}' on object '{}' is pending because the game-logic DLL is unavailable",
                        stable_name,
                        object_name.empty() ? std::string{"<unnamed>"}
                                            : object_name);
                }
                prepared.push_back(PreparedSceneBehaviorAttachment{
                    .object_index = object.authoring_object_index,
                    .component_index = component.merged_component_index,
                    .component_origin = component.origin,
                    .object_name = object_name,
                    .stable_name = stable_name,
                    .canonical_params = {},
                    .raw_component = component.source_json_exact,
                    .registration_owner = internal::engineRegistrationOwner,
                    .pending = true,
                });
                continue;
            }
            if (!component.behavior_canonical_params) {
                throw std::logic_error(
                    "resolved behavior is missing canonical params");
            }
            prepared.push_back(PreparedSceneBehaviorAttachment{
                .object_index = object.authoring_object_index,
                .component_index = component.merged_component_index,
                .component_origin = component.origin,
                .object_name = object_name,
                .stable_name = stable_name,
                .canonical_params = *component.behavior_canonical_params,
                .raw_component = component.source_json_exact,
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

void refreshSceneAttachmentSequence(BehaviorAttachmentInfo &info) noexcept {
    if (!info.scene_ordered) return;
    info.attachment_seq =
        (static_cast<std::uint64_t>(info.scene_object_index) << 32U) |
        static_cast<std::uint64_t>(info.component_index);
}

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
                    refreshSceneAttachmentSequence(attachment.info);
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
                    refreshSceneAttachmentSequence(attachment.info);
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
                if (found->info.activation_failed) {
                    found->info.activation_failed = false;
                    found->info.activation_error.clear();
                    found->activation_delay = 1;
                }
            }
            break;
        }
        case BehaviorAttachmentEditKind::insert_component:
            for (auto &attachment : arena.attachments) {
                if (attachment.info.entity == edit.entity &&
                    attachment.info.component_index >= edit.component_index) {
                    ++attachment.info.component_index;
                    refreshSceneAttachmentSequence(attachment.info);
                }
            }
            break;
        case BehaviorAttachmentEditKind::remove_component:
            for (auto &attachment : arena.attachments) {
                if (attachment.info.entity == edit.entity &&
                    attachment.info.component_index > edit.component_index) {
                    --attachment.info.component_index;
                    refreshSceneAttachmentSequence(attachment.info);
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

namespace {

bool sameSceneBehaviorOrigin(const ResolvedComponentOrigin &left,
                             const ResolvedComponentOrigin &right) {
    if (const auto *authored = left.authored()) {
        const auto *candidate = right.authored();
        return candidate != nullptr &&
               candidate->authoring_component_index ==
                   authored->authoring_component_index;
    }
    const auto *generated = left.generated();
    const auto *candidate = right.generated();
    return generated != nullptr && candidate != nullptr &&
           candidate->stable_generated_id == generated->stable_generated_id;
}

} // namespace

struct PreparedBehaviorSceneReconciliation::Impl {
    struct ParamUpdate {
        BehaviorAttachmentHandle handle = invalidBehaviorAttachmentHandle;
        internal::RawBehaviorParams old_params;
        internal::RawBehaviorParams next_params;
        internal::BehaviorApplyPreparedParamsFn apply = nullptr;
        internal::BehaviorDestroyPreparedParamsFn destroy = nullptr;
    };

    BehaviorAttachmentArena *arena = nullptr;
    std::vector<BehaviorAttachmentArena::Attachment> detached;
    std::vector<BehaviorAttachmentHandle> reused;
    std::vector<ParamUpdate> param_updates;
    std::uint64_t old_next_handle = 1;
    std::uint64_t old_next_attachment_seq = 1;
    std::uint64_t next_handle = 1;
    std::uint64_t next_attachment_seq = 1;
    bool old_attachment_sequence_exhausted = false;
    bool attachment_sequence_exhausted = false;
    bool published = false;
    bool finished = false;

    void releasePreparedParams() noexcept {
        for (auto &update : param_updates) {
            if (update.destroy == nullptr) continue;
            if (update.old_params.value != nullptr) {
                update.destroy(update.old_params);
            }
            if (update.next_params.value != nullptr) {
                update.destroy(update.next_params);
            }
        }
        param_updates.clear();
    }

    static void transferRuntime(BehaviorAttachmentArena::Attachment &from,
                                BehaviorAttachmentArena::Attachment &to) noexcept {
        to.instance = std::exchange(from.instance, {});
        to.destroy = std::exchange(from.destroy, nullptr);
        to.initialized = std::exchange(from.initialized, false);
        to.activation_delay = std::exchange(from.activation_delay, 0);
        to.info.active = std::exchange(from.info.active, false);
        to.info.activation_failed =
            std::exchange(from.info.activation_failed, false);
        to.info.activation_error = std::move(from.info.activation_error);
        from.info.activation_error.clear();
        to.info.owner = from.info.owner;
    }

    void destroyDetached(bool callbacks) noexcept {
        if (arena == nullptr) return;
        for (auto it = detached.rbegin(); it != detached.rend(); ++it) {
            if (callbacks) {
                arena->destroyInstance(*it, true);
            } else if (it->instance.behavior != nullptr &&
                       it->destroy != nullptr) {
                it->destroy(it->instance);
                it->destroy = nullptr;
                it->instance = {};
            }
        }
        detached.clear();
    }

    ~Impl() {
        releasePreparedParams();
        if (!finished) destroyDetached(false);
    }
};

PreparedBehaviorSceneReconciliation::PreparedBehaviorSceneReconciliation(
    std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
PreparedBehaviorSceneReconciliation::~PreparedBehaviorSceneReconciliation() =
    default;
PreparedBehaviorSceneReconciliation::PreparedBehaviorSceneReconciliation(
    PreparedBehaviorSceneReconciliation &&) noexcept = default;
PreparedBehaviorSceneReconciliation &
PreparedBehaviorSceneReconciliation::operator=(
    PreparedBehaviorSceneReconciliation &&) noexcept = default;

void PreparedBehaviorSceneReconciliation::publish() noexcept {
    if (!impl_ || impl_->published || impl_->finished) return;
    auto &arena = *impl_->arena;
    for (const auto handle : impl_->reused) {
        const auto old = std::find_if(
            arena.attachments.begin(), arena.attachments.end(),
            [handle](const auto &attachment) {
                return attachment.info.handle == handle;
            });
        const auto next = std::find_if(
            impl_->detached.begin(), impl_->detached.end(),
            [handle](const auto &attachment) {
                return attachment.info.handle == handle;
            });
        if (old != arena.attachments.end() &&
            next != impl_->detached.end()) {
            Impl::transferRuntime(*old, *next);
        }
    }
    arena.attachments.swap(impl_->detached);
    arena.next_handle = impl_->next_handle;
    arena.next_attachment_seq = impl_->next_attachment_seq;
    arena.attachment_sequence_exhausted =
        impl_->attachment_sequence_exhausted;
    for (auto &update : impl_->param_updates) {
        const auto current = std::find_if(
            arena.attachments.begin(), arena.attachments.end(),
            [&](const auto &attachment) {
                return attachment.info.handle == update.handle;
            });
        if (current != arena.attachments.end() &&
            current->instance.params != nullptr &&
            update.next_params.value != nullptr) {
            update.apply(current->instance.params, update.next_params);
        }
    }
    impl_->published = true;
}

void PreparedBehaviorSceneReconciliation::rollback() noexcept {
    if (!impl_ || impl_->finished) return;
    auto &arena = *impl_->arena;
    if (impl_->published) {
        for (auto &update : impl_->param_updates) {
            const auto current = std::find_if(
                arena.attachments.begin(), arena.attachments.end(),
                [&](const auto &attachment) {
                    return attachment.info.handle == update.handle;
                });
            if (current != arena.attachments.end() &&
                current->instance.params != nullptr &&
                update.old_params.value != nullptr) {
                update.apply(current->instance.params, update.old_params);
            }
        }
        for (const auto handle : impl_->reused) {
            const auto current = std::find_if(
                arena.attachments.begin(), arena.attachments.end(),
                [handle](const auto &attachment) {
                    return attachment.info.handle == handle;
                });
            const auto old = std::find_if(
                impl_->detached.begin(), impl_->detached.end(),
                [handle](const auto &attachment) {
                    return attachment.info.handle == handle;
                });
            if (current != arena.attachments.end() &&
                old != impl_->detached.end()) {
                Impl::transferRuntime(*current, *old);
            }
        }
        arena.attachments.swap(impl_->detached);
        arena.next_handle = impl_->old_next_handle;
        arena.next_attachment_seq = impl_->old_next_attachment_seq;
        arena.attachment_sequence_exhausted =
            impl_->old_attachment_sequence_exhausted;
    }
    impl_->releasePreparedParams();
    impl_->destroyDetached(false);
    impl_->finished = true;
}

void PreparedBehaviorSceneReconciliation::finish() noexcept {
    if (!impl_ || impl_->finished) return;
    impl_->releasePreparedParams();
    impl_->destroyDetached(impl_->published);
    impl_->finished = true;
}

struct PreparedBehaviorEntityPreDestroy::Impl {
    BehaviorAttachmentArena *arena = nullptr;
    std::vector<GameObjectId> entities;
    std::vector<BehaviorAttachmentArena::Attachment> detached;
    std::vector<BehaviorAttachmentArena::Attachment> scratch;
    std::size_t deferred_checkpoint = 0;
    bool published = false;
    bool finished = false;

    bool targets(GameObjectId entity) const noexcept {
        return std::find(entities.begin(), entities.end(), entity) !=
               entities.end();
    }

    void destroyDetached() noexcept {
        if (arena == nullptr) return;
        for (auto it = detached.rbegin(); it != detached.rend(); ++it) {
            arena->destroyInstance(*it, false);
        }
        detached.clear();
    }

    ~Impl() {
        if (published && !finished) destroyDetached();
    }
};

PreparedBehaviorEntityPreDestroy::PreparedBehaviorEntityPreDestroy(
    std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}
PreparedBehaviorEntityPreDestroy::~PreparedBehaviorEntityPreDestroy() =
    default;
PreparedBehaviorEntityPreDestroy::PreparedBehaviorEntityPreDestroy(
    PreparedBehaviorEntityPreDestroy &&) noexcept = default;
PreparedBehaviorEntityPreDestroy &
PreparedBehaviorEntityPreDestroy::operator=(
    PreparedBehaviorEntityPreDestroy &&) noexcept = default;

void PreparedBehaviorEntityPreDestroy::publish() noexcept {
    if (!impl_ || impl_->published || impl_->finished) return;
    auto &arena = *impl_->arena;

    // The callback pass observes the untouched arena and ECS. Moving the
    // instances happens only after the complete reverse-order barrier.
    for (auto it = arena.attachments.rbegin();
         it != arena.attachments.rend(); ++it) {
        if (!impl_->targets(it->info.entity) ||
            it->instance.behavior == nullptr || !it->initialized) {
            continue;
        }
        arena.invokeDestroyCallback(*it);
    }

    impl_->detached.clear();
    impl_->scratch.clear();
    for (auto &attachment : arena.attachments) {
        if (impl_->targets(attachment.info.entity)) {
            impl_->detached.push_back(std::move(attachment));
        } else {
            impl_->scratch.push_back(std::move(attachment));
        }
    }
    arena.attachments.swap(impl_->scratch);
    impl_->scratch.clear();
    impl_->published = true;
}

void PreparedBehaviorEntityPreDestroy::rollback() noexcept {
    if (!impl_ || impl_->finished) return;
    auto &arena = *impl_->arena;
    if (!impl_->published) {
        if (arena.deferred_mutations.size() > impl_->deferred_checkpoint) {
            arena.deferred_mutations.resize(impl_->deferred_checkpoint);
        }
    } else {
        // Publication is deliberately beyond the point of no return. If a
        // caller violates that invariant, never resurrect an initialized
        // instance whose onDestroy already ran.
        impl_->destroyDetached();
    }
    impl_->finished = true;
}

void PreparedBehaviorEntityPreDestroy::finish() noexcept {
    if (!impl_ || impl_->finished) return;
    impl_->destroyDetached();
    impl_->finished = true;
}

std::unique_ptr<PreparedBehaviorEntityPreDestroy>
BehaviorAttachmentArena::prepareEntityPreDestroy(
    std::vector<GameObjectId> entities) {
    std::sort(entities.begin(), entities.end(), [](auto left, auto right) {
        if (left.index != right.index) return left.index < right.index;
        return left.generation < right.generation;
    });
    entities.erase(std::unique(entities.begin(), entities.end()),
                   entities.end());
    if (std::any_of(entities.begin(), entities.end(), [](auto entity) {
            return entity == invalidGameObjectId;
        })) {
        throw std::invalid_argument(
            "behavior pre-destroy requires valid entity identities");
    }
    auto impl = std::make_unique<PreparedBehaviorEntityPreDestroy::Impl>();
    impl->arena = this;
    impl->entities = std::move(entities);
    impl->deferred_checkpoint = deferred_mutations.size();
    impl->detached.reserve(attachments.size());
    impl->scratch.reserve(attachments.size());
    return std::unique_ptr<PreparedBehaviorEntityPreDestroy>{
        new PreparedBehaviorEntityPreDestroy{std::move(impl)}};
}

std::unique_ptr<PreparedBehaviorSceneReconciliation>
BehaviorAttachmentArena::prepareSceneReconciliation(
    std::vector<BoundSceneBehaviorAttachment> desired) {
    auto impl =
        std::make_unique<PreparedBehaviorSceneReconciliation::Impl>();
    impl->arena = this;
    impl->old_next_handle = next_handle;
    impl->old_next_attachment_seq = next_attachment_seq;
    impl->next_handle = next_handle;
    impl->next_attachment_seq = next_attachment_seq;
    impl->old_attachment_sequence_exhausted =
        attachment_sequence_exhausted;
    impl->attachment_sequence_exhausted =
        attachment_sequence_exhausted;
    impl->detached.reserve(attachments.size() + desired.size());
    std::vector<bool> consumed(attachments.size(), false);

    // Runtime-created, non-scene attachments are outside reconciliation but
    // participate in the same atomic vector publication.
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        const auto &attachment = attachments[index];
        if (attachment.info.scene_ordered) continue;
        consumed[index] = true;
        impl->detached.push_back(Attachment{
            .info = attachment.info,
            .raw_component = attachment.raw_component,
        });
        impl->reused.push_back(attachment.info.handle);
    }

    for (auto &bound : desired) {
        if (bound.entity == invalidGameObjectId) {
            throw std::runtime_error(
                "behavior scene reconciliation requires a live entity");
        }
        const auto sequence = sceneBehaviorAttachmentSeq(
            bound.prepared.object_index, bound.prepared.component_index);
        if (bound.planned_identity &&
            (bound.planned_identity->handle ==
                 invalidBehaviorAttachmentHandle ||
             bound.planned_identity->attachment_seq != sequence)) {
            throw std::runtime_error(
                "behavior scene reconciliation identity plan is stale");
        }
        std::optional<std::size_t> reusable;
        if (bound.preserve_planned_identity) {
            if (!bound.planned_identity) {
                throw std::logic_error(
                    "behavior identity preservation requires a plan entry");
            }
            for (std::size_t index = 0; index < attachments.size(); ++index) {
                const auto &current = attachments[index];
                if (consumed[index] ||
                    current.info.handle != bound.planned_identity->handle) {
                    continue;
                }
                if (!current.info.scene_ordered ||
                    current.info.entity != bound.entity ||
                    current.info.stable_name != bound.prepared.stable_name ||
                    current.info.pending != bound.prepared.pending ||
                    !sameSceneBehaviorOrigin(
                        current.info.component_origin,
                        bound.prepared.component_origin)) {
                    throw std::runtime_error(
                        "behavior identity plan targets a different attachment");
                }
                reusable = index;
                break;
            }
            if (!reusable) {
                throw std::runtime_error(
                    "behavior identity plan target is absent");
            }
        }
        if (!bound.planned_identity) {
            for (std::size_t index = 0; index < attachments.size(); ++index) {
                if (reusable) break;
                const auto &current = attachments[index];
                if (consumed[index] || !current.info.scene_ordered ||
                    current.info.entity != bound.entity ||
                    current.info.stable_name != bound.prepared.stable_name ||
                    current.info.canonical_params !=
                        bound.prepared.canonical_params ||
                    current.info.pending != bound.prepared.pending ||
                    !sameSceneBehaviorOrigin(current.info.component_origin,
                                             bound.prepared.component_origin)) {
                    continue;
                }
                reusable = index;
                break;
            }
        }

        BehaviorAttachmentHandle handle{};
        if (reusable) {
            consumed[*reusable] = true;
            handle = attachments[*reusable].info.handle;
            if (bound.planned_identity &&
                bound.planned_identity->handle != handle) {
                throw std::runtime_error(
                    "reusable behavior identity disagrees with the final plan");
            }
            impl->reused.push_back(handle);
            const auto &current = attachments[*reusable];
            if (!current.info.pending &&
                current.info.canonical_params !=
                    bound.prepared.canonical_params) {
                const auto *registration =
                    internal::getBehaviorRegisterer().findByNameAndOwner(
                        current.info.stable_name, current.info.owner);
                if (registration == nullptr) {
                    throw std::runtime_error(
                        "behavior registration disappeared during params reconciliation: " +
                        current.info.stable_name);
                }
                PreparedBehaviorSceneReconciliation::Impl::ParamUpdate update{
                    .handle = handle,
                    .apply = registration->apply_prepared_params,
                    .destroy = registration->destroy_prepared_params,
                };
                update.old_params = registration->prepare_params(
                    current.info.canonical_params);
                try {
                    update.next_params = registration->prepare_params(
                        bound.prepared.canonical_params);
                } catch (...) {
                    if (update.old_params.value != nullptr) {
                        update.destroy(update.old_params);
                    }
                    throw;
                }
                impl->param_updates.push_back(std::move(update));
            }
        } else {
            if (bound.planned_identity) {
                handle = bound.planned_identity->handle;
                if (std::any_of(
                        attachments.begin(), attachments.end(),
                        [&](const auto &attachment) {
                            return attachment.info.handle == handle;
                        }) ||
                    std::any_of(
                        impl->detached.begin(), impl->detached.end(),
                        [&](const auto &attachment) {
                            return attachment.info.handle == handle;
                        })) {
                    throw std::runtime_error(
                        "planned behavior handle is already live");
                }
                if (handle.value >= impl->next_handle) {
                    if (handle.value ==
                        std::numeric_limits<std::uint64_t>::max()) {
                        throw std::overflow_error(
                            "BehaviorAttachmentHandle space exhausted");
                    }
                    impl->next_handle = handle.value + 1U;
                }
            } else {
                if (impl->next_handle == 0) {
                    throw std::overflow_error(
                        "BehaviorAttachmentHandle space exhausted");
                }
                handle = BehaviorAttachmentHandle{impl->next_handle++};
            }
        }
        Attachment next{
            .info = BehaviorAttachmentInfo{
                .handle = handle,
                .attachment_seq = sequence,
                .entity = bound.entity,
                .component_index = bound.prepared.component_index,
                .component_origin = bound.prepared.component_origin,
                .scene_object_index = bound.prepared.object_index,
                .scene_ordered = true,
                .stable_name = bound.prepared.stable_name,
                .canonical_params = bound.prepared.canonical_params,
                .owner = bound.prepared.registration_owner,
                .pending = bound.prepared.pending,
                .active = false,
            },
            .raw_component = std::move(bound.prepared.raw_component),
        };
        if (!reusable && !next.info.pending) {
            const auto *registration =
                internal::getBehaviorRegisterer().findByNameAndOwner(
                    next.info.stable_name, next.info.owner);
            if (registration == nullptr) {
                throw std::runtime_error(
                    "behavior registration disappeared during scene reconciliation: " +
                    next.info.stable_name);
            }
            next.instance = registration->create(next.info.canonical_params);
            next.destroy = registration->destroy;
            next.activation_delay = 1;
        }
        impl->detached.push_back(std::move(next));
        if (sequence == std::numeric_limits<std::uint64_t>::max()) {
            impl->attachment_sequence_exhausted = true;
        } else if (!impl->attachment_sequence_exhausted) {
            impl->next_attachment_seq =
                std::max(impl->next_attachment_seq, sequence + 1U);
        }
    }

    std::sort(impl->detached.begin(), impl->detached.end(),
              [](const auto &left, const auto &right) {
                  return left.info.attachment_seq <
                         right.info.attachment_seq;
              });
    for (std::size_t index = 1; index < impl->detached.size(); ++index) {
        if (impl->detached[index - 1].info.attachment_seq ==
            impl->detached[index].info.attachment_seq) {
            throw std::runtime_error(
                "behavior scene reconciliation produced an attachment sequence collision");
        }
    }
    return std::unique_ptr<PreparedBehaviorSceneReconciliation>{
        new PreparedBehaviorSceneReconciliation{std::move(impl)}};
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
                .component_origin = bound.prepared.component_origin,
                .scene_object_index = bound.prepared.object_index,
                .scene_ordered = true,
                .stable_name = bound.prepared.stable_name,
                .canonical_params = bound.prepared.canonical_params,
                .owner = bound.prepared.registration_owner,
                .pending = bound.prepared.pending,
                .active = false,
            },
            .raw_component = std::move(bound.prepared.raw_component),
        });
        if (sequence == std::numeric_limits<std::uint64_t>::max()) {
            attachment_sequence_exhausted = true;
        } else if (!attachment_sequence_exhausted) {
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

void BehaviorAttachmentArena::invokeDestroyCallback(
    Attachment &attachment) noexcept {
    if (attachment.instance.behavior != nullptr && attachment.initialized) {
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
}

void BehaviorAttachmentArena::destroyInstance(Attachment &attachment,
                                              bool invoke_destroy_callback) noexcept {
    if (attachment.instance.behavior == nullptr) {
        attachment.initialized = false;
        attachment.info.active = false;
        return;
    }
    if (invoke_destroy_callback) invokeDestroyCallback(attachment);
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
            attachment.info.activation_failed || attachment.activation_delay != 0) {
            continue;
        }
        const auto deferred_base = deferred_mutations.size();
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
            attachment.info.activation_error.clear();
        } catch (const std::exception &error) {
            destroyInstance(attachment, false);
            deferred_mutations.resize(deferred_base);
            attachment.info.activation_failed = true;
            attachment.info.activation_error = error.what();
            if (logger != nullptr) {
                LOG_ERROR(logger, "editor behavior activation failed for type '{}': {}",
                          attachment.info.stable_name, error.what());
            }
        } catch (...) {
            destroyInstance(attachment, false);
            deferred_mutations.resize(deferred_base);
            attachment.info.activation_failed = true;
            attachment.info.activation_error = "unknown editor behavior activation failure";
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
    if (attachment_sequence_exhausted ||
        next_attachment_seq > max_exact_json_integer) {
        throw std::overflow_error("runtime behavior attachment sequence space exhausted");
    }
    const auto sequence = next_attachment_seq;
    if (next_attachment_seq == std::numeric_limits<std::uint64_t>::max()) {
        attachment_sequence_exhausted = true;
    } else {
        ++next_attachment_seq;
    }
    return BehaviorAttachmentIdentity{
        .handle = BehaviorAttachmentHandle{next_handle++},
        .attachment_seq = sequence,
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
        std::size_t scene_object_index = 0;
        bool scene_ordered = false;
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
            .scene_object_index = attachment.info.scene_object_index,
            .scene_ordered = attachment.info.scene_ordered,
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
            const auto *registration =
                internal::getBehaviorRegisterer().findByName(current.stable_name);
            if (registration == nullptr) {
                throw std::runtime_error("behavior registration unavailable: " +
                                         current.stable_name);
            }
            auto instance = registration->create(current.canonical_params);
            for (auto &candidate : simulated) {
                if (candidate.entity == current.entity &&
                    candidate.component_index >= current.component_index) {
                    ++candidate.component_index;
                    if (candidate.scene_ordered) {
                        candidate.attachment_seq = sceneBehaviorAttachmentSeq(
                            candidate.scene_object_index,
                            candidate.component_index);
                    }
                }
            }
            const auto effective_sequence = current.scene_ordered
                ? sceneBehaviorAttachmentSeq(current.scene_object_index,
                                             current.component_index)
                : current.identity.attachment_seq;
            if (std::any_of(simulated.begin(), simulated.end(), [&](const auto &candidate) {
                    return candidate.handle == current.identity.handle ||
                           candidate.attachment_seq == effective_sequence;
                })) {
                throw std::runtime_error("behavior editor attach identity is already live");
            }
            item.attachment.emplace(Attachment{
                .info = BehaviorAttachmentInfo{
                    .handle = current.identity.handle,
                    .attachment_seq = effective_sequence,
                    .entity = current.entity,
                    .component_index = current.component_index,
                    .component_origin = current.component_origin,
                    .scene_object_index = current.scene_object_index,
                    .scene_ordered = current.scene_ordered,
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
            simulated.push_back({
                .handle = current.identity.handle,
                .attachment_seq = effective_sequence,
                .entity = current.entity,
                .component_index = current.component_index,
                .scene_object_index = current.scene_object_index,
                .scene_ordered = current.scene_ordered,
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
                (current.identity.handle == invalidBehaviorAttachmentHandle &&
                 current.identity.attachment_seq != 0 &&
                 found->attachment_seq != current.identity.attachment_seq)) {
                throw std::runtime_error("behavior editor remove target is stale");
            }
            simulated.erase(found);
            for (auto &candidate : simulated) {
                if (candidate.entity == current.entity &&
                    candidate.component_index > current.component_index) {
                    --candidate.component_index;
                    if (candidate.scene_ordered) {
                        candidate.attachment_seq = sceneBehaviorAttachmentSeq(
                            candidate.scene_object_index,
                            candidate.component_index);
                    }
                }
            }
            break;
        }
        case BehaviorAttachmentEditKind::set_params: {
            const auto found = find_simulated(current);
            if (found == simulated.end() || found->entity != current.entity ||
                found->component_index != current.component_index || found->pending ||
                (current.identity.handle == invalidBehaviorAttachmentHandle &&
                 current.identity.attachment_seq != 0 &&
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
                    if (candidate.scene_ordered) {
                        candidate.attachment_seq = sceneBehaviorAttachmentSeq(
                            candidate.scene_object_index,
                            candidate.component_index);
                    }
                }
            }
            break;
        case BehaviorAttachmentEditKind::remove_component:
            for (auto &candidate : simulated) {
                if (candidate.entity == current.entity &&
                    candidate.component_index > current.component_index) {
                    --candidate.component_index;
                    if (candidate.scene_ordered) {
                        candidate.attachment_seq = sceneBehaviorAttachmentSeq(
                            candidate.scene_object_index,
                            candidate.component_index);
                    }
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
