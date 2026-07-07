#pragma once

#include "components/localtransform.hpp"
#include "gameobjects.hpp"
#include "userinput.hpp"
#include <phys/physquery.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

class GameContext {
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
    void removeObject(GameObjectId id) const;
    LocalTransformComponent localTransform(GameObjectId id) const;
    void setLocalTransform(GameObjectId id, const LocalTransformComponent &transform) const;

    std::optional<phys::ObjectRaycastHit> raycastClosest(const phys::Ray &ray) const;
    std::vector<std::string> overlapAll(const phys::Shape &shape) const;
};

} // namespace Pelican
