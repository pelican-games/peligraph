#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct DebugUtilsExtensionSelection {
    bool requested = false;
    bool available = false;
    bool enabled = false;
    std::string reason{"disabled_by_launch_option"};
};

DebugUtilsExtensionSelection selectDebugUtilsExtension(bool requested,
                                                       std::span<const std::string> supported_extensions);

struct DebugUtilsFunctions {
    PFN_vkSetDebugUtilsObjectNameEXT set_object_name = nullptr;
    PFN_vkCmdBeginDebugUtilsLabelEXT begin_command_label = nullptr;
    PFN_vkCmdEndDebugUtilsLabelEXT end_command_label = nullptr;
    PFN_vkQueueBeginDebugUtilsLabelEXT begin_queue_label = nullptr;
    PFN_vkQueueEndDebugUtilsLabelEXT end_queue_label = nullptr;
};

struct DebugUtilsStatus {
    bool available = false;
    bool enabled = false;
    std::string reason{"not_initialized"};
    bool object_name = false;
    bool command_label = false;
    bool queue_label = false;
};

class DebugUtilsDispatch {
    vk::Device device;
    DebugUtilsFunctions functions;
    DebugUtilsStatus status;

    void setObjectName(vk::ObjectType type, std::uint64_t handle, const char *name) const noexcept;

  public:
    DebugUtilsDispatch() = default;
    DebugUtilsDispatch(vk::Device device, const DebugUtilsExtensionSelection &selection,
                       DebugUtilsFunctions functions) noexcept;

    static DebugUtilsDispatch resolve(vk::Instance instance, vk::Device device,
                                      const DebugUtilsExtensionSelection &selection) noexcept;

    const DebugUtilsStatus &getStatus() const noexcept { return status; }
    bool commandLabelsEnabled() const noexcept { return status.enabled && status.command_label; }

    void beginCommandLabel(vk::CommandBuffer command_buffer, const char *name,
                           std::array<float, 4> color = {0.18F, 0.55F, 0.85F, 1.0F}) const noexcept;
    void endCommandLabel(vk::CommandBuffer command_buffer) const noexcept;
    void beginQueueLabel(vk::Queue queue, const char *name,
                         std::array<float, 4> color = {0.18F, 0.55F, 0.85F, 1.0F}) const noexcept;
    void endQueueLabel(vk::Queue queue) const noexcept;

    void nameImage(vk::Image image, const char *name) const noexcept;
    void nameImageView(vk::ImageView image_view, const char *name) const noexcept;
    void nameSwapchain(vk::SwapchainKHR swapchain, const char *name) const noexcept;
    void nameBuffer(vk::Buffer buffer, const char *name) const noexcept;
    void nameDescriptorSet(vk::DescriptorSet descriptor_set, const char *name) const noexcept;
};

class ScopedCommandDebugLabel {
    const DebugUtilsDispatch *debug_utils = nullptr;
    vk::CommandBuffer command_buffer;

  public:
    ScopedCommandDebugLabel(const DebugUtilsDispatch &debug_utils, vk::CommandBuffer command_buffer,
                            const char *name) noexcept;
    ~ScopedCommandDebugLabel();
    void end() noexcept;
    ScopedCommandDebugLabel(const ScopedCommandDebugLabel &) = delete;
    ScopedCommandDebugLabel &operator=(const ScopedCommandDebugLabel &) = delete;
};

struct FrameGraphDebugLabelIdentity {
    std::uint64_t logical_frame = 0;
    std::string_view graph_variant;
    std::uint32_t view_index = 0;
    std::size_t node_ordinal = 0;
    std::string_view node_kind;
    std::string_view node_name;
};

std::string makeFrameGraphDebugLabel(const FrameGraphDebugLabelIdentity &identity);

template <class Barriers, class Body>
void recordDebugLabeledNode(const DebugUtilsDispatch &debug_utils, vk::CommandBuffer command_buffer,
                            const FrameGraphDebugLabelIdentity &identity, Barriers &&barriers, Body &&body) {
    if (!debug_utils.commandLabelsEnabled()) {
        barriers();
        body();
        return;
    }

    const auto node_name = makeFrameGraphDebugLabel(identity);
    ScopedCommandDebugLabel node_label{debug_utils, command_buffer, node_name.c_str()};
    {
        ScopedCommandDebugLabel barrier_label{debug_utils, command_buffer, "barriers"};
        barriers();
    }
    {
        ScopedCommandDebugLabel body_label{debug_utils, command_buffer, "body"};
        body();
    }
}

} // namespace Pelican
