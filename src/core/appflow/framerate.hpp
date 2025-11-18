#include "../container.hpp"
#include <chrono>
#include <thread>

namespace Pelican {

DECLARE_MODULE(FramerateAdjust) {
    float current_fps_target;
    std::chrono::system_clock::time_point base;
    int frame_index;

  public:
    FramerateAdjust();
    void reset();
    void setFramerate(float frame_per_second);
    void wait();
};

} // namespace Pelican
