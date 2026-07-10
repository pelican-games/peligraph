#include "gamecontext.hpp"

#include "../appflow/enginetime.hpp"
#include "../build_features.hpp"
#include "components/predefined.hpp"
#include "deterministicrng.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "../ecs/predefined/transform.hpp"
#include "../loader/scene.hpp"
#include "../log.hpp"
#include "../persistence/persistence.hpp"
#include "../phys/physworld.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/debugtext.hpp"

#include <utility>

namespace Pelican {

bool GameContext::actionsConfigured() const {
    return Actions::isConfigured();
}

bool GameContext::actionPressed(std::string_view action_name) const {
    return Actions::isPressed(action_name);
}

bool GameContext::actionReleased(std::string_view action_name) const {
    return Actions::isReleased(action_name);
}

bool GameContext::actionHeld(std::string_view action_name) const {
    return Actions::isHeld(action_name);
}

float GameContext::actionAxis1(std::string_view action_name) const {
    return Actions::axis1(action_name);
}

ActionAxis2 GameContext::actionAxis2(std::string_view action_name) const {
    return Actions::axis2(action_name);
}

ActionPose GameContext::actionPose(std::string_view action_name) const {
    return Actions::pose(action_name);
}

double GameContext::time() const {
    return GET_MODULE(EngineTime).now();
}

double GameContext::deltaTime() const {
    return GET_MODULE(EngineTime).dt();
}

std::uint64_t GameContext::frameIndex() const {
    return GET_MODULE(EngineTime).frameIndex();
}

void GameContext::logInfo(std::string_view message) const {
    LOG_INFO(logger, "{}", std::string{message});
}

void GameContext::logWarning(std::string_view message) const {
    LOG_WARNING(logger, "{}", std::string{message});
}

void GameContext::logError(std::string_view message) const {
    LOG_ERROR(logger, "{}", std::string{message});
}

GameObjectId GameContext::createObject(const LocalTransformComponent &transform) const {
    const auto id = GameObjects::add()
                        .addComponent<TransformComponent>()
                        .addComponent<LocalTransformComponent>(transform)
                        .finish();
    (void)GameObjects::setLocalTransform(id, transform);
    return id;
}

bool GameContext::removeObject(GameObjectId id) const {
    return GameObjects::remove(id);
}

LocalTransformComponent GameContext::localTransform(GameObjectId id) const {
    return GameObjects::localTransform(id);
}

bool GameContext::setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) const {
    return GameObjects::setLocalTransform(id, transform);
}

std::optional<phys::ObjectRaycastHit> GameContext::raycastClosest(const phys::Ray &ray) const {
    return GET_MODULE(PhysWorld).raycastClosest(ray);
}

std::vector<std::string> GameContext::overlapAll(const phys::Shape &shape) const {
    return GET_MODULE(PhysWorld).overlapAll(shape);
}

void GameContext::setCamera(std::string_view name) const {
    GET_MODULE(Camera).setActiveCamera(name);
}

double GameContext::random() {
    return GET_MODULE(DeterministicRng).random();
}

int GameContext::randomInt(int min, int max) {
    return GET_MODULE(DeterministicRng).randomInt(min, max);
}

float GameContext::randomFloat(float min, float max) {
    return GET_MODULE(DeterministicRng).randomFloat(min, max);
}

void GameContext::setSeed(std::uint64_t seed) {
    GET_MODULE(DeterministicRng).setSeed(seed);
}

std::uint64_t GameContext::seed() const {
    return GET_MODULE(DeterministicRng).seed();
}

SoundHandle GameContext::playSound(std::string_view path) const {
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).playSound(path);
#else
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "playSound is unavailable");
#endif
}

void GameContext::stopSound(SoundHandle handle) const {
#if PELICAN_WITH_AUDIO
    GET_MODULE(Audio).stopSound(handle);
#else
    (void)handle;
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "stopSound is unavailable");
#endif
}

void GameContext::setBusVolume(std::string_view bus, float volume) const {
#if PELICAN_WITH_AUDIO
    GET_MODULE(Audio).setBusVolume(bus, volume);
#else
    (void)bus;
    (void)volume;
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "setBusVolume is unavailable");
#endif
}

bool GameContext::isPlaying(SoundHandle handle) const {
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).isPlaying(handle);
#else
    (void)handle;
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "isPlaying is unavailable");
#endif
}

void GameContext::loadScene(std::string_view name) const {
    GET_MODULE(SceneLoader).requestLoad(std::string{name});
}

std::string GameContext::currentScene() const {
    return GET_MODULE(SceneLoader).currentScene();
}

void GameContext::debugText(int x, int y, std::string_view text) const {
    GET_MODULE(DebugText).text(x, y, text);
}

GameContext::Json GameContext::gameSettings() const {
    return GET_MODULE(Persistence).gameSettings();
}

void GameContext::setGameSettings(Json settings) const {
    GET_MODULE(Persistence).setGameSettings(std::move(settings));
}

void GameContext::saveSettings() const {
    auto &persistence = GET_MODULE(Persistence);
#if PELICAN_WITH_AUDIO
    persistence.captureAudioSettings(GET_MODULE(Audio));
#endif
    persistence.saveSettings();
}

void GameContext::saveData(std::string_view slot, const Json &data) const {
    GET_MODULE(Persistence).saveData(slot, data);
}

std::optional<GameContext::Json> GameContext::loadData(std::string_view slot) const {
    return GET_MODULE(Persistence).loadData(slot);
}

std::vector<GameContext::SlotInfo> GameContext::listSaves() const {
    const auto saves = GET_MODULE(Persistence).listSaves();
    std::vector<SlotInfo> result;
    result.reserve(saves.size());
    for (const auto &save : saves) {
        result.push_back(SlotInfo{.slot = save.slot, .timestamp = save.timestamp});
    }
    return result;
}

} // namespace Pelican
