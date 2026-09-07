#pragma once

#include "../../container.hpp"
#include <components/animation.hpp>
#include <components/modelview.hpp>
#include <components/predefined.hpp>
#include <animation/animgraph.hpp>
#include <details/ecs/coretemplate.hpp>
#include <memory>

namespace Pelican {

class EngineTime;
class ModelAssetContainer;
class PathResolver;
class PolygonInstanceContainer;
struct AnimationSystemState;

DECLARE_MODULE(AnimationSystem) {
    std::unique_ptr<AnimationSystemState> state;
    EngineTime *engine_time = nullptr;
    PolygonInstanceContainer *instances = nullptr;
    ModelAssetContainer *models = nullptr;
    PathResolver *path_resolver = nullptr;

  public:
    using QueryComponents = std::tuple<EntityId *, AnimationComponent *, SimpleModelViewComponent *>;
    using DependencyQuery = std::span<ChunkView<EntityId, AnimationComponent, SimpleModelViewComponent>>;
    AnimationSystem();
    ~AnimationSystem();
    void prepareEcsWorkerDependencies(DependencyQuery chunks);
    void process(QueryComponents components, size_t count);
    void updateGraphs();
    Animation::Status setGraphParameter(EntityId entity, std::string_view name, double value);
    Animation::Status forceGraphState(EntityId entity, std::string_view name);
};

} // namespace Pelican
