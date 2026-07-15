#pragma once

#include "../../container.hpp"
#include <components/animation.hpp>
#include <components/modelview.hpp>
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
    using QueryComponents = std::tuple<AnimationComponent *, SimpleModelViewComponent *>;
    AnimationSystem();
    ~AnimationSystem();
    void prepareEcsWorkerDependencies(bool has_matching_chunks);
    void process(QueryComponents components, size_t count);
};

} // namespace Pelican
