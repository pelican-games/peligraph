#pragma once

#include "../../container.hpp"
#include <details/ecs/coretemplate.hpp>
#include <span>

#include "modelview.hpp"

namespace Pelican {

class ModelAssetContainer;
class PolygonInstanceContainer;

DECLARE_MODULE(SimpleModelViewUpdateSystem) {
    ModelAssetContainer *models = nullptr;
    PolygonInstanceContainer *instances = nullptr;

  public:
    using QueryComponents = std::tuple<SimpleModelViewComponent *>;
    using DependencyQuery = std::span<ChunkView<SimpleModelViewComponent>>;
    void prepareEcsWorkerDependencies(DependencyQuery chunks);
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
