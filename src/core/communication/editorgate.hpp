#pragma once

#include <cstdint>
#include <optional>

namespace Pelican {

// The one rule every editor service applies to an edit accepted earlier: it
// may commit at the frame boundary only while the gate epoch it was accepted
// under is still current.
//
// The epoch advances whenever an observed gate state differs from the
// previous observation. A gate that closed and reopened between acceptance
// and commit therefore also counts as a change, and so does a transition
// that returned to an earlier value. `State` is the service's gate
// observation and must be equality comparable.
template <class State>
class EditorGateEpoch {
  public:
    // Records `state` and returns the epoch it belongs to.
    std::uint64_t observe(const State &state) {
        if (!last_) {
            last_ = state;
        } else if (!(*last_ == state)) {
            *last_ = state;
            ++epoch_;
        }
        return epoch_;
    }

    // For a transition the gate observation cannot see, such as the
    // journal's scene-id check at commit.
    std::uint64_t advance() noexcept { return ++epoch_; }

    std::uint64_t current() const noexcept { return epoch_; }

  private:
    std::optional<State> last_;
    std::uint64_t epoch_ = 1;
};

} // namespace Pelican
