#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "transform.hpp"
#include <components/localtransform.hpp>
#include <components/predefined.hpp>

namespace Pelican {

DECLARE_MODULE(LocalTransformSystem) {
    ECSCoreTemplatePublic *ecs = nullptr;

  public:
    using Query = std::span<ChunkView<EntityId, TransformComponent, LocalTransformComponent>>;
    void prepareEcsWorkerDependencies(bool has_matching_chunks);
    void process(Query chunks);
};

} // namespace Pelican
