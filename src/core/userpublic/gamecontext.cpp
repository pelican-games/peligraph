#include "gamecontext.hpp"

#include "../appflow/enginetime.hpp"
#include "components/predefined.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../log.hpp"
#include "../phys/physworld.hpp"
#include "../renderer/camera.hpp"

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
    GameObjects::setLocalTransform(id, transform);
    return id;
}

void GameContext::removeObject(GameObjectId id) const {
    GameObjects::remove(id);
}

LocalTransformComponent GameContext::localTransform(GameObjectId id) const {
    return GameObjects::localTransform(id);
}

void GameContext::setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) const {
    GameObjects::setLocalTransform(id, transform);
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

} // namespace Pelican
