#pragma once

#include "details/event/registerer.hpp"
#include "details/ecs/entity.hpp"

#include <string>

namespace Pelican {

struct SceneLoaded {
    std::string scene_name;

    template <class T> void ref(T &ar) {
        ar.prop("scene_name", scene_name);
    }
};

struct OverlapEnter {
    EntityId self = invalidEntityId;
    EntityId other = invalidEntityId;
};

struct OverlapExit {
    EntityId self = invalidEntityId;
    EntityId other = invalidEntityId;
};

} // namespace Pelican

PELICAN_REGISTER_EVENT(Pelican::SceneLoaded);
PELICAN_REGISTER_EVENT(Pelican::OverlapEnter);
PELICAN_REGISTER_EVENT(Pelican::OverlapExit);

namespace Pelican {
PELICAN_REGISTER_EVENT(SceneLoaded);
PELICAN_REGISTER_EVENT(OverlapEnter);
PELICAN_REGISTER_EVENT(OverlapExit);
} // namespace Pelican
