#pragma once

#include "../container.hpp"

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class InputState;

DECLARE_MODULE(ImGuiSystem) {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    ImGuiSystem();
    ~ImGuiSystem();

    void routeInputAndBeginFrame(InputState &input);
    void endFrameIfStarted();
    void render(vk::CommandBuffer command_buffer, vk::ImageView target_view,
                vk::Extent2D target_extent, vk::Format target_format,
                vk::AttachmentLoadOp load_op,
                vk::AttachmentStoreOp store_op,
                vk::ClearColorValue clear_color,
                vk::ImageView resolve_view = {},
                vk::ResolveModeFlagBits resolve_mode =
                    vk::ResolveModeFlagBits::eNone);
    std::uint64_t publicApiCallCountForTesting() const noexcept;
};

} // namespace Pelican
