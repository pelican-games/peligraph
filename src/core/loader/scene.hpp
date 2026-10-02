#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include "resolvedscene.hpp"
#include "sceneload.hpp"
#include "../userpublic/object_ref.hpp"
#include "../userpublic/valuegraph.hpp"
#include <details/ecs/entity.hpp>

#include <array>
#include <filesystem>
#include <glm/ext/quaternion_float.hpp>
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Pelican {

class SceneLoader;
class ProjectBasicConfig;
// The scene-load transaction's shared state.  Defined in scene.cpp, where the
// participants that publish into the loader's own fields live; it is the one
// seam allowed to reach them, so no participant needs friendship of its own.
struct SceneLoadContext;
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

// The scene run game code addresses: the run being loaded while a load's
// callbacks run, otherwise the live one. `scene` is its resolved scene, null
// when no scene is loaded, and `resolved` the resolved document it belongs
// to (its prefab registry, bindable provider and defaults).
struct SceneObjectRun {
    SceneAuthority authority{};
    const ResolvedSceneView *scene = nullptr;
    const ResolvedScene *resolved = nullptr;
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
        explicit GraphRuntime(const ValueGraph::Graph &graph)
            : evaluation(graph.createState()), pose_evaluation(graph.createState()) {}
        ~GraphRuntime();
        GraphRuntime(const GraphRuntime &) = delete;
        GraphRuntime &operator=(const GraphRuntime &) = delete;
        GraphParameterOverrides parameters;
        ValueGraph::EvaluationState evaluation;
        // The pose feed evaluates the same document with its own state, so its
        // clocks advance once per frame without touching the rendering
        // evaluation. It lives here, beside the rendering state, so a graph
        // generation change carries both or neither -- one half surviving a
        // republish while the other restarts at 0 would make the same node
        // produce two different values in one frame.
        ValueGraph::EvaluationState pose_evaluation;
        std::map<std::string, GameObjectId> owners;
        ValueGraph::SoundBindings sounds;
        // The start (AudioVoiceDirective::epoch) each voice in `sounds` plays.
        std::map<std::string, std::uint64_t, std::less<>> sound_epochs;
        // CT-1: what each voice in `sounds` was last made to do -- held or
        // running, and the source second the graph put it at.
        struct VoiceStatus {
            bool held = false;
            double position_seconds = 0.0;
        };
        std::map<std::string, VoiceStatus, std::less<>> voice_status;
    };
    using GraphRuntimes = std::map<std::weak_ptr<const ValueGraph::Graph>, GraphRuntime, std::owner_less<>>;
    GraphRuntimes graph_runtime;
    // CT-1: the graph whose voices the frames drive (graphAudioWriters), so
    // its audio.playback nodes are listed even when no runtime holds them yet.
    std::weak_ptr<const ValueGraph::Graph> voice_graph;
    ValueGraph::PublishedInstanceInputs graph_values;
    GraphRuntime &synchronizeGraphRuntime(const std::shared_ptr<const ValueGraph::Graph> &graph);
    // Scopes whose graph instance owner has resolved to a live object at least
    // once in the scene that is loaded now, and the scopes already named in the
    // log.  Both are scene-scoped rather than graph-generation-scoped on
    // purpose: removing an object and republishing the document makes a new
    // generation every time, and a memo that reset with it would re-announce
    // the same scope on every republish -- the spam this exists to avoid.
    std::set<std::string, std::less<>> graph_owner_resolved_scopes;
    std::set<std::string, std::less<>> graph_owner_reported_scopes;
    void reportUnresolvedGraphOwners(const ValueGraph::Graph &graph,
                                     const std::map<std::string, GameObjectId> &owners);
    bool ownsGraphParameter(const ValueGraph::Graph &graph, GameObjectId object,
                            std::string_view parameter) const;
    bool ownsGraphOwner(const GraphInstanceOwner *owner, GameObjectId object) const;
    std::uint64_t next_authority_epoch = 1;
    std::uint64_t run_nonce = 0;
    SceneLoadFaultInjector *fault_injector = nullptr;
    std::array<SceneLoadDiagnostics, 5> diagnostics_by_root;
    static SceneLoadFaultInjector *default_fault_injector_for_testing;
    struct RetirementState;
    std::unique_ptr<RetirementState> retirement_state;

    friend struct SceneLoadContext;
    friend void test_support::setSceneLoadFaultInjectorForTesting(
        SceneLoader &loader, SceneLoadFaultInjector *injector) noexcept;
    friend void test_support::setDefaultSceneLoadFaultInjectorForTesting(
        SceneLoadFaultInjector *injector) noexcept;

    void bindObjectTransform(const std::string &name, GameObjectId object_id);
    void bindRuntimeObject(AuthoringObjectId authoring_object_id,
                           GameObjectId object_id);
    void clearRuntimeScene();
    void releaseTransientModels(bool deferred) noexcept;

    // MetadataCommitToken's staging surface and ProjectBasicConfig's borrowed
    // metadata factory are both private to SceneLoader.  The metadata
    // participant of a scene load drives them through these three, so the
    // participants themselves need no friendship of their own.
    static MetadataCommitToken prepareLoadMetadata(
        ProjectBasicConfig &config, SceneLoadRequest &request,
        SceneAuthority authority, SceneLoadPhaseTrace &phase_trace);
    static void publishLoadMetadataDocumentOnly(
        MetadataCommitToken &metadata) noexcept;
    static void commitLoadMetadata(MetadataCommitToken &metadata,
                                   SceneLoadPhaseTrace &phase_trace) noexcept;

  public:
    SceneLoader();
    ~SceneLoader();
    std::optional<GameObjectId> graphOwnerObject(const GraphInstanceOwner &owner) const;
    std::map<std::string, GameObjectId> graphOwnerObjects(const ValueGraph::Graph &graph) const;

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
    // The animation partition's own evaluation state for the same document.
    ValueGraph::EvaluationState &poseGraphEvaluationState(
        const std::shared_ptr<const ValueGraph::Graph> &graph);
    bool emitGraphEvent(std::shared_ptr<const ValueGraph::Graph> graph, GameObjectId object,
                        std::string_view input, Animation::CurvePlaybackCommand command);
    // AU-1: make one audio.playback node's voice what the graph asks for this
    // frame -- start it (again, under a new epoch), stop it, hold it, or keep
    // it on its position. False when the node has no voice to drive here.
    bool applyGraphVoice(const std::shared_ptr<const ValueGraph::Graph> &graph,
                         std::string_view node, const ValueGraph::AudioVoiceDirective &directive);
    // An audio.volume / audio.pitch sink's value. A volume is reached over
    // this frame's length (decision 6), not at once.
    bool applyGraphAudioValue(const std::shared_ptr<const ValueGraph::Graph> &graph, std::string_view node,
                              bool voice, ValueGraph::AudioValueKind kind, float value);
    std::optional<SoundHandle> graphSound(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                        std::string_view node) const;
    // CT-1: each audio.playback node of the graph the frames drive, by its
    // graph node id: idle (no voice: not started, reset, or stopped by a scene
    // commit), paused (the voice is held: its own pause, a clock or position
    // standing still, a delay not reached) or playing; and the source second
    // the graph last put it at (0 when idle). Read from what the last
    // publication asked of each voice, not from the mixer, so a null backend
    // reports the same. The whole-mix hold of a paused game is the caller's
    // to add.
    struct GraphVoiceStatus {
        enum class State : std::uint8_t { idle, playing, paused };
        std::string node;
        State state = State::idle;
        double position_seconds = 0.0;
    };
    std::vector<GraphVoiceStatus> graphVoiceStatus() const;
    class PreparedGraphRuntimeTransfer;
    // The audio half of one frame's Graph::publish, shared by the renderer and
    // by a caller without one (the device-less audio tests), so both drive
    // voices through the same code. With a staged generation change the
    // writers stage into it; the transfer applies them once it is published.
    struct GraphAudioWriters {
        ValueGraph::AudioVoiceWriter voices;
        ValueGraph::SoundResolver sounds;
        ValueGraph::AudioValueWriter values;
    };
    GraphAudioWriters graphAudioWriters(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                        PreparedGraphRuntimeTransfer *staged);
    // AU-1 decision 1: a scene commit stops every voice a graph started in the
    // scene it replaces. A voice that should carry on is started again by the
    // next scene's owner of the same name (decision 8).
    void stopGraphVoices() noexcept;
    // What the last published draw generation fed each instance input port.
    // Written by the publishing pass; read-only afterwards. Evaluating the
    // graph again to answer the question would advance its clocks and consume
    // its events, so a reader reports the value that was drawn, or none.
    void publishGraphValues(ValueGraph::PublishedInstanceInputs snapshot);
    const ValueGraph::PublishedInstanceInputs &graphValues() const noexcept { return graph_values; }
    void updateGraphRuntime();
    std::unique_ptr<PreparedGraphRuntimeTransfer> prepareGraphRuntimeTransfer(
        const std::shared_ptr<const ValueGraph::Graph> &previous,
        const std::shared_ptr<const ValueGraph::Graph> &next);
    nlohmann::ordered_json diagnosticsStatusJson() const;
    const std::optional<SceneId> &pendingSceneForTesting() const noexcept {
        return pending_scene_id;
    }
    // GP-1: the one place a name becomes an ObjectRef. The name is looked up
    // in the scene run that is live -- inside a scene load's callbacks, the
    // run being loaded -- and the first declared object of that name wins.
    Resolved<ObjectRef> resolveObject(std::string_view name) const;
    // The live object a ref names, refused as stale when the ref belongs to
    // another scene run.
    Resolved<GameObjectId> materializeObject(const ObjectRef &ref) const;
    // GP-2: the run resolveObject names objects in, which is also the run a
    // runtime spawn is queued into.
    SceneObjectRun objectRun() const;
    std::optional<GameObjectId> objectId(std::string_view name) const;
    std::optional<std::string> objectName(GameObjectId object) const;
    bool hasObjectTransform(std::string_view name) const;
    SceneObjectTransform objectTransform(std::string_view name) const;
    void applyObjectTransform(std::string_view name, const SceneObjectTransform &transform);
    std::filesystem::path loadTransientGltf(std::string_view path_ref, const std::optional<std::string> &name);
    bool hasRuntimeOnlyChanges() const noexcept { return runtime_only_changes; }
};

class SceneLoader::PreparedGraphRuntimeTransfer {
    friend class SceneLoader;
    PreparedGraphRuntimeTransfer(SceneLoader &scene, std::shared_ptr<const ValueGraph::Graph> previous)
        : scene_(scene), previous_(std::move(previous)) {}
    SceneLoader &scene_;
    std::shared_ptr<const ValueGraph::Graph> previous_;
    GraphRuntimes prepared_;
    // Borrowed until publication. Destroying a rejected candidate cannot stop
    // voices still owned by the old runtime.
    ValueGraph::SoundBindings retained_sounds_;
    std::map<std::string, std::uint64_t, std::less<>> retained_sound_epochs_;
    struct AudioValue {
        std::string node;
        ValueGraph::AudioValueKind kind;
        float value;
    };
    std::vector<std::pair<std::string, ValueGraph::AudioVoiceDirective>> audio_voices_;
    std::vector<AudioValue> audio_values_;
    bool committed_ = false;
  public:
    ValueGraph::ParameterValues parameters() const;
    ValueGraph::EvaluationState &evaluation();
    bool stageAudioVoice(std::string_view node, const ValueGraph::AudioVoiceDirective &directive);
    bool stageAudioValue(std::string_view node, bool voice, ValueGraph::AudioValueKind kind, float value);
    void publishAudio(const std::shared_ptr<const ValueGraph::Graph> &graph);
    void commit() noexcept;
};

} // namespace Pelican
