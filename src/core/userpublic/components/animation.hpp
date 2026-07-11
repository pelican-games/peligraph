#pragma once

#include <cstdint>
#include <string>

namespace Pelican {

struct AnimationComponent {
    std::string clip;
    double speed = 1.0;
    std::uint8_t loop = 1;
    double start_time = 0.0;

    template <class T> void ref(T &ar) {
        ar.prop("clip", clip);
        ar.prop("speed", speed);
        ar.prop("loop", loop);
        ar.prop("start_time", start_time);
    }
};

} // namespace Pelican
