#include "loop.hpp"

#include "../ecs/core.hpp"
#include "../log.hpp"
#include "../os/window.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/renderer.hpp"
#include "framerate.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Pelican {

Loop::Loop() {}

void Loop::run() {
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    auto &window = GET_MODULE(Window);
    auto &renderer = GET_MODULE(Renderer);
    auto &ecs = GET_MODULE(ECSCore);
    auto &framerate_adjuster = GET_MODULE(FramerateAdjust);

    LOG_INFO(logger, "starting main loop");

    while (true) {
        if (!window.process())
            break;
        ecs.update();
        renderer.render();
        framerate_adjuster.wait();
    }
}

} // namespace Pelican
