#pragma once

#include "../../gamecontext.hpp"

#include <concepts>
#include <string>
#include <vector>

namespace Pelican {

namespace internal {

using GameSystemUpdateFn = void (*)(GameContext &ctx);

struct GameSystemRegistration {
    std::string name;
    int order = 0;
    GameSystemUpdateFn update = nullptr;
};

template <class System>
concept HasGameSystemUpdate = requires(System &system, GameContext &ctx) {
    { system.update(ctx) } -> std::same_as<void>;
};

class UserGameSystemRegistererTemplatePublic {
    std::vector<GameSystemRegistration> systems;

    void __registerSystem(GameSystemRegistration registration);

  public:
    template <class System>
        requires HasGameSystemUpdate<System>
    void registerSystem(std::string name, int order) {
        __registerSystem(GameSystemRegistration{
            .name = std::move(name),
            .order = order,
            .update = [](GameContext &ctx) {
                static System system;
                system.update(ctx);
            },
        });
    }

    const std::vector<GameSystemRegistration> &registeredSystems() const noexcept;
};

UserGameSystemRegistererTemplatePublic &getGameSystemRegisterer();
std::vector<GameSystemRegistration> sortGameSystemRegistrations(std::vector<GameSystemRegistration> systems);
void updateRegisteredGameSystems(GameContext &ctx);

} // namespace internal

} // namespace Pelican

#define PELICAN_DETAIL_CONCAT_INNER(a, b) a##b
#define PELICAN_DETAIL_CONCAT(a, b) PELICAN_DETAIL_CONCAT_INNER(a, b)
#define PELICAN_REGISTER_SYSTEM_IMPL(Type, order, unique_id)                                                        \
    namespace {                                                                                                      \
    struct PELICAN_DETAIL_CONCAT(PelicanGameSystemAutoRegister_, unique_id) {                                        \
        PELICAN_DETAIL_CONCAT(PelicanGameSystemAutoRegister_, unique_id)() {                                         \
            ::Pelican::internal::getGameSystemRegisterer().registerSystem<Type>(#Type, order);                       \
        }                                                                                                           \
    };                                                                                                              \
    static const PELICAN_DETAIL_CONCAT(PelicanGameSystemAutoRegister_, unique_id)                                    \
        PELICAN_DETAIL_CONCAT(pelican_game_system_auto_register_, unique_id);                                        \
    }
#define PELICAN_REGISTER_SYSTEM(Type, order) PELICAN_REGISTER_SYSTEM_IMPL(Type, order, __COUNTER__)
