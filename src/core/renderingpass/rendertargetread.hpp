#pragma once

#include <cstdint>
#include <optional>

namespace Pelican {

// Which of a history render target's two surfaces a read wants. A target
// without history has one surface and ignores this.
struct RenderTargetRead {
    // true: the surface the previous frame wrote.
    bool previous = false;
    // The frame whose parity picks the surface. Empty: the container's own
    // history frame (advanceHistoryFrame()).
    std::optional<std::uint32_t> frame;

    static constexpr RenderTargetRead current() noexcept { return {}; }
    static constexpr RenderTargetRead previousFrame() noexcept {
        return {.previous = true};
    }
    static constexpr RenderTargetRead forFrame(
        bool previous, std::uint32_t frame_index) noexcept {
        return {.previous = previous, .frame = frame_index};
    }
};

} // namespace Pelican
