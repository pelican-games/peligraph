#pragma once

#include "../renderingpass/renderingpass.hpp"
#include "../vkcore/frametarget.hpp"
#include <cstdint>
#include <optional>
#include <string_view>
#include <vulkan/vulkan.hpp>

namespace Pelican::OpenXr {

inline constexpr std::string_view xr_mirror_intermediate_name =
    "__xr_mirror_left";

// Returns an empty rectangle for minimized/zero-sized destinations.  The
// source aspect ratio is preserved and the unused destination area is black.
std::optional<vk::Rect2D> mirrorLetterboxRect(vk::Extent2D source,
                                              vk::Extent2D destination);

enum class XrMirrorBeginAction {
    present,
    drop,
    disable,
};

XrMirrorBeginAction classifyMirrorBeginResult(
    const FrameBeginResult &result) noexcept;

struct XrMirrorSinkStats {
    std::uint64_t presented = 0;
    std::uint64_t dropped = 0;
    std::uint64_t failures = 0;
    FrameUnavailableReason last_drop_reason = FrameUnavailableReason::none;
};

// The desktop mirror is an optional, best-effort consumer of an engine-owned
// intermediate.  It never receives an OpenXR swapchain image and never waits
// for the desktop swapchain.
class XrMirrorSink {
    std::optional<CompiledPass> output_transform;
    GlobalRenderTargetId source_id = noRenderTargetId();
    bool screen_ui = false;
    bool disabled = false;
    bool first_outcome_reported = false;
    bool first_present_reported = false;
    bool thousand_frames_reported = false;
    XrMirrorSinkStats stats;

    void initialize();
    void rebindSourceIfNeeded();
    void reportProgress();

  public:
    XrMirrorSink() noexcept;

    void tryPresent() noexcept;
    const XrMirrorSinkStats &statistics() const noexcept { return stats; }
    bool available() const noexcept { return output_transform.has_value() && !disabled; }
};

} // namespace Pelican::OpenXr
