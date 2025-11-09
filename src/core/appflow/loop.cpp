#include "loop.hpp"

#include "../log.hpp"
#include "../os/window.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/renderer.hpp"
#include "framerate.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {

Loop::Loop(DependencyContainer &_con) : con{_con} {}

void Loop::run() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    auto &window = con.get<Window>();
    auto &renderer = con.get<Renderer>();
    auto &framerate_adjuster = con.get<FramerateAdjust>();

    LOG_INFO(logger, "starting main loop");

    while (true) {
        if (!window.process())
            break;
        renderer.render();
        framerate_adjuster.wait();
    }
}

} // namespace Pelican
