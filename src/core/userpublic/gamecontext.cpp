#include "gamecontext.hpp"

#include "../appflow/enginetime.hpp"
#include "../build_features.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/animationsystem.hpp"
#include "components/predefined.hpp"
#include "deterministicrng.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include "../ecs/predefined/transform.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../loader/scene.hpp"
#include "../loader/effecttransaction.hpp"
#include "../light/lightcontainer.hpp"
#include "../log.hpp"
#include "../persistence/persistence.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
#include "../renderer/camera.hpp"
#include "../renderer/debugtext.hpp"
#include "../renderingpass/computetask.hpp"

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
    if (internal::behaviorCallbackActive()) {
        GET_MODULE(BehaviorAttachmentArena).deferCreateObject(transform);
        return invalidGameObjectId;
    }
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
    if (internal::behaviorCallbackActive()) {
        GET_MODULE(BehaviorAttachmentArena).deferCreateSpriteObject(transform,
                                                                    sprite);
        return invalidGameObjectId;
    }
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
    if (auto *effect = internal::activeEffectTransaction()) {
        if (const auto *value = effect->retiredComponent(
                id, ComponentIdByType<LocalTransformComponent>::value)) {
            return *static_cast<const LocalTransformComponent *>(value);
        }
    }
    return GameObjects::localTransform(id);
}

bool GameContext::setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) const {
    if (auto *effect = internal::activeEffectTransaction();
        effect != nullptr && effect->rejectRetiredMutation(
                                 "setLocalTransform", id)) return false;
    return GameObjects::setLocalTransform(id, transform);
}

std::optional<SpriteViewComponent> GameContext::spriteView(GameObjectId id) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        if (const auto *value = effect->retiredComponent(
                id, ComponentIdByType<SpriteViewComponent>::value)) {
            return *static_cast<const SpriteViewComponent *>(value);
        }
    }
    const auto *sprite = GET_MODULE(ECSCore).getTemplatePublicModule().tryComponent<SpriteViewComponent>(id);
    if (sprite == nullptr) return std::nullopt;
    return *sprite;
}

bool GameContext::setSpriteView(GameObjectId id, const SpriteViewComponent &sprite) const {
    if (auto *effect = internal::activeEffectTransaction();
        effect != nullptr && effect->rejectRetiredMutation(
                                 "setSpriteView", id)) return false;
    sprite.validate();
    return GET_MODULE(ECSCore).getTemplatePublicModule().setComponent<SpriteViewComponent>(id, sprite);
}

bool GameContext::setSpriteTexture(GameObjectId id, std::string_view texture) const {
    if (auto *effect = internal::activeEffectTransaction();
        effect != nullptr && effect->rejectRetiredMutation(
                                 "setSpriteTexture", id)) return false;
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    const auto *current = ecs.tryComponent<SpriteViewComponent>(id);
    if (current == nullptr) return false;
    auto replacement = *current;
    replacement.texture = std::string{texture};
    replacement.validate();
    return ecs.setComponent<SpriteViewComponent>(id, replacement);
}

bool GameContext::setDirectionalLightDirection(std::string_view name, vec3 direction) const {
    return GET_MODULE(LightContainer).setDirectionalLightDirection(
        std::string{name}, glm::vec3{direction.x, direction.y, direction.z});
}

void GameContext::setRayTracingDispatchExtent(
    std::string_view task_name, std::uint32_t width, std::uint32_t height,
    std::uint32_t depth) const {
    if (internal::activeEffectTransaction() != nullptr)
        throw SceneTransactionMutationRejected{"setRayTracingDispatchExtent"};
    auto &tasks = GET_MODULE(ComputeTaskContainer);
    const auto task = tasks.getComputeTaskIdByName(std::string{task_name});
    if (task.value < 0) {
        throw std::invalid_argument(
            "pelican.ray_tracing.task_not_found@1: task '" + std::string{task_name} + "'");
    }
    tasks.setRayTracingDispatchExtent(task, width, height, depth);
}

void GameContext::setRayTracingCallableRecord(
    std::string_view task_name, std::uint32_t index, const nlohmann::json &values) const {
    if (internal::activeEffectTransaction() != nullptr) {
        throw SceneTransactionMutationRejected{"setRayTracingCallableRecord"};
    }
    auto &tasks = GET_MODULE(ComputeTaskContainer);
    const auto task = tasks.getComputeTaskIdByName(std::string{task_name});
    if (task.value < 0) {
        throw std::invalid_argument(
            "pelican.ray_tracing.task_not_found@1: task '" + std::string{task_name} + "'");
    }
    tasks.setCallableRecord(task, index, values);
}

bool GameContext::setDirectionalLightIntensity(std::string_view name, float intensity) const {
    return GET_MODULE(LightContainer).setDirectionalLightIntensity(std::string{name}, intensity);
}

bool GameContext::setPointLightPosition(std::string_view name, vec3 position) const {
    return GET_MODULE(LightContainer).setPointLightPosition(
        std::string{name}, glm::vec3{position.x, position.y, position.z});
}

bool GameContext::setSpotLightDirection(std::string_view name, vec3 direction) const {
    return GET_MODULE(LightContainer).setSpotLightDirection(
        std::string{name}, glm::vec3{direction.x, direction.y, direction.z});
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
    if (auto *effect = internal::activeEffectTransaction()) {
        return effect->playSound(path);
    }
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).playSound(path);
#else
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "playSound is unavailable");
#endif
}

Animation::Status GameContext::setAnimationGraphParameter(GameObjectId object, std::string_view name, double value) const {
    if (internal::activeEffectTransaction()) return Animation::Status::phase_order_error;
    return GET_MODULE(AnimationSystem).setGraphParameter(object, name, value);
}

Animation::Status GameContext::forceAnimationGraphState(GameObjectId object, std::string_view state) const {
    if (internal::activeEffectTransaction()) return Animation::Status::phase_order_error;
    return GET_MODULE(AnimationSystem).forceGraphState(object, state);
}

void GameContext::stopSound(SoundHandle handle) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->stopSound(handle);
        return;
    }
#if PELICAN_WITH_AUDIO
    GET_MODULE(Audio).stopSound(handle);
#else
    (void)handle;
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "stopSound is unavailable");
#endif
}

void GameContext::setBusVolume(std::string_view bus, float volume) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->setBusVolume(bus, volume);
        return;
    }
#if PELICAN_WITH_AUDIO
    GET_MODULE(Audio).setBusVolume(bus, volume);
#else
    (void)bus;
    (void)volume;
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "setBusVolume is unavailable");
#endif
}

float GameContext::busVolume(std::string_view bus) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        return effect->busVolume(bus);
    }
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).busVolume(bus);
#else
    (void)bus;
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "busVolume is unavailable");
#endif
}

bool GameContext::isPlaying(SoundHandle handle) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        return effect->isPlaying(handle);
    }
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).isPlaying(handle);
#else
    (void)handle;
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "isPlaying is unavailable");
#endif
}

bool GameContext::setSoundVolume(SoundHandle handle, float volume) const {
    if (internal::activeEffectTransaction()) return false;
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).setSoundVolume(handle, volume);
#else
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "setSoundVolume is unavailable");
#endif
}

bool GameContext::setSoundPitch(SoundHandle handle, float pitch) const {
    if (internal::activeEffectTransaction()) return false;
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).setSoundPitch(handle, pitch);
#else
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "setSoundPitch is unavailable");
#endif
}

std::optional<SoundPlaybackState> GameContext::soundState(SoundHandle handle) const {
#if PELICAN_WITH_AUDIO
    return GET_MODULE(Audio).soundState(handle);
#else
    throwBuildFeatureDisabled("PELICAN_WITH_AUDIO", "soundState is unavailable");
#endif
}

void GameContext::loadScene(std::string_view name) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->loadScene(name);
        return;
    }
    GET_MODULE(SceneLoader).requestLoad(std::string{name});
}

std::string GameContext::currentScene() const {
    return GET_MODULE(SceneLoader).currentScene();
}

void GameContext::debugText(int x, int y, std::string_view text, vec4 color, int scale) const {
    GET_MODULE(DebugText).text(x, y, text, {color.x, color.y, color.z, color.w}, scale);
}

GameContext::Json GameContext::gameSettings() const {
    if (auto *effect = internal::activeEffectTransaction()) {
        return effect->gameSettings();
    }
    return GET_MODULE(Persistence).gameSettings();
}

void GameContext::setGameSettings(Json settings) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->setGameSettings(std::move(settings));
        return;
    }
    GET_MODULE(Persistence).setGameSettings(std::move(settings));
}

void GameContext::saveSettings() const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->saveSettings();
        return;
    }
    auto &persistence = GET_MODULE(Persistence);
#if PELICAN_WITH_AUDIO
    persistence.captureAudioSettings(GET_MODULE(Audio));
#endif
    persistence.saveSettings();
}

void GameContext::saveData(std::string_view slot, const Json &data) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        effect->saveData(slot, data);
        return;
    }
    GET_MODULE(Persistence).saveData(slot, data);
}

std::optional<GameContext::Json> GameContext::loadData(std::string_view slot) const {
    if (auto *effect = internal::activeEffectTransaction()) {
        return effect->loadData(slot);
    }
    return GET_MODULE(Persistence).loadData(slot);
}

std::vector<GameContext::SlotInfo> GameContext::listSaves() const {
    const auto saves = internal::activeEffectTransaction()
        ? internal::activeEffectTransaction()->listSaves()
        : GET_MODULE(Persistence).listSaves();
    std::vector<SlotInfo> result;
    result.reserve(saves.size());
    for (const auto &save : saves) {
        result.push_back(SlotInfo{.slot = save.slot, .timestamp = save.timestamp});
    }
    return result;
}

} // namespace Pelican
