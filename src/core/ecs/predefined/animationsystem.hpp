#pragma once

#include "../../container.hpp"
#include <components/animation.hpp>
#include <components/modelview.hpp>
#include <components/predefined.hpp>
#include <animation/animgraph.hpp>
#include <details/ecs/coretemplate.hpp>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

class EngineTime;
class ModelAssetContainer;
class PathResolver;
class PolygonInstanceContainer;
struct AnimationSystemState;

// AG-3a. What one animation component was doing on the frame that just ran.
// The two component paths answer from different places -- a graph keeps its own
// evaluator, a clip keeps nothing at all -- so this is the one shape the status
// RPC reads either of them through.
struct AnimationComponentTrace {
    EntityId object{};
    // Empty when the object is not a named scene object; the component path
    // registers such an object under a generated name that means nothing
    // outside the animation service.
    std::string object_name;
    // The document or clip reference the component names. `graph` says which
    // of the two, and so which of the two sources this sink is.
    bool graph = false;
    std::string reference;
    std::string state;
    std::vector<AnimationGraph::CursorStatusV1> cursors;
    bool transition_active = false;
    std::string transition_target;
    double transition_progress = 0.0;
    std::uint64_t frame_revision = 0;
};

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
    // Evaluate the animation partition of every published common graph, on the
    // owner thread, before the animation phases run. Scalars come from this
    // frame's value evaluation of the same document, so the feed is one-way.
    void updatePoseGraphs();
    // One entry per animation component that is driving a pose right now, in
    // scene object order. A graph's entry is copied out of the evaluator's own
    // status; a clip's is computed from the same three playback numbers the
    // clip path samples with, so the phase reported is the phase drawn rather
    // than a second bookkeeping that can drift away from it.
    std::vector<AnimationComponentTrace> componentTrace();
    Animation::Status setGraphParameter(EntityId entity, std::string_view name, double value);
    Animation::Status forceGraphState(EntityId entity, std::string_view name);
};

} // namespace Pelican
