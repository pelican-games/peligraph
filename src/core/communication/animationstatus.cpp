#include "animationstatus.hpp"

#if PELICAN_WITH_ANIMATION
#include "../animation/posegraphruntime.hpp"
#include "../container.hpp"
#include "../ecs/predefined/animationsystem.hpp"
#include "../loader/scene.hpp"

#include <string>
#include <utility>
#endif

namespace Pelican::internal {

#if PELICAN_WITH_ANIMATION
namespace {
using Json = nlohmann::json;

Json cursorsJson(const std::vector<Animation::PoseGraphRuntime::CursorTrace> &cursors) {
    auto result = Json::array();
    for (const auto &cursor : cursors)
        result.push_back(Json{{"clip", cursor.clip},
                              {"time_seconds", cursor.time_seconds},
                              {"normalized_phase", cursor.normalized_phase}});
    return result;
}

Json cursorsJson(const std::vector<AnimationGraph::CursorStatusV1> &cursors) {
    auto result = Json::array();
    for (const auto &cursor : cursors)
        result.push_back(Json{{"clip", cursor.clip},
                              {"time_seconds", cursor.time_seconds},
                              {"normalized_phase", cursor.normalized_phase}});
    return result;
}

Json ownerJson(const std::string &scene, std::uint64_t object) {
    return Json{{"scene", scene}, {"object", object}};
}
} // namespace
#endif

nlohmann::json animationStatusJson() {
#if !PELICAN_WITH_ANIMATION
    return nlohmann::json{{"available", false}};
#else
    auto sinks = Json::array();
    // The pose graph's sinks first, in the sink order its own trace is sorted
    // in, so two reads of the same frame cannot disagree about the order.
    for (const auto &trace : Animation::poseGraphRuntime().trace()) {
        Json sink{
            {"owner", ownerJson(trace.owner.scene, trace.owner.object)},
            {"node", trace.node},
            {"source", Animation::PoseGraphRuntime::sinkSourceName(trace.source)},
            {"cursors", cursorsJson(trace.cursors)},
            {"frame_revision", trace.frame_revision},
            {"pose_hash", trace.pose_hash}};
        // A sink with no state machine in its chain has no state and no
        // transition; the keys are absent rather than empty, so "idle" and
        // "there is no machine here" cannot be read as the same answer.
        if (!trace.state.empty()) sink["state"] = trace.state;
        if (trace.source == Animation::PoseGraphRuntime::SinkSource::state_machine)
            sink["transition"] = Json{{"active", trace.transition_active},
                                      {"target", trace.transition_target},
                                      {"progress", trace.transition_progress}};
        // AG-2's displacement, and only on a chain that extracts one. It is the
        // amount in the clip's own space, before the individual's rotation
        // turns it, so a rotated individual's travel is not this number.
        if (trace.root_motion)
            sink["root_delta"] = Json::array({trace.root_delta[0], trace.root_delta[1],
                                              trace.root_delta[2]});
        sinks.push_back(std::move(sink));
    }
    // Then the animation component's two paths. They are sinks in the same
    // sense -- one object, one writer -- and leaving them out would make a
    // scene animated by components look like a scene with no animation at all.
    if (auto *animation = FastModuleContainer::tryGet<AnimationSystem>()) {
        auto &scene = GET_MODULE(SceneLoader);
        const auto authoringId = [&](EntityId object) -> std::uint64_t {
            for (const auto &binding : scene.runtimeObjectBindings())
                if (binding.object_id == object) return binding.authoring_object_id.value;
            return 0;
        };
        for (const auto &trace : animation->componentTrace()) {
            Json sink{{"owner", ownerJson(scene.currentScene(), authoringId(trace.object))},
                      {"node", trace.reference},
                      {"source", trace.graph ? "component_graph" : "component_clip"},
                      {"cursors", cursorsJson(trace.cursors)},
                      {"frame_revision", trace.frame_revision},
                      // The component path publishes straight to the renderer
                      // and keeps no pose hash; 0 says "not recorded here",
                      // which is why the key is present rather than invented.
                      {"pose_hash", 0}};
            if (!trace.state.empty()) sink["state"] = trace.state;
            if (trace.graph)
                sink["transition"] = Json{{"active", trace.transition_active},
                                          {"target", trace.transition_target},
                                          {"progress", trace.transition_progress}};
            sinks.push_back(std::move(sink));
        }
    }
    return Json{{"available", true}, {"sinks", std::move(sinks)}};
#endif
}

} // namespace Pelican::internal
