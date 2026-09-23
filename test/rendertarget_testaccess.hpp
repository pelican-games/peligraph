#pragma once

// The one poke a test makes on RenderTarget: make the next frames report an
// output with no image, the state a window reaches before its swapchain
// exists. Production reaches it only through a real window, so without this a
// headless test could not exercise the renderer's "skip this frame" path.

#include "../src/core/vkcore/rendertarget.hpp"

#include <cstdint>

namespace Pelican::test_support {

struct RenderTargetTestAccess {
    static void reportUnavailableFrames(RenderTarget &target,
                                        std::uint32_t frames) {
        target.unavailable_frames_for_testing = frames;
    }
};

} // namespace Pelican::test_support
