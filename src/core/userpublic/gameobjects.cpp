#include "gameobjects.hpp"
#include "../ecs/core.hpp"
#include "../ecs/componentinfo.hpp"

namespace Pelican {

GameObjectId GameObjects::alloc(ComponentId *ids, void **ptrs, uint32_t components_count) {
    return GET_MODULE(ECSCore).allocateEntity(std::span{ids, components_count}, std::span{ptrs, components_count}, 1);
}
void GameObjects::commit(ComponentId *ids, void **ptrs, uint32_t components_count) {
    for (int i = 0; i < components_count; i++) {
        GET_MODULE(ComponentInfoManager).initComponent(ids[i], ptrs[i]);
    }
}

void GameObjects::remove(GameObjectId id) { GET_MODULE(ECSCore).remove(id); }

} // namespace Pelican
