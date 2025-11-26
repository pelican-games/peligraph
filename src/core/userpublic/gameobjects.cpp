#include "gameobjects.hpp"
#include "../ecs/core.hpp"

namespace Pelican {

GameObjectId GameObjects::add(ComponentId *ids, void **ptrs, uint32_t components_count) {
    return GET_MODULE(ECSCore).allocateEntity(std::span{ids, components_count}, std::span{ptrs, components_count}, 1);
}

void GameObjects::remove(GameObjectId id) { GET_MODULE(ECSCore).remove(id); }

} // namespace Pelican
