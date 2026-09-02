#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <components/predefined.hpp>
#include <span>
#include <optional>
#include <unordered_map>

#include "modelview.hpp"
#include <components/rayinstance.hpp>

namespace Pelican {

class ModelAssetContainer;
class PolygonInstanceContainer;

DECLARE_MODULE(SimpleModelViewUpdateSystem) {
    ModelAssetContainer *models = nullptr;
    PolygonInstanceContainer *instances = nullptr;
    std::unordered_map<EntityId, std::optional<RayInstanceComponent>>
        prepared_ray_instances;

  public:
    using QueryComponents =
        std::tuple<EntityId *, SimpleModelViewComponent *>;
    using DependencyQuery =
        std::span<ChunkView<EntityId, SimpleModelViewComponent>>;
    void prepareEcsWorkerDependencies(DependencyQuery chunks);
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
