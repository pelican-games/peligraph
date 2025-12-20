#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "transform.hpp"
#include <components/localtransform.hpp>

namespace Pelican {

DECLARE_MODULE(LocalTransformSystem) {
    struct TempTransform {
        LocalTransformComponent *local;
        TransformComponent *dst_ptr;
    };
    std::vector<TempTransform> tmpbuf;
    std::vector<bool> tmpappearbuf;

  public:
    using Query = std::span<ChunkView<EntityId, TransformComponent, LocalTransformComponent>>;
    void process(Query chunks);
};

} // namespace Pelican
