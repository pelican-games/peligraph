#pragma once
#include "../os/actionmap.hpp"
#include "../userpublic/userdataerror.hpp"
#include <string>
namespace Pelican {
inline void requireControllerTarget(std::string_view camera, std::string_view target, bool exists) {
    if (!exists) throw UserDataError("camera '" + std::string{camera} + "' controller target '" +
                                    std::string{target} + "' has no transform");
}
inline void requireControllerActions(const InputActionMap *actions, bool orbit, bool fly) {
    if (!actions || (!orbit && !fly)) return;
    const auto require = [&](const char *name, InputActionType type) {
        const auto *action = actions->findAction(name);
        if (!action) throw UserDataError("unknown input action: " + std::string{name});
        if (action->type != type) throw UserDataError("camera controller input action has wrong type: " + std::string{name});
    };
    require("look", InputActionType::axis2);
    require("move", InputActionType::axis2);
    if (orbit) {
        require("pan", InputActionType::axis2);
        require("zoom", InputActionType::axis1);
    }
}
}
