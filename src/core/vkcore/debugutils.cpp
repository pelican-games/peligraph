#include "debugutils.hpp"

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace Pelican {

namespace {

template <class Handle> std::uint64_t objectHandleValue(Handle handle) noexcept {
    if constexpr (std::is_pointer_v<Handle>) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

DebugUtilsFunctions resolveFunctions(vk::Instance instance) noexcept {
    const auto raw_instance = static_cast<VkInstance>(instance);
    return DebugUtilsFunctions{
        reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(raw_instance, "vkSetDebugUtilsObjectNameEXT")),
        reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(raw_instance, "vkCmdBeginDebugUtilsLabelEXT")),
        reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(raw_instance, "vkCmdEndDebugUtilsLabelEXT")),
        reinterpret_cast<PFN_vkQueueBeginDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(raw_instance, "vkQueueBeginDebugUtilsLabelEXT")),
        reinterpret_cast<PFN_vkQueueEndDebugUtilsLabelEXT>(
            vkGetInstanceProcAddr(raw_instance, "vkQueueEndDebugUtilsLabelEXT")),
    };
}

} // namespace

DebugUtilsExtensionSelection selectDebugUtilsExtension(bool requested,
                                                       std::span<const std::string> supported_extensions) {
    const bool available = std::find(supported_extensions.begin(), supported_extensions.end(),
                                     VK_EXT_DEBUG_UTILS_EXTENSION_NAME) != supported_extensions.end();
    if (!requested) {
        return {false, available, false, "disabled_by_launch_option"};
    }
    if (!available) {
        return {true, false, false, "VK_EXT_debug_utils_unavailable"};
    }
    return {true, true, true, "enabled"};
}

DebugUtilsDispatch::DebugUtilsDispatch(vk::Device device, const DebugUtilsExtensionSelection &selection,
                                       DebugUtilsFunctions functions) noexcept
    : device{device}, functions{functions} {
    status.available = selection.available;
    status.enabled = selection.enabled;
    status.reason = selection.reason;
    status.object_name = selection.enabled && functions.set_object_name != nullptr;
    status.command_label =
        selection.enabled && functions.begin_command_label != nullptr && functions.end_command_label != nullptr;
    status.queue_label =
        selection.enabled && functions.begin_queue_label != nullptr && functions.end_queue_label != nullptr;
    if (selection.enabled && (!status.object_name || !status.command_label || !status.queue_label)) {
        status.enabled = false;
        status.reason = "required_function_unavailable";
    }
}

DebugUtilsDispatch DebugUtilsDispatch::resolve(vk::Instance instance, vk::Device device,
                                               const DebugUtilsExtensionSelection &selection) noexcept {
    return DebugUtilsDispatch{device, selection,
                              selection.enabled ? resolveFunctions(instance) : DebugUtilsFunctions{}};
}

void DebugUtilsDispatch::setObjectName(vk::ObjectType type, std::uint64_t handle, const char *name) const noexcept {
    if (!status.enabled || !status.object_name || handle == 0 || name == nullptr || *name == '\0') {
        return;
    }
    vk::DebugUtilsObjectNameInfoEXT info;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    (void)functions.set_object_name(static_cast<VkDevice>(device),
                                    reinterpret_cast<const VkDebugUtilsObjectNameInfoEXT *>(&info));
}

void DebugUtilsDispatch::beginCommandLabel(vk::CommandBuffer command_buffer, const char *name,
                                           std::array<float, 4> color) const noexcept {
    if (!commandLabelsEnabled() || !command_buffer || name == nullptr || *name == '\0') {
        return;
    }
    vk::DebugUtilsLabelEXT label;
    label.pLabelName = name;
    label.color = color;
    functions.begin_command_label(static_cast<VkCommandBuffer>(command_buffer),
                                  reinterpret_cast<const VkDebugUtilsLabelEXT *>(&label));
}

void DebugUtilsDispatch::endCommandLabel(vk::CommandBuffer command_buffer) const noexcept {
    if (!commandLabelsEnabled() || !command_buffer)
        return;
    functions.end_command_label(static_cast<VkCommandBuffer>(command_buffer));
}

void DebugUtilsDispatch::beginQueueLabel(vk::Queue queue, const char *name, std::array<float, 4> color) const noexcept {
    if (!status.enabled || !status.queue_label || !queue || name == nullptr || *name == '\0') {
        return;
    }
    vk::DebugUtilsLabelEXT label;
    label.pLabelName = name;
    label.color = color;
    functions.begin_queue_label(static_cast<VkQueue>(queue), reinterpret_cast<const VkDebugUtilsLabelEXT *>(&label));
}

void DebugUtilsDispatch::endQueueLabel(vk::Queue queue) const noexcept {
    if (!status.enabled || !status.queue_label || !queue)
        return;
    functions.end_queue_label(static_cast<VkQueue>(queue));
}

void DebugUtilsDispatch::nameImage(vk::Image image, const char *name) const noexcept {
    setObjectName(vk::ObjectType::eImage, objectHandleValue(static_cast<VkImage>(image)), name);
}

void DebugUtilsDispatch::nameImageView(vk::ImageView image_view, const char *name) const noexcept {
    setObjectName(vk::ObjectType::eImageView, objectHandleValue(static_cast<VkImageView>(image_view)), name);
}

void DebugUtilsDispatch::nameSwapchain(vk::SwapchainKHR swapchain, const char *name) const noexcept {
    setObjectName(vk::ObjectType::eSwapchainKHR, objectHandleValue(static_cast<VkSwapchainKHR>(swapchain)), name);
}

void DebugUtilsDispatch::nameBuffer(vk::Buffer buffer, const char *name) const noexcept {
    setObjectName(vk::ObjectType::eBuffer, objectHandleValue(static_cast<VkBuffer>(buffer)), name);
}

void DebugUtilsDispatch::nameDescriptorSet(vk::DescriptorSet descriptor_set, const char *name) const noexcept {
    setObjectName(vk::ObjectType::eDescriptorSet, objectHandleValue(static_cast<VkDescriptorSet>(descriptor_set)),
                  name);
}

ScopedCommandDebugLabel::ScopedCommandDebugLabel(const DebugUtilsDispatch &debug_utils,
                                                 vk::CommandBuffer command_buffer, const char *name) noexcept
    : debug_utils{debug_utils.commandLabelsEnabled() && name != nullptr && *name != '\0'
                      ? &debug_utils
                      : nullptr},
      command_buffer{command_buffer} {
    if (this->debug_utils != nullptr) {
        this->debug_utils->beginCommandLabel(command_buffer, name);
    }
}

ScopedCommandDebugLabel::~ScopedCommandDebugLabel() {
    end();
}

void ScopedCommandDebugLabel::end() noexcept {
    if (debug_utils != nullptr)
        debug_utils->endCommandLabel(command_buffer);
    debug_utils = nullptr;
}

std::string makeFrameGraphDebugLabel(const FrameGraphDebugLabelIdentity &identity) {
    return "frame/" + std::to_string(identity.logical_frame) + "/graph/" + std::string{identity.graph_variant} +
           "/view/" + std::to_string(identity.view_index) + "/node/" + std::to_string(identity.node_ordinal) + ":" +
           std::string{identity.node_kind} + ":" + std::string{identity.node_name};
}

} // namespace Pelican
