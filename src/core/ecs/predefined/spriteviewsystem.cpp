#include "spriteviewsystem.hpp"

#include "../../renderer/spritescene.hpp"
#include "../../renderingpass/renderingpasscontainer.hpp"

#include <stdexcept>

namespace Pelican {
namespace {

std::vector<SpriteSceneItem> collectSpriteItems(SpriteViewRenderSystem::Query chunks) {
    std::vector<SpriteSceneItem> items;
    for (const auto &chunk : chunks) {
        const auto entities = std::get<EntityId *>(chunk.components);
        const auto transforms = std::get<TransformComponent *>(chunk.components);
        const auto views = std::get<SpriteViewComponent *>(chunk.components);
        for (std::size_t i = 0; i < chunk.count; ++i)
            items.push_back({entities[i], &transforms[i], &views[i]});
    }
    return items;
}

} // namespace

void SpriteViewRenderSystem::prepareEcsWorkerDependencies(Query chunks) {
    rendering_passes = FastModuleContainer::tryGet<RenderingPassContainer>();
    sprite_scene = FastModuleContainer::tryGet<SpriteScene>();
    if (rendering_passes != nullptr && rendering_passes->isFeatureEnabled("sprite") &&
        sprite_scene == nullptr) {
        sprite_scene = &GET_MODULE(SpriteScene);
    }
    if (sprite_scene != nullptr && rendering_passes != nullptr &&
        rendering_passes->isFeatureEnabled("sprite")) {
        const auto items = collectSpriteItems(chunks);
        sprite_scene->prepareAssets(items);
    }
}

void SpriteViewRenderSystem::process(Query chunks) {
    if (rendering_passes == nullptr || !rendering_passes->isFeatureEnabled("sprite")) {
        if (sprite_scene != nullptr) sprite_scene->clear();
        return;
    }
    auto items = collectSpriteItems(chunks);
    if (sprite_scene == nullptr)
        throw std::logic_error("SpriteViewRenderSystem dependencies were not prepared on the ECS owner thread");
    sprite_scene->rebuild(items);
}

} // namespace Pelican
