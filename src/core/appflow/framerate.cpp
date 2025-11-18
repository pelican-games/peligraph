#include "framerate.hpp"
#include "../loader/basicconfig.hpp"
#include "../log.hpp"

using namespace std::chrono_literals;

namespace Pelican {

constexpr auto max_frame_counter = 256;

FramerateAdjust::FramerateAdjust() {
    setFramerate(GET_MODULE(ProjectBasicConfig).framerateTarget());
}

void FramerateAdjust::reset() {
    frame_index = 0;
    base = std::chrono::system_clock::now();
}

void FramerateAdjust::setFramerate(float frame_per_secound) {
    current_fps_target = frame_per_secound;
    LOG_INFO(logger, "frame rate is set to {} frame/sec", frame_per_secound);
    reset();
}

void FramerateAdjust::wait() {
    const auto now_time = std::chrono::system_clock::now();
    const auto next_target_time =
        base + std::chrono::microseconds(int((frame_index + 1) * 1'000'000 / current_fps_target));

    if (next_target_time - now_time < 1ms) {
        std::this_thread::sleep_for(1ms);
        base = std::chrono::system_clock::now();
        frame_index = 0;
    } else {
        std::this_thread::sleep_until(next_target_time);
        frame_index++;

        if (frame_index > max_frame_counter) {
            base = next_target_time;
            frame_index = 0;
        }
    }
}

} // namespace Pelican
