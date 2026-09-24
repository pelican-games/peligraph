#pragma once

#include "components/localtransform.hpp"
#include "components/spriteview.hpp"
#include "animation/abi_v1.hpp"
#include "animation/curve.hpp"
#include "audio.hpp"
#include "export.hpp"
#include "events.hpp"
#include "gameobjects.hpp"
#include "object_ref.hpp"
#include "ui_write.hpp"
#include "userinput.hpp"
#include <handle.hpp>
#include <phys/physquery.hpp>

#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// Physics queries. Reached through GameContext::physics(); the facade is
// stateless, so holding one is the same as holding nothing.
//
// Every query throws BuildFeatureDisabledError when the engine was built
// without PELICAN_WITH_PHYSICS. A build without physics answers no query,
// rather than answering "nothing was hit". The object thrown is a
// BuildFeatureCallDisabledError (build_features.hpp): code() is
// build_feature_disabled@1 and feature() is "PELICAN_WITH_PHYSICS".
//
// A build with physics but no provider that answers the query -- the builtin
// and Jolt providers both left out and no game provider registered for it --
// answers no query either: it throws phys::ProviderUnavailableError,
// physics.provider_unavailable@1 (phys/physquery.hpp).
//
// Except in a destruction callback (Behavior::onDestroy, which is noexcept):
// there neither refusal is thrown. It is recorded -- during a scene load as a
// build_feature_disabled@1 or physics.provider_unavailable@1 effect error,
// otherwise in the engine log -- and the query answers empty (no hits,
// nullopt). That empty answer is the same one "nothing was hit" gives; only
// the record tells them apart.
class PELICAN_API PhysicsContext {
  public:
    std::vector<phys::RaycastQueryHit> raycastAll(
        const phys::Ray &ray, const phys::QueryFilter &filter = {}) const;
    // The nearest hit. Hits within phys::internal::queryTieEpsilon of each
    // other are a tie, and a tie is broken by the smallest collider id, so the
    // answer does not depend on collider or provider iteration order.
    std::optional<phys::RaycastQueryHit> raycastClosest(
        const phys::Ray &ray, const phys::QueryFilter &filter = {}) const;
    // Sorted by collider id.
    std::vector<phys::OverlapHit> overlapAll(
        const phys::Shape &shape, const phys::QueryFilter &filter = {}) const;
    std::vector<phys::ShapeCastQueryHit> shapeCastAll(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
    std::optional<phys::ShapeCastQueryHit> shapeCastClosest(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
};

// Sound playback. Reached through GameContext::audio().
//
// Every entry throws BuildFeatureDisabledError when the engine was built
// without PELICAN_WITH_AUDIO, as a BuildFeatureCallDisabledError whose code()
// is build_feature_disabled@1 and feature() is "PELICAN_WITH_AUDIO". During
// scene effect staging, playSound / stopSound / setBusVolume / busVolume /
// isPlaying throw the same; setSoundVolume / setSoundPitch / setSoundPaused
// return false there before the build is asked.
//
// Except in a destruction callback (Behavior::onDestroy, which is noexcept):
// there no entry throws this refusal. It is recorded -- during a scene load as
// a build_feature_disabled@1 effect error, otherwise in the engine log -- and
// the entry answers empty: an invalid handle, false, 0, or nullopt from
// soundState. The empty answer cannot be told from a real one (soundState's
// nullopt is also "no such voice"); only the record tells them apart.
class PELICAN_API AudioContext {
  public:
    SoundHandle playSound(std::string_view path) const;
    // The same voice on a named bus: "se" (what the one-argument form plays
    // on) or "bgm". Another name throws.
    SoundHandle playSound(std::string_view path, std::string_view bus) const;
    void stopSound(SoundHandle handle) const;
    void setBusVolume(std::string_view bus, float volume) const;
    float busVolume(std::string_view bus) const;
    bool isPlaying(SoundHandle handle) const;
    // Update-thread controls for existing voices. Mutations return false during
    // scene effect staging or for absent handles; invalid values throw.
    bool setSoundVolume(SoundHandle handle, float volume) const;
    bool setSoundPitch(SoundHandle handle, float pitch) const;
    bool setSoundPaused(SoundHandle handle, bool paused) const;
    std::optional<SoundPlaybackState> soundState(SoundHandle handle) const;
};

// The animation graph attached to an object. Reached through
// GameContext::animation().
//
// Before the graph's first frame attachment, after its removal, and in a build
// without PELICAN_WITH_ANIMATION, these return not_found. Scene
// activation/destruction transactions return phase_order_error.
//
// The rule for a module this build left out: an API that answers with a
// Status answers the missing module with a Status too, never an exception.
// Animation::Status is frozen with the ABI, so the missing module is the
// not_found above rather than a Status of its own; the facades that return
// values (audio, physics) throw BuildFeatureCallDisabledError instead.
class PELICAN_API AnimationContext {
  public:
    Animation::Status setParameter(GameObjectId object, std::string_view name,
                                   double value) const;
    Animation::Status forceState(GameObjectId object, std::string_view state) const;
};

// Save slots. Reached through GameContext::save(). Game settings are not save
// data and stay on GameContext.
class PELICAN_API SaveContext {
  public:
    using Json = nlohmann::json;
    struct SlotInfo {
        std::string slot;
        std::filesystem::file_time_type timestamp;
    };

    void saveData(std::string_view slot, const Json &data) const;
    std::optional<Json> loadData(std::string_view slot) const;
    std::vector<SlotInfo> listSaves() const;
};

// What the frame draws: lights, the active camera, values the render graph
// reads, and debug overlays. Reached through GameContext::render().
class PELICAN_API RenderContext {
  public:
    [[nodiscard]] bool setDirectionalLightDirection(std::string_view name, vec3 direction) const;
    [[nodiscard]] bool setDirectionalLightIntensity(std::string_view name, float intensity) const;
    [[nodiscard]] bool setPointLightPosition(std::string_view name, vec3 position) const;
    [[nodiscard]] bool setSpotLightDirection(std::string_view name, vec3 direction) const;

    void setCamera(std::string_view name) const;

    // Publishes float32 values for a declared runtime_f32 render buffer. The
    // entire buffer must be supplied. Copied now, uploaded before rendering.
    // Returns false for missing buffers or during scene effect staging.
    bool setRenderValues(std::string_view buffer, std::span<const float> values) const;

    // The render graph's value graph, not the animation graph: these reach the
    // parameters and events a frame graph program declares.
    // Full instance/parameter path. nullopt removes the runtime override.
    bool setValueGraphParameter(GameObjectId object, std::string_view parameter,
                                std::optional<double> value) const;
    bool emitValueGraphEvent(GameObjectId object, std::string_view input,
                             Animation::CurvePlaybackCommand command) const;

    void debugText(int x, int y, std::string_view text,
                   vec4 color = {1, 1, 1, 1}, int scale = 1) const;

    // Call from the game/update thread. Values must match the task's
    // callable_record schema; invalid updates leave the previous values intact.
    // Scene activation/destruction transactions cannot modify live RT tasks.
    void setRayTracingCallableRecord(std::string_view task_name, std::uint32_t index,
                                    const nlohmann::json &values) const;

    // Updates an explicit dispatch.rays task on the game/update thread.
    // Image-derived tasks and scene transactions cannot be overridden.
    void setRayTracingDispatchExtent(std::string_view task_name, std::uint32_t width,
                                    std::uint32_t height, std::uint32_t depth) const;
};

// Scene objects found by name. Reached through GameContext::objects().
//
// resolve() looks a name up once, in the scene that is live (inside a scene
// load's callbacks, the scene being loaded), and returns a ref bound to that
// scene run. When several objects share a name, the first one the scene
// declares is the one resolved. A name the scene does not declare is
// object.not_materialized@1.
//
// materialize() and worldTransform() refuse a ref from an earlier scene run
// -- the scene was loaded or reloaded since it was resolved -- with
// object.stale_authority@1, and a declared object that has no live runtime
// object (removed, or light-only) with object.not_materialized@1.
// worldTransform() of a live object without a transform is
// object.missing_capability@1.
//
// worldTransform() reads the world transform as it stands, which the
// transform update recomputes once per frame. setLocalTransform() writes the
// object's own world transform at once but as if it had no parent; its
// parent's placement, and the new placement of its children, arrive at the
// next transform update. Until then a read of a parented object, or of a
// child of the moved object, lags by a frame.
class PELICAN_API ObjectsContext {
  public:
    Resolved<ObjectRef> resolve(std::string_view name) const;
    Resolved<GameObjectId> materialize(const ObjectRef &ref) const;
    Resolved<WorldTransform> worldTransform(const ObjectRef &ref) const;
};

// The project's UI, written from game code. Reached through
// GameContext::ui(), which addresses the project's UI document, or
// GameContext::ui(document_key), which must name that document's `key`.
//
// A node is named by its path: the node ids from the root, root included,
// joined by '/' (the root "root" with a child "hud" holding "score" is
// "root/hud/score"). Ids are unique only among siblings, so the path, not
// the id, is what tells two nodes apart.
//
// A write is checked when it is made and queued; accepted writes land
// together at the frame boundary after game code runs, before the frame is
// drawn. setText suits a label or a button, setValue a gauge (its bar fills
// the share 0..1 of its width), setVisible any node. A refused write changes
// nothing and says why (UiWriteError). A node property a ui.* graph sink
// writes is the graph's: writing it here is ui.sink_conflict@1.
//
// The UI document is not part of a scene: a write made while a scene loads
// stays even if that load fails.
//
// The facade holds the document key as a view; keep the string alive while
// the facade is used.
class PELICAN_API UiContext {
  public:
    // An empty key is the project's UI document.
    explicit UiContext(std::string_view document_key) noexcept;

    [[nodiscard]] UiWriteResult setText(std::string_view path, std::string_view text) const;
    [[nodiscard]] UiWriteResult setVisible(std::string_view path, bool visible) const;
    [[nodiscard]] UiWriteResult setValue(std::string_view path, float value) const;

  private:
    std::string_view document_key_;
};

class PELICAN_API GameContext {
  public:
    bool actionsConfigured() const;
    bool actionPressed(std::string_view action_name) const;
    bool actionReleased(std::string_view action_name) const;
    bool actionHeld(std::string_view action_name) const;
    float actionAxis1(std::string_view action_name) const;
    ActionAxis2 actionAxis2(std::string_view action_name) const;
    ActionPose actionPose(std::string_view action_name) const;

    double time() const;
    double deltaTime() const;
    std::uint64_t frameIndex() const;

    void logInfo(std::string_view message) const;
    void logWarning(std::string_view message) const;
    void logError(std::string_view message) const;

    GameObjectId createObject(const LocalTransformComponent &transform) const;
    GameObjectId createSpriteObject(const LocalTransformComponent &transform,
                                    const SpriteViewComponent &sprite) const;
    [[nodiscard]] bool removeObject(GameObjectId id) const;
    LocalTransformComponent localTransform(GameObjectId id) const;
    [[nodiscard]] bool setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) const;
    std::optional<SpriteViewComponent> spriteView(GameObjectId id) const;
    [[nodiscard]] bool setSpriteView(GameObjectId id, const SpriteViewComponent &sprite) const;
    [[nodiscard]] bool setSpriteTexture(GameObjectId id, std::string_view texture) const;

    double random();
    int randomInt(int min, int max);
    float randomFloat(float min, float max);
    void setSeed(std::uint64_t seed);
    std::uint64_t seed() const;

    void loadScene(std::string_view name) const;
    std::string currentScene() const;

    template <class Event> void emit(const Event &event) const {
        internal::getEventRegisterer().emit(event);
    }

    using Json = nlohmann::json;

    Json gameSettings() const;
    void setGameSettings(Json settings) const;
    void saveSettings() const;

    // The domain facades. Each is stateless and costs nothing to return, so
    // ctx.audio().playSound(...) is a single call, not a lookup.
    PhysicsContext physics() const noexcept { return {}; }
    AudioContext audio() const noexcept { return {}; }
    AnimationContext animation() const noexcept { return {}; }
    SaveContext save() const noexcept { return {}; }
    RenderContext render() const noexcept { return {}; }
    ObjectsContext objects() const noexcept { return {}; }
    UiContext ui() const noexcept { return UiContext{std::string_view{}}; }
    UiContext ui(std::string_view document_key) const noexcept { return UiContext{document_key}; }
};

} // namespace Pelican
