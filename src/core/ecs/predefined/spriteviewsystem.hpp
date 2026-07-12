#pragma once

#include "../../container.hpp"
#include "transform.hpp"
#include <components/spriteview.hpp>
#include <details/ecs/coretemplate.hpp>

#include <span>

namespace Pelican {

DECLARE_MODULE(SpriteViewRenderSystem) {
  public:
    using Query = std::span<ChunkView<EntityId, TransformComponent, SpriteViewComponent>>;
    void process(Query chunks);
};

} // namespace Pelican
