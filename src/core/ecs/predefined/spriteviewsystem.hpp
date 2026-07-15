#pragma once

#include "../../container.hpp"
#include "transform.hpp"
#include <components/spriteview.hpp>
#include <details/ecs/coretemplate.hpp>

#include <span>

namespace Pelican {

class RenderingPassContainer;
class SpriteScene;

DECLARE_MODULE(SpriteViewRenderSystem) {
    RenderingPassContainer *rendering_passes = nullptr;
    SpriteScene *sprite_scene = nullptr;

  public:
    using Query = std::span<ChunkView<EntityId, TransformComponent, SpriteViewComponent>>;
    void prepareEcsWorkerDependencies(Query chunks);
    void process(Query chunks);
};

} // namespace Pelican
