#include "behaviorarena.hpp"
#include "../registrationownerhost.hpp"
#include "../loader/sceneload.hpp"

#include "../ecs/core.hpp"
#include "../log.hpp"
#include "../userpublic/components/predefined.hpp"
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

BehaviorTypeUnknownError::BehaviorTypeUnknownError(
    std::string stable_name, std::string object_name,
    std::optional<AuthoringObjectId> object_id)
    : std::runtime_error(
          "Unknown behavior type '" + stable_name + "'" +
          (object_name.empty()
               ? std::string{}
               : " on object '" + object_name + "'")),
      stable_name_{std::move(stable_name)}, object_id_{object_id} {}

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
                    throw BehaviorTypeUnknownError{
                        stable_name,
                        object_name.empty() ? std::string{"<unnamed>"}
                                            : object_name};
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
                const ResolutionFailureException failure{
                    *component.runtime_resolution_error};
                throw ResolutionFailureException{
                    failure.failure(),
                    "behavior_resolution_error on object '" +
                        (object_name.empty() ? std::string{"<unnamed>"}
                                             : object_name) +
                        "': " + failure.what()};
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
                    throw BehaviorTypeUnknownError{
                        stable_name,
                        object_name.empty() ? std::string{"<unnamed>"}
                                            : object_name,
                        object.authoring_object_id};
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
    struct ParamsUpdate {
        BehaviorAttachmentHandle handle = invalidBehaviorAttachmentHandle;
        internal::RawBehaviorParams params;
        internal::BehaviorApplyPreparedParamsFn apply_params = nullptr;
        internal::BehaviorDestroyPreparedParamsFn destroy_params = nullptr;
        bool activation_failed = false;
        std::string activation_error;
        std::uint8_t activation_delay = 0;
    };

    BehaviorAttachmentArena *arena = nullptr;
    // Before publication this is the fully prepared next vector. Publication
    // swaps it with the arena, retaining the complete old vector as the
    // inverse token. Runtime instances for retained handles move between the
    // two vectors; no owner is copied and rollback performs no allocation.
    std::vector<BehaviorAttachmentArena::Attachment> detached;
    std::vector<BehaviorAttachmentHandle> reused;
    std::vector<ParamsUpdate> params_updates;
    bool published = false;
    bool finished = false;

    static void transferRuntime(BehaviorAttachmentArena::Attachment &from,
                                BehaviorAttachmentArena::Attachment &to) noexcept {
        to.instance = std::exchange(from.instance, {});
        to.destroy = std::exchange(from.destroy, nullptr);
        to.initialized = std::exchange(from.initialized, false);
        to.info.activation_delay =
            std::exchange(from.info.activation_delay, 0);
        to.info.active = std::exchange(from.info.active, false);
        to.info.activation_failed =
            std::exchange(from.info.activation_failed, false);
        to.info.activation_error = std::move(from.info.activation_error);
        from.info.activation_error.clear();
        to.info.owner = from.info.owner;
    }

    static void swapParamsAndActivation(
        BehaviorAttachmentArena::Attachment &attachment,
        ParamsUpdate &update) noexcept {
        if (attachment.instance.params != nullptr &&
            update.params.value != nullptr) {
            update.apply_params(attachment.instance.params, update.params);
        }
        using std::swap;
        swap(attachment.info.activation_failed, update.activation_failed);
        attachment.info.activation_error.swap(update.activation_error);
        swap(attachment.info.activation_delay, update.activation_delay);
    }

    void releasePreparedParams() noexcept {
        for (auto &update : params_updates) {
            if (update.params.value != nullptr &&
                update.destroy_params != nullptr) {
                update.destroy_params(update.params);
            }
        }
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
    for (auto &update : impl_->params_updates) {
        const auto next = std::find_if(
            impl_->detached.begin(), impl_->detached.end(),
            [&](const auto &attachment) {
                return attachment.info.handle == update.handle;
            });
        if (next != impl_->detached.end()) {
            Impl::swapParamsAndActivation(*next, update);
        }
    }
    arena.attachments.swap(impl_->detached);
    impl_->published = true;
}

void PreparedBehaviorAttachmentEdits::rollback() noexcept {
    if (!impl_ || impl_->finished) return;
    auto &arena = *impl_->arena;
    if (impl_->published) {
        for (auto update = impl_->params_updates.rbegin();
             update != impl_->params_updates.rend(); ++update) {
            const auto current = std::find_if(
                arena.attachments.begin(), arena.attachments.end(),
                [&](const auto &attachment) {
                    return attachment.info.handle == update->handle;
                });
            if (current != arena.attachments.end()) {
                Impl::swapParamsAndActivation(*current, *update);
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
    }
    impl_->destroyDetached(false);
    impl_->releasePreparedParams();
    impl_->finished = true;
}

void PreparedBehaviorAttachmentEdits::finish() noexcept {
    if (!impl_ || impl_->finished) return;
    impl_->destroyDetached(impl_->published);
    impl_->releasePreparedParams();
    impl_->finished = true;
}

namespace {

bool sameSceneBehaviorOrigin(const ResolvedComponentOrigin &left,
                             const ResolvedComponentOrigin &right,
                             const OriginTransitionMap &transition) {
    if (const auto *authored = left.authored()) {
        const auto *candidate = right.authored();
        if (candidate != nullptr) {
            return candidate->authoring_component_index ==
                   authored->authoring_component_index;
        }
        if (transition.direction !=
            OriginTransitionDirection::AuthoredToGenerated) {
            return false;
        }
        const auto *generated = right.generated();
        if (generated == nullptr) return false;
        return std::any_of(
            transition.entries.begin(), transition.entries.end(),
            [&](const OriginTransitionEntry &entry) {
                return entry.gid == generated->stable_generated_id &&
                       entry.component_key == generated->component_key &&
                       entry.final_authored_index ==
                           authored->authoring_component_index;
            });
    }
    const auto *generated = left.generated();
    const auto *candidate = right.generated();
    if (generated == nullptr) return false;
    if (candidate != nullptr) {
        return candidate->stable_generated_id == generated->stable_generated_id;
    }
    if (transition.direction !=
        OriginTransitionDirection::GeneratedToAuthored) {
        return false;
    }
    const auto *authored = right.authored();
    if (authored == nullptr) return false;
    return std::any_of(
        transition.entries.begin(), transition.entries.end(),
        [&](const OriginTransitionEntry &entry) {
            return entry.gid == generated->stable_generated_id &&
                   entry.component_key == generated->component_key &&
                   entry.final_authored_index ==
                       authored->authoring_component_index;
        });
}

} // namespace

struct PreparedBehaviorSceneReconciliation::Impl {
    struct ParamsUpdate {
        BehaviorAttachmentHandle handle = invalidBehaviorAttachmentHandle;
        internal::RawBehaviorParams params;
        internal::BehaviorApplyPreparedParamsFn apply_params = nullptr;
        internal::BehaviorDestroyPreparedParamsFn destroy_params = nullptr;
        bool activation_failed = false;
        std::string activation_error;
        std::uint8_t activation_delay = 0;
    };

    BehaviorAttachmentArena *arena = nullptr;
    std::vector<BehaviorAttachmentArena::Attachment> detached;
    std::vector<BehaviorAttachmentHandle> reused;
    std::vector<ParamsUpdate> params_updates;
    std::uint64_t old_next_handle = 1;
    std::uint64_t old_next_attachment_seq = 1;
    std::uint64_t next_handle = 1;
    std::uint64_t next_attachment_seq = 1;
    std::vector<std::size_t> activated;
    std::size_t deferred_base = 0;
    bool published = false;
    bool finished = false;

    static void transferRuntime(BehaviorAttachmentArena::Attachment &from,
                                BehaviorAttachmentArena::Attachment &to) noexcept {
        to.instance = std::exchange(from.instance, {});
        to.destroy = std::exchange(from.destroy, nullptr);
        to.initialized = std::exchange(from.initialized, false);
        to.info.activation_delay =
            std::exchange(from.info.activation_delay, 0);
        to.info.active = std::exchange(from.info.active, false);
        to.info.activation_failed =
            std::exchange(from.info.activation_failed, false);
        to.info.activation_error = std::move(from.info.activation_error);
        from.info.activation_error.clear();
        to.info.owner = from.info.owner;
    }

    ParamsUpdate *findParamsUpdate(BehaviorAttachmentHandle handle) noexcept {
        const auto found = std::find_if(
            params_updates.begin(), params_updates.end(),
            [handle](const ParamsUpdate &update) {
                return update.handle == handle;
            });
        return found == params_updates.end() ? nullptr : &*found;
    }

    static void swapParamsAndActivation(
        BehaviorAttachmentArena::Attachment &attachment,
        ParamsUpdate &update) noexcept {
        if (attachment.instance.params != nullptr &&
            update.params.value != nullptr) {
            update.apply_params(attachment.instance.params, update.params);
        }
        using std::swap;
        swap(attachment.info.activation_failed, update.activation_failed);
        attachment.info.activation_error.swap(update.activation_error);
        swap(attachment.info.activation_delay, update.activation_delay);
    }

    void releasePreparedParams() noexcept {
        for (auto &update : params_updates) {
            if (update.params.value != nullptr &&
                update.destroy_params != nullptr) {
                update.destroy_params(update.params);
            }
        }
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
            if (auto *update = impl_->findParamsUpdate(handle)) {
                Impl::swapParamsAndActivation(*next, *update);
            }
        }
    }
    arena.attachments.swap(impl_->detached);
    arena.next_handle = impl_->next_handle;
    arena.next_attachment_seq = impl_->next_attachment_seq;
    impl_->published = true;
}

void PreparedBehaviorSceneReconciliation::publish(
    SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::publish);
    publish();
}

void PreparedBehaviorSceneReconciliation::activate() {
    if (!impl_ || !impl_->published || impl_->finished) {
        throw std::logic_error(
            "behavior scene reconciliation is not published");
    }
    auto &arena = *impl_->arena;
    impl_->deferred_base = arena.deferred_mutations.size();
    impl_->activated.clear();
    try {
        for (std::size_t index = 0; index < arena.attachments.size(); ++index) {
            auto &attachment = arena.attachments[index];
            if (attachment.info.pending || attachment.info.active) continue;
            if (attachment.instance.behavior == nullptr) {
                const auto *registration =
                    internal::getBehaviorRegisterer().findByNameAndOwner(
                        attachment.info.stable_name, attachment.info.owner);
                if (registration == nullptr) {
                    throw std::runtime_error(
                        "behavior registration disappeared before activation: " +
                        attachment.info.stable_name);
                }
                attachment.instance =
                    registration->create(attachment.info.canonical_params);
                attachment.destroy = registration->destroy;
            }
            try {
                arena.invokeInit(attachment);
            } catch (...) {
                arena.destroyInstance(attachment, false);
                throw;
            }
            attachment.initialized = true;
            attachment.info.active = true;
            impl_->activated.push_back(index);
        }
    } catch (...) {
        for (auto it = impl_->activated.rbegin();
             it != impl_->activated.rend(); ++it) {
            arena.destroyInstance(arena.attachments[*it], true);
        }
        arena.deferred_mutations.resize(impl_->deferred_base);
        impl_->activated.clear();
        throw;
    }
}

void PreparedBehaviorSceneReconciliation::activate(
    SceneLoadPhaseTrace &phase_trace) {
    phase_trace.record(SceneLoadPhase::activate);
    activate();
}

void PreparedBehaviorSceneReconciliation::rollback() noexcept {
    if (!impl_ || impl_->finished) return;
    auto &arena = *impl_->arena;
    if (impl_->published) {
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
                if (auto *update = impl_->findParamsUpdate(handle)) {
                    Impl::swapParamsAndActivation(*current, *update);
                }
                Impl::transferRuntime(*current, *old);
            }
        }
        arena.attachments.swap(impl_->detached);
        arena.next_handle = impl_->old_next_handle;
        arena.next_attachment_seq = impl_->old_next_attachment_seq;
    }
    impl_->destroyDetached(false);
    impl_->releasePreparedParams();
    impl_->finished = true;
}

void PreparedBehaviorSceneReconciliation::finish() noexcept {
    if (!impl_ || impl_->finished) return;
    impl_->destroyDetached(impl_->published);
    impl_->releasePreparedParams();
    impl_->finished = true;
}

void PreparedBehaviorSceneReconciliation::finish(
    SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::retire);
    finish();
}

std::unique_ptr<PreparedBehaviorSceneReconciliation>
BehaviorAttachmentArena::prepareSceneReconciliation(
    std::vector<BoundSceneBehaviorAttachment> desired,
    const OriginTransitionMap &origin_transition) {
    auto impl =
        std::make_unique<PreparedBehaviorSceneReconciliation::Impl>();
    impl->arena = this;
    impl->old_next_handle = next_handle;
    impl->old_next_attachment_seq = next_attachment_seq;
    impl->next_handle = next_handle;
    impl->next_attachment_seq = next_attachment_seq;
    impl->detached.reserve(attachments.size() + desired.size());
    impl->params_updates.reserve(desired.size());
    impl->activated.reserve(attachments.size() + desired.size());
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
        std::optional<std::size_t> reusable;
        for (std::size_t index = 0; index < attachments.size(); ++index) {
            const auto &current = attachments[index];
            if (consumed[index] || !current.info.scene_ordered ||
                current.info.entity != bound.entity ||
                current.info.stable_name != bound.prepared.stable_name ||
                current.info.pending != bound.prepared.pending ||
                current.info.owner != bound.prepared.registration_owner ||
                !sameSceneBehaviorOrigin(current.info.component_origin,
                                         bound.prepared.component_origin,
                                         origin_transition)) {
                continue;
            }
            reusable = index;
            break;
        }

        BehaviorAttachmentHandle handle{};
        if (reusable) {
            consumed[*reusable] = true;
            const auto &current = attachments[*reusable];
            handle = current.info.handle;
            impl->reused.push_back(handle);
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
                PreparedBehaviorSceneReconciliation::Impl::ParamsUpdate update{
                    .handle = handle,
                    .apply_params = registration->apply_prepared_params,
                    .destroy_params = registration->destroy_prepared_params,
                    .activation_failed = false,
                    .activation_error = current.info.activation_failed
                                            ? std::string{}
                                            : current.info.activation_error,
                    .activation_delay = current.info.activation_failed
                                            ? std::uint8_t{1}
                                            : current.info.activation_delay,
                };
                update.params = registration->prepare_params(
                    bound.prepared.canonical_params);
                try {
                    impl->params_updates.push_back(std::move(update));
                } catch (...) {
                    if (update.params.value != nullptr &&
                        update.destroy_params != nullptr) {
                        update.destroy_params(update.params);
                    }
                    throw;
                }
            }
        } else {
            if (impl->next_handle == 0) {
                throw std::overflow_error(
                    "BehaviorAttachmentHandle space exhausted");
            }
            handle = BehaviorAttachmentHandle{impl->next_handle++};
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
            next.info.activation_delay = 1;
        }
        impl->detached.push_back(std::move(next));
        if (sequence == std::numeric_limits<std::uint64_t>::max()) {
            impl->next_attachment_seq = 0;
        } else {
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
            attachment.info.activation_failed ||
            attachment.info.activation_delay != 0) {
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
        if (!attachment.info.active &&
            attachment.info.activation_delay != 0) {
            --attachment.info.activation_delay;
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
    GameObjectId entity, std::size_t authored_component_occurrence) const {
    const auto found = std::find_if(
        attachments.begin(), attachments.end(), [&](const Attachment &attachment) {
            const auto *authored = attachment.info.component_origin.authored();
            return attachment.info.entity == entity && authored != nullptr &&
                   authored->authoring_component_index ==
                       authored_component_occurrence;
        });
    return found == attachments.end() ? std::nullopt
                                      : std::optional{found->info};
}

std::unique_ptr<PreparedBehaviorAttachmentEdits>
BehaviorAttachmentArena::prepareEditorEdits(
    std::vector<BehaviorAttachmentEdit> edits,
    BehaviorRegistryAvailability availability) {
    auto impl = std::make_unique<PreparedBehaviorAttachmentEdits::Impl>();
    impl->arena = this;
    impl->detached.reserve(attachments.size() + edits.size());
    impl->reused.reserve(attachments.size());
    impl->params_updates.reserve(edits.size());
    for (const auto &attachment : attachments) {
        impl->detached.push_back(Attachment{
            .info = attachment.info,
            .raw_component = attachment.raw_component,
        });
        impl->reused.push_back(attachment.info.handle);
    }

    const auto find_prepared = [&](const BehaviorAttachmentEdit &edit) {
        return std::find_if(
            impl->detached.begin(), impl->detached.end(),
            [&](const Attachment &attachment) {
                if (edit.identity.handle != invalidBehaviorAttachmentHandle) {
                    return attachment.info.handle == edit.identity.handle;
                }
                return attachment.info.entity == edit.entity &&
                       attachment.info.component_index == edit.component_index;
            });
    };
    const auto destroy_unpublished = [](Attachment &attachment) noexcept {
        if (attachment.instance.behavior != nullptr &&
            attachment.destroy != nullptr) {
            attachment.destroy(attachment.instance);
            attachment.destroy = nullptr;
            attachment.instance = {};
        }
    };

    for (auto &current : edits) {
        switch (current.kind) {
        case BehaviorAttachmentEditKind::attach: {
            const auto *registration =
                internal::getBehaviorRegisterer().findByName(current.stable_name);
            if (registration == nullptr &&
                availability == BehaviorRegistryAvailability::active) {
                throw BehaviorTypeUnknownError{
                    current.stable_name, {}, current.authoring_object_id};
            }
            if (current.entity == invalidGameObjectId) {
                throw std::runtime_error(
                    "behavior editor attachment requires a live entity");
            }
            if (current.identity.handle == invalidBehaviorAttachmentHandle) {
                throw std::runtime_error(
                    "behavior editor attach requires a reserved identity");
            }
            internal::RawBehaviorInstance instance;
            if (registration != nullptr) {
                instance = registration->create(current.canonical_params);
            }
            for (auto &candidate : impl->detached) {
                if (candidate.info.entity == current.entity &&
                    candidate.info.component_index >=
                        current.component_index) {
                    ++candidate.info.component_index;
                    refreshSceneAttachmentSequence(candidate.info);
                }
            }
            const auto effective_sequence =
                current.scene_ordered
                    ? sceneBehaviorAttachmentSeq(current.scene_object_index,
                                                 current.component_index)
                    : current.identity.attachment_seq;
            if (std::any_of(
                    impl->detached.begin(), impl->detached.end(),
                    [&](const Attachment &candidate) {
                        return candidate.info.handle ==
                                   current.identity.handle ||
                               candidate.info.attachment_seq ==
                                   effective_sequence;
                    })) {
                if (instance.behavior != nullptr && registration != nullptr) {
                    registration->destroy(instance);
                }
                throw std::runtime_error(
                    "behavior editor attach identity is already live");
            }
            Attachment next{
                .info = BehaviorAttachmentInfo{
                    .handle = current.identity.handle,
                    .attachment_seq = effective_sequence,
                    .entity = current.entity,
                    .component_index = current.component_index,
                    .component_origin = current.component_origin,
                    .scene_object_index = current.scene_object_index,
                    .scene_ordered = current.scene_ordered,
                    .stable_name = current.stable_name,
                    .canonical_params = registration != nullptr
                                            ? current.canonical_params
                                            : std::string{},
                    .owner = registration != nullptr
                                 ? registration->owner
                                 : internal::engineRegistrationOwner,
                    .pending = registration == nullptr,
                    .active = false,
                },
                .raw_component = std::move(current.raw_component),
                .instance = instance,
                .destroy = registration != nullptr ? registration->destroy
                                                   : nullptr,
                .initialized = false,
            };
            next.info.activation_delay = registration != nullptr ? 1 : 0;
            impl->detached.push_back(std::move(next));
            break;
        }
        case BehaviorAttachmentEditKind::remove: {
            if (current.entity == invalidGameObjectId) {
                throw std::runtime_error(
                    "behavior editor attachment requires a live entity");
            }
            const auto found = find_prepared(current);
            if (found == impl->detached.end() ||
                found->info.entity != current.entity ||
                found->info.component_index != current.component_index ||
                (current.identity.handle == invalidBehaviorAttachmentHandle &&
                 current.identity.attachment_seq != 0 &&
                 found->info.attachment_seq !=
                     current.identity.attachment_seq)) {
                throw std::runtime_error(
                    "behavior editor remove target is stale");
            }
            const auto reused = std::find(impl->reused.begin(),
                                          impl->reused.end(),
                                          found->info.handle);
            if (reused == impl->reused.end()) {
                destroy_unpublished(*found);
            } else {
                impl->reused.erase(reused);
            }
            impl->detached.erase(found);
            for (auto &candidate : impl->detached) {
                if (candidate.info.entity == current.entity &&
                    candidate.info.component_index >
                        current.component_index) {
                    --candidate.info.component_index;
                    refreshSceneAttachmentSequence(candidate.info);
                }
            }
            break;
        }
        case BehaviorAttachmentEditKind::set_params: {
            if (current.entity == invalidGameObjectId) {
                throw std::runtime_error(
                    "behavior editor attachment requires a live entity");
            }
            const auto found = find_prepared(current);
            if (found == impl->detached.end() ||
                found->info.entity != current.entity ||
                found->info.component_index != current.component_index ||
                found->info.pending ||
                (current.identity.handle == invalidBehaviorAttachmentHandle &&
                 current.identity.attachment_seq != 0 &&
                 found->info.attachment_seq !=
                     current.identity.attachment_seq)) {
                throw std::runtime_error(
                    "behavior editor params target is stale or pending");
            }
            const auto *registration =
                internal::getBehaviorRegisterer().findByNameAndOwner(
                    found->info.stable_name, found->info.owner);
            if (registration == nullptr) {
                throw std::runtime_error(
                    "behavior registration owner changed before params apply");
            }
            PreparedBehaviorAttachmentEdits::Impl::ParamsUpdate update{
                .handle = found->info.handle,
                .apply_params = registration->apply_prepared_params,
                .destroy_params = registration->destroy_prepared_params,
                .activation_failed = false,
                .activation_error = found->info.activation_failed
                                        ? std::string{}
                                        : found->info.activation_error,
                .activation_delay = found->info.activation_failed
                                        ? std::uint8_t{1}
                                        : found->info.activation_delay,
            };
            update.params =
                registration->prepare_params(current.canonical_params);
            try {
                impl->params_updates.push_back(std::move(update));
            } catch (...) {
                if (update.params.value != nullptr &&
                    update.destroy_params != nullptr) {
                    update.destroy_params(update.params);
                }
                throw;
            }
            found->info.canonical_params =
                std::move(current.canonical_params);
            found->raw_component = std::move(current.raw_component);
            if (found->info.activation_failed) {
                found->info.activation_failed = false;
                found->info.activation_error.clear();
                found->info.activation_delay = 1;
            }
            break;
        }
        case BehaviorAttachmentEditKind::insert_component:
            if (current.entity == invalidGameObjectId) {
                throw std::runtime_error(
                    "behavior editor attachment requires a live entity");
            }
            for (auto &candidate : impl->detached) {
                if (candidate.info.entity == current.entity &&
                    candidate.info.component_index >=
                        current.component_index) {
                    ++candidate.info.component_index;
                    refreshSceneAttachmentSequence(candidate.info);
                }
            }
            break;
        case BehaviorAttachmentEditKind::remove_component:
            if (current.entity == invalidGameObjectId) {
                throw std::runtime_error(
                    "behavior editor attachment requires a live entity");
            }
            for (auto &candidate : impl->detached) {
                if (candidate.info.entity == current.entity &&
                    candidate.info.component_index >
                        current.component_index) {
                    --candidate.info.component_index;
                    refreshSceneAttachmentSequence(candidate.info);
                }
            }
            break;
        }
    }

    std::sort(impl->detached.begin(), impl->detached.end(),
              [](const Attachment &left, const Attachment &right) {
                  return left.info.attachment_seq <
                         right.info.attachment_seq;
              });
    for (std::size_t index = 1; index < impl->detached.size(); ++index) {
        if (impl->detached[index - 1].info.attachment_seq ==
                impl->detached[index].info.attachment_seq ||
            impl->detached[index - 1].info.handle ==
                impl->detached[index].info.handle) {
            throw std::runtime_error(
                "behavior editor attachment identity is already live");
        }
    }
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
