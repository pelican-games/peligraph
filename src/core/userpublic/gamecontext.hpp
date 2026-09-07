#pragma once

#include "components/localtransform.hpp"
#include "components/spriteview.hpp"
#include "animation/abi_v1.hpp"
#include "animation/curve.hpp"
#include "audio.hpp"
#include "export.hpp"
#include "events.hpp"
#include "gameobjects.hpp"
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

    [[nodiscard]] bool setDirectionalLightDirection(std::string_view name, vec3 direction) const;
    [[nodiscard]] bool setDirectionalLightIntensity(std::string_view name, float intensity) const;
    [[nodiscard]] bool setPointLightPosition(std::string_view name, vec3 position) const;
    [[nodiscard]] bool setSpotLightDirection(std::string_view name, vec3 direction) const;

    std::vector<phys::RaycastQueryHit> raycastAll(
        const phys::Ray &ray, const phys::QueryFilter &filter = {}) const;
    std::optional<phys::ObjectRaycastHit> raycastClosest(const phys::Ray &ray) const;
    std::optional<phys::RaycastQueryHit> raycastClosest(
        const phys::Ray &ray, const phys::QueryFilter &filter) const;
    std::vector<phys::OverlapHit> overlapAllHits(
        const phys::Shape &shape, const phys::QueryFilter &filter = {}) const;
    std::vector<phys::ShapeCastQueryHit> shapeCastAll(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
    std::optional<phys::ShapeCastQueryHit> shapeCastClosest(
        const phys::Shape &moving_shape, vec3 delta,
        const phys::QueryFilter &filter = {}) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape) const;
    std::vector<std::string> overlapAll(
        const phys::Shape &shape, const phys::QueryFilter &filter) const;
    void setCamera(std::string_view name) const;

    // Call from the game/update thread. Values must match the task's
    // callable_record schema; invalid updates leave the previous values intact.
    // Scene activation/destruction transactions cannot modify live RT tasks.
    void setRayTracingCallableRecord(std::string_view task_name, std::uint32_t index,
                                    const nlohmann::json &values) const;

    // Updates an explicit dispatch.rays task on the game/update thread.
    // Image-derived tasks and scene transactions cannot be overridden.
    void setRayTracingDispatchExtent(std::string_view task_name, std::uint32_t width,
                                    std::uint32_t height, std::uint32_t depth) const;

    double random();
    int randomInt(int min, int max);
    float randomFloat(float min, float max);
    void setSeed(std::uint64_t seed);
    std::uint64_t seed() const;

    SoundHandle playSound(std::string_view path) const;
    void stopSound(SoundHandle handle) const;
    void setBusVolume(std::string_view bus, float volume) const;
    float busVolume(std::string_view bus) const;
    bool isPlaying(SoundHandle handle) const;
    // Update-thread controls for existing voices. Mutations return false during
    // scene effect staging or for absent handles; invalid values throw.
    bool setSoundVolume(SoundHandle handle, float volume) const;
    bool setSoundPitch(SoundHandle handle, float pitch) const;
    std::optional<SoundPlaybackState> soundState(SoundHandle handle) const;
    // Publishes float32 values for a declared runtime_f32 render buffer. The
    // entire buffer must be supplied. Copied now, uploaded before rendering.
    // Returns false for missing buffers or during scene effect staging.
    bool setRenderValues(std::string_view buffer, std::span<const float> values) const;
    // Full instance/parameter path. nullopt removes the runtime override.
    bool setGraphParameter(GameObjectId object, std::string_view parameter, std::optional<double> value) const;
    bool emitGraphEvent(GameObjectId object, std::string_view input, Animation::CurvePlaybackCommand command) const;

    // Update-thread controls for an attached animation graph. Before its first
    // frame attachment, or after removal, these return not_found. Scene
    // activation/destruction transactions return phase_order_error.
    Animation::Status setAnimationGraphParameter(GameObjectId object, std::string_view name, double value) const;
    Animation::Status forceAnimationGraphState(GameObjectId object, std::string_view state) const;

    void loadScene(std::string_view name) const;
    std::string currentScene() const;

    void debugText(int x, int y, std::string_view text,
                   vec4 color = {1, 1, 1, 1}, int scale = 1) const;

    template <class Event> void emit(const Event &event) const {
        internal::getEventRegisterer().emit(event);
    }

    using Json = nlohmann::json;
    struct SlotInfo {
        std::string slot;
        std::filesystem::file_time_type timestamp;
    };

    Json gameSettings() const;
    void setGameSettings(Json settings) const;
    void saveSettings() const;

    void saveData(std::string_view slot, const Json &data) const;
    std::optional<Json> loadData(std::string_view slot) const;
    std::vector<SlotInfo> listSaves() const;
};

} // namespace Pelican
