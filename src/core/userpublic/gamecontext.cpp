#include "gamecontext.hpp"

#include "../appflow/enginetime.hpp"
#include "../build_features.hpp"
#include "../ecs/core.hpp"
#include "components/predefined.hpp"
#include "deterministicrng.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "../ecs/predefined/transform.hpp"
#include "../loader/scene.hpp"
#include "../log.hpp"
#include "../persistence/persistence.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
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

GameObjectId GameContext::createSpriteObject(const LocalTransformComponent &transform,
                                             const SpriteViewComponent &sprite) const {
    sprite.validate();
    const auto id = GameObjects::add()
                        .addComponent<TransformComponent>()
                        .addComponent<LocalTransformComponent>(transform)
                        .addComponent<SpriteViewComponent>(sprite)
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

std::optional<SpriteViewComponent> GameContext::spriteView(GameObjectId id) const {
    const auto *sprite = GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<SpriteViewComponent>(id);
    if (sprite == nullptr) return std::nullopt;
    return *sprite;
}

bool GameContext::setSpriteView(GameObjectId id, const SpriteViewComponent &sprite) const {
    sprite.validate();
    return GET_MODULE(ECSCore).getTemplatePublicModule().setComponent<SpriteViewComponent>(id, sprite);
}

bool GameContext::setSpriteTexture(GameObjectId id, std::string_view texture) const {
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    const auto *current = ecs.tryComponent<SpriteViewComponent>(id);
    if (current == nullptr) return false;
    auto replacement = *current;
    replacement.texture = std::string{texture};
    replacement.validate();
    return ecs.setComponent<SpriteViewComponent>(id, replacement);
}

std::vector<phys::RaycastQueryHit> GameContext::raycastAll(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).raycastAll(ray, filter);
#else
    (void)ray;
    (void)filter;
    return {};
#endif
}

std::optional<phys::ObjectRaycastHit> GameContext::raycastClosest(
    const phys::Ray &ray) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).raycastClosest(ray);
#else
    (void)ray;
    return std::nullopt;
#endif
}

std::optional<phys::RaycastQueryHit> GameContext::raycastClosest(
    const phys::Ray &ray, const phys::QueryFilter &filter) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).raycastClosest(ray, filter);
#else
    (void)ray;
    (void)filter;
    return std::nullopt;
#endif
}

std::vector<phys::OverlapHit> GameContext::overlapAllHits(
    const phys::Shape &shape, const phys::QueryFilter &filter) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).overlapAllHits(shape, filter);
#else
    (void)shape;
    (void)filter;
    return {};
#endif
}

std::vector<phys::ShapeCastQueryHit> GameContext::shapeCastAll(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).shapeCastAll(moving_shape, delta, filter);
#else
    (void)moving_shape;
    (void)delta;
    (void)filter;
    return {};
#endif
}

std::optional<phys::ShapeCastQueryHit> GameContext::shapeCastClosest(
    const phys::Shape &moving_shape, vec3 delta,
    const phys::QueryFilter &filter) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).shapeCastClosest(moving_shape, delta, filter);
#else
    (void)moving_shape;
    (void)delta;
    (void)filter;
    return std::nullopt;
#endif
}

std::vector<std::string> GameContext::overlapAll(
    const phys::Shape &shape) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).overlapAll(shape);
#else
    (void)shape;
    return {};
#endif
}

std::vector<std::string> GameContext::overlapAll(
    const phys::Shape &shape, const phys::QueryFilter &filter) const {
#if PELICAN_WITH_PHYSICS
    return GET_MODULE(PhysWorld).overlapAll(shape, filter);
#else
    (void)shape;
    (void)filter;
    return {};
#endif
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
