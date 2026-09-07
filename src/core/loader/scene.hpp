#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include "resolvedscene.hpp"
#include "sceneload.hpp"
#include "../userpublic/valuegraph.hpp"
#include <details/ecs/entity.hpp>

#include <array>
#include <filesystem>
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

class SceneLoader;
namespace test_support {
void setSceneLoadFaultInjectorForTesting(
    SceneLoader &loader, SceneLoadFaultInjector *injector) noexcept;
void setDefaultSceneLoadFaultInjectorForTesting(
    SceneLoadFaultInjector *injector) noexcept;
} // namespace test_support

struct SceneObjectTransform {
    glm::vec3 pos;
    glm::quat rotation;
    glm::vec3 scale;
};

struct SceneRuntimeObjectBinding {
    AuthoringObjectId authoring_object_id{};
    GameObjectId object_id = invalidGameObjectId;
};

namespace internal {
// Pure resolved-projection gate used before SceneLoader mutates any runtime
// state. It is also the configuration-matrix seam for named feature errors.
void validateResolvedSceneFeatureBindings(const ResolvedSceneView &scene);
} // namespace internal

DECLARE_MODULE(SceneLoader) {
    struct ObjectBinding {
        GameObjectId object_id;
    };

    std::unordered_map<std::string, ObjectBinding> object_bindings;
    std::vector<SceneRuntimeObjectBinding> runtime_object_bindings;
    std::vector<ModelTemplate> transient_models;
    std::optional<SceneId> pending_scene_id;
    bool runtime_only_changes = false;
    double scene_start_time = 0;
    struct GraphParameterOverride {
        GameObjectId object;
        double value;
    };
    using GraphParameterOverrides = std::map<std::string, GraphParameterOverride, std::less<>>;
    struct GraphRuntime {
        explicit GraphRuntime(const ValueGraph::Graph &graph) : evaluation(graph.createState()) {}
        ~GraphRuntime();
        GraphRuntime(const GraphRuntime &) = delete;
        GraphRuntime &operator=(const GraphRuntime &) = delete;
        GraphParameterOverrides parameters;
        ValueGraph::EvaluationState evaluation;
        std::map<std::string, GameObjectId> owners;
        ValueGraph::SoundBindings sounds;
    };
    std::map<std::weak_ptr<const ValueGraph::Graph>, GraphRuntime, std::owner_less<>> graph_runtime;
    GraphRuntime &synchronizeGraphRuntime(const std::shared_ptr<const ValueGraph::Graph> &graph);
    bool ownsGraphParameter(const ValueGraph::Graph &graph, GameObjectId object,
                            std::string_view parameter) const;
    bool ownsGraphOwner(const GraphInstanceOwner *owner, GameObjectId object) const;
    std::uint64_t next_authority_epoch = 1;
    std::uint64_t run_nonce = 0;
    SceneLoadFaultInjector *fault_injector = nullptr;
    std::array<SceneLoadDiagnostics, 6> diagnostics_by_root;
    static SceneLoadFaultInjector *default_fault_injector_for_testing;
    struct RetirementState;
    std::unique_ptr<RetirementState> retirement_state;

    friend void test_support::setSceneLoadFaultInjectorForTesting(
        SceneLoader &loader, SceneLoadFaultInjector *injector) noexcept;
    friend void test_support::setDefaultSceneLoadFaultInjectorForTesting(
        SceneLoadFaultInjector *injector) noexcept;

    void bindObjectTransform(const std::string &name, GameObjectId object_id);
    void bindRuntimeObject(AuthoringObjectId authoring_object_id,
                           GameObjectId object_id);
    void clearRuntimeScene();
    void releaseTransientModels(bool deferred) noexcept;

  public:
    SceneLoader();
    ~SceneLoader();
    std::optional<GameObjectId> graphOwnerObject(const GraphInstanceOwner &owner) const;

    SceneLoadResult load(SceneLoadRequest request);
    void requestLoad(SceneId scene_id);
    // onDestroy resolves against committed metadata while retaining the
    // transaction's pending candidate as the write target.
    void requestLoadDuringRetire(SceneId scene_id);
    bool applyPendingLoad();
    // Consumes retirement prepared by the preceding committed load at the
    // next frame (or before ECS teardown).
    void drainRetirementQueue();
    const SceneId &currentScene() const;
    std::span<const SceneRuntimeObjectBinding> runtimeObjectBindings() const
        noexcept {
        return runtime_object_bindings;
    }
    void publishRuntimeObjectBindings(std::vector<SceneRuntimeObjectBinding> &prepared) noexcept {
        runtime_object_bindings.swap(prepared);
    }
    std::uint64_t runtimeSceneEpoch() const noexcept;
    SceneAuthority activeAuthority() const noexcept;
    double sceneStartTime() const noexcept { return scene_start_time; }
    bool setGraphParameter(std::shared_ptr<const ValueGraph::Graph> graph,
                           GameObjectId object, std::string_view parameter, std::optional<double> value);
    ValueGraph::ParameterValues graphParameterValues(std::shared_ptr<const ValueGraph::Graph> graph);
    ValueGraph::InactiveScopes inactiveGraphScopes(const ValueGraph::Graph &graph) const;
    ValueGraph::EvaluationState &graphEvaluationState(const std::shared_ptr<const ValueGraph::Graph> &graph);
    bool emitGraphEvent(std::shared_ptr<const ValueGraph::Graph> graph, GameObjectId object,
                        std::string_view input, Animation::CurvePlaybackCommand command);
    bool applyGraphAudioEvent(const std::shared_ptr<const ValueGraph::Graph> &graph,
                             std::string_view node, Animation::CurvePlaybackCommand command);
    std::optional<SoundHandle> graphSound(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                        std::string_view node) const;
    void updateGraphRuntime();
    nlohmann::ordered_json diagnosticsStatusJson() const;
    const std::optional<SceneId> &pendingSceneForTesting() const noexcept {
        return pending_scene_id;
    }
    std::optional<GameObjectId> objectId(std::string_view name) const;
    std::optional<std::string> objectName(GameObjectId object) const;
    bool hasObjectTransform(std::string_view name) const;
    SceneObjectTransform objectTransform(std::string_view name) const;
    void applyObjectTransform(std::string_view name, const SceneObjectTransform &transform);
    std::filesystem::path loadTransientGltf(std::string_view path_ref, const std::optional<std::string> &name);
    bool hasRuntimeOnlyChanges() const noexcept { return runtime_only_changes; }
};

} // namespace Pelican
