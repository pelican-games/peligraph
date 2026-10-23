#pragma once

#include "framebuffersnapshot.hpp"

#include <chrono>
#include <optional>

namespace Pelican {

// Owner-thread policy: observing a change never waits. A revision also catches
// A -> B -> A callbacks between frames. Bootstrap is already a complete epoch.
class FramebufferExtentStability {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto QuietPeriod = std::chrono::milliseconds{100};

    bool settled(FramebufferExtentSnapshot framebuffer, Clock::time_point now) noexcept {
        if (!observed_) {
            observed_ = framebuffer;
            return true;
        }
        if (*observed_ != framebuffer) {
            observed_ = framebuffer;
            changed_at_ = now;
        }
        return !changed_at_ || now - *changed_at_ >= QuietPeriod;
    }

  private:
    std::optional<FramebufferExtentSnapshot> observed_;
    std::optional<Clock::time_point> changed_at_;
};

} // namespace Pelican
