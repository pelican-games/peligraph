#pragma once

#include "../container.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

enum class RenderDocCaptureState {
    unavailable,
    idle,
    armed,
    capturing,
    completing,
    failed,
};

enum class RenderDocCaptureSource {
    f11,
    rpc,
};

std::string_view renderDocCaptureStateName(RenderDocCaptureState state) noexcept;
std::string_view renderDocCaptureSourceName(RenderDocCaptureSource source) noexcept;

struct RenderDocApiTable {
    std::function<void()> disable_capture_keys;
    std::function<std::uint32_t()> get_num_captures;
    std::function<std::uint32_t(std::uint32_t, char *, std::uint32_t *, std::uint64_t *)>
        get_capture;
    std::function<void(void *, void *)> start_frame_capture;
    std::function<std::uint32_t(void *, void *)> end_frame_capture;
    std::function<std::uint32_t(void *, void *)> discard_frame_capture;
    std::function<std::uint32_t()> is_frame_capturing;
};

struct RenderDocCaptureResult {
    std::filesystem::path path;
    std::uint32_t capture_index = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t timestamp = 0;
};

struct RenderDocCaptureStatus {
    std::string status;
    RenderDocCaptureState state = RenderDocCaptureState::unavailable;
    std::string reason;
    std::string api_version;
    std::optional<RenderDocCaptureSource> source;
};

class RenderDocCaptureError : public std::runtime_error {
    std::string failure_reason;

  public:
    RenderDocCaptureError(std::string reason, std::string detail);
    const std::string &reason() const noexcept { return failure_reason; }
};

DECLARE_MODULE(RenderDocCapture) {
    RenderDocApiTable api;
    RenderDocCaptureState current_state = RenderDocCaptureState::unavailable;
    std::string unavailable_reason;
    std::string loaded_api_version;
    std::string last_error;
    std::optional<RenderDocCaptureSource> active_source;
    std::uint32_t capture_count_before = 0;
    bool shutting_down = false;

    [[noreturn]] void fail(std::string reason, std::string detail);
    void discardActiveCapture(void *device, void *window) noexcept;

  public:
    RenderDocCapture();
    explicit RenderDocCapture(RenderDocApiTable table, std::string api_version = "test");
    explicit RenderDocCapture(std::string unavailable_reason_for_testing);

    bool available() const noexcept { return current_state != RenderDocCaptureState::unavailable; }
    RenderDocCaptureState state() const noexcept { return current_state; }
    RenderDocCaptureStatus status() const;

    void request(RenderDocCaptureSource source, bool xr_active);
    RenderDocCaptureResult captureArmedFrame(std::uint64_t frame_index, void *device,
                                             void *window,
                                             const std::function<void()> &render_once);
    void beginShutdown() noexcept;
};

// RenderDoc's Vulkan API expects the dispatch-table pointer stored in a
// VkInstance, not the VkInstance handle itself. The implementation uses the
// pinned upstream helper macro; callers pass the raw VkInstance pointer value.
void *renderDocDevicePointerFromVulkanInstance(void *instance) noexcept;

} // namespace Pelican
