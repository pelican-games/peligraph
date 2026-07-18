#pragma once

#include "../../gamecontext.hpp"
#include "../event/registerer.hpp"
#include "../reload/registrationowner.hpp"

#include <concepts>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace Pelican {

namespace internal {

using GameSystemUpdateFn = void (*)(GameContext &ctx);
using GameSystemEventFn = void (*)(const void *event, GameContext &ctx);
using GameSystemQueuedEventFn = void (*)(const QueuedEvent &event, GameContext &ctx);

struct GameSystemEventHandlerRegistration {
    std::type_index event_type = std::type_index{typeid(void)};
    GameSystemEventFn dispatch = nullptr;
};

struct GameSystemRegistration {
    std::string name;
    int order = 0;
    GameSystemUpdateFn update = nullptr;
    GameSystemQueuedEventFn dispatch_queued_event = nullptr;
    std::vector<GameSystemEventHandlerRegistration> event_handlers;
    RegistrationOwner owner = engineRegistrationOwner;
};

template <class System>
concept HasGameSystemUpdate = requires(System &system, GameContext &ctx) {
    { system.update(ctx) } -> std::same_as<void>;
};

template <class System, class Event>
concept HasGameSystemEvent = requires(System &system, const Event &event, GameContext &ctx) {
    { system.onEvent(event, ctx) } -> std::same_as<void>;
};

template <class System>
concept HasGameSystemQueuedEvent = requires(System &system, const QueuedEvent &event,
                                            GameContext &ctx) {
    { system.dispatchQueuedEvent(event, ctx) } -> std::same_as<void>;
};

template <class System> System &gameSystemInstance() {
    static System system;
    return system;
}

template <class System, class Event>
void appendGameSystemEventHandler(std::vector<GameSystemEventHandlerRegistration> &handlers) {
    if constexpr (HasGameSystemEvent<System, Event>) {
        handlers.push_back(GameSystemEventHandlerRegistration{
            .event_type = std::type_index{typeid(Event)},
            .dispatch =
                [](const void *event, GameContext &ctx) {
                    gameSystemInstance<System>().onEvent(*static_cast<const Event *>(event), ctx);
                },
        });
    }
}

template <class Lookup, int Index>
concept HasEventCatalogLookup = requires(Lookup lookup) {
    typename decltype(lookup.template operator()<Index>())::type;
};

template <class System, int Index, class Lookup>
void appendGameSystemEventHandlerAt(std::vector<GameSystemEventHandlerRegistration> &handlers, Lookup lookup) {
    if constexpr (HasEventCatalogLookup<Lookup, Index>) {
        using Event = typename decltype(lookup.template operator()<Index>())::type;
        appendGameSystemEventHandler<System, Event>(handlers);
    }
}

template <class System, class Lookup, int... Indices>
std::vector<GameSystemEventHandlerRegistration> collectGameSystemEventHandlers(std::integer_sequence<int, Indices...>,
                                                                               Lookup lookup) {
    std::vector<GameSystemEventHandlerRegistration> handlers;
    (appendGameSystemEventHandlerAt<System, Indices>(handlers, lookup), ...);
    return handlers;
}

class UserGameSystemRegistererTemplatePublic {
    std::vector<GameSystemRegistration> systems;

    PELICAN_API void __registerSystem(GameSystemRegistration registration);

    friend void unregisterGameSystems(RegistrationOwner owner) noexcept;

  public:
    template <class System>
    void registerSystem(std::string name, int order,
                        std::vector<GameSystemEventHandlerRegistration> event_handlers) {
        constexpr bool has_update = HasGameSystemUpdate<System>;
        constexpr bool has_queued_event = HasGameSystemQueuedEvent<System>;
        if constexpr (!has_update) {
            if (!has_queued_event && event_handlers.empty()) {
                throw std::runtime_error("registered game systems must define update(ctx) or onEvent(event, ctx)");
            }
        }

        GameSystemUpdateFn update = nullptr;
        if constexpr (has_update) {
            update = [](GameContext &ctx) { gameSystemInstance<System>().update(ctx); };
        }

        GameSystemQueuedEventFn dispatch_queued_event = nullptr;
        if constexpr (has_queued_event) {
            dispatch_queued_event = [](const QueuedEvent &event, GameContext &ctx) {
                gameSystemInstance<System>().dispatchQueuedEvent(event, ctx);
            };
        }

        __registerSystem(GameSystemRegistration{
            .name = std::move(name),
            .order = order,
            .update = update,
            .dispatch_queued_event = dispatch_queued_event,
            .event_handlers = std::move(event_handlers),
        });
    }

    const std::vector<GameSystemRegistration> &registeredSystems() const noexcept;
};

PELICAN_API UserGameSystemRegistererTemplatePublic &getGameSystemRegisterer();
std::vector<GameSystemRegistration> sortGameSystemRegistrations(std::vector<GameSystemRegistration> systems);
void updateRegisteredGameSystems(GameContext &ctx);
void dispatchEventToRegisteredGameSystems(const QueuedEvent &event, GameContext &ctx);
void unregisterGameSystems(RegistrationOwner owner) noexcept;
std::size_t gameSystemRegistrationCount(RegistrationOwner owner) noexcept;

} // namespace internal

} // namespace Pelican

#ifndef PELICAN_DETAIL_CONCAT_INNER
#define PELICAN_DETAIL_CONCAT_INNER(a, b) a##b
#endif
#ifndef PELICAN_DETAIL_CONCAT
#define PELICAN_DETAIL_CONCAT(a, b) PELICAN_DETAIL_CONCAT_INNER(a, b)
#endif
#define PELICAN_REGISTER_SYSTEM_IMPL(Type, order, unique_id)                                                        \
    namespace {                                                                                                      \
    struct PELICAN_DETAIL_CONCAT(PelicanGameSystemAutoRegister_, unique_id) {                                        \
        PELICAN_DETAIL_CONCAT(PelicanGameSystemAutoRegister_, unique_id)() {                                         \
            auto event_handlers = ::Pelican::internal::collectGameSystemEventHandlers<Type>(                         \
                std::make_integer_sequence<int, unique_id>{},                                                        \
                []<int Index>()                                                                                      \
                    -> decltype(pelicanEventCatalogEntry(::Pelican::internal::EventCatalogTag<Index>{})) {           \
                    return {};                                                                                       \
                });                                                                                                  \
            ::Pelican::internal::getGameSystemRegisterer().registerSystem<Type>(#Type, order,                        \
                                                                                std::move(event_handlers));          \
        }                                                                                                           \
    };                                                                                                              \
    static const PELICAN_DETAIL_CONCAT(PelicanGameSystemAutoRegister_, unique_id)                                    \
        PELICAN_DETAIL_CONCAT(pelican_game_system_auto_register_, unique_id);                                        \
    }
#define PELICAN_REGISTER_SYSTEM(Type, order) PELICAN_REGISTER_SYSTEM_IMPL(Type, order, __COUNTER__)
