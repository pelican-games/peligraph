#include "spriteviewsystem.hpp"

#include "../../renderer/spritescene.hpp"
#include "../../renderingpass/renderingpasscontainer.hpp"

namespace Pelican {

void SpriteViewRenderSystem::process(Query chunks) {
    if (!FastModuleContainer::isInitialized<RenderingPassContainer>() ||
        !GET_MODULE(RenderingPassContainer).isFeatureEnabled("sprite")) {
        if (FastModuleContainer::isInitialized<SpriteScene>()) GET_MODULE(SpriteScene).clear();
        return;
    }
    std::vector<SpriteSceneItem> items;
    for (const auto &chunk : chunks) {
        const auto entities = std::get<EntityId *>(chunk.components);
        const auto transforms = std::get<TransformComponent *>(chunk.components);
        const auto views = std::get<SpriteViewComponent *>(chunk.components);
        for (std::size_t i = 0; i < chunk.count; ++i)
            items.push_back({entities[i], &transforms[i], &views[i]});
    }
    GET_MODULE(SpriteScene).rebuild(items);
}

} // namespace Pelican
