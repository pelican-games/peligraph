#pragma once

#include "details/event/registerer.hpp"

#include <string>

namespace Pelican {

struct SceneLoaded {
    std::string scene_name;

    template <class T> void ref(T &ar) {
        ar.prop("scene_name", scene_name);
    }
};

} // namespace Pelican

PELICAN_REGISTER_EVENT(Pelican::SceneLoaded);

namespace Pelican {
PELICAN_REGISTER_EVENT(SceneLoaded);
} // namespace Pelican
