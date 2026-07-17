#pragma once

#include "../renderingpass/renderingpass.hpp"
#include <cstdint>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace Pelican::OpenXr {

// Returns an empty rectangle for minimized/zero-sized destinations.  The
// source aspect ratio is preserved and the unused destination area is black.
std::optional<vk::Rect2D> mirrorLetterboxRect(vk::Extent2D source,
                                              vk::Extent2D destination);

struct XrMirrorSinkStats {
    std::uint64_t presented = 0;
    std::uint64_t dropped = 0;
    std::uint64_t failures = 0;
};

// The desktop mirror is an optional, best-effort consumer of an engine-owned
// intermediate.  It never receives an OpenXR swapchain image and never waits
// for the desktop swapchain.
class XrMirrorSink {
    std::optional<CompiledPass> output_transform;
    GlobalRenderTargetId source_id = noRenderTargetId();
    bool screen_ui = false;
    bool disabled = false;
    XrMirrorSinkStats stats;

    void initialize();
    void rebindSourceIfNeeded();

  public:
    XrMirrorSink() noexcept;

    void tryPresent() noexcept;
    const XrMirrorSinkStats &statistics() const noexcept { return stats; }
    bool available() const noexcept { return output_transform.has_value() && !disabled; }
};

} // namespace Pelican::OpenXr
