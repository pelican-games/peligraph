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
    void render(vk::CommandBuffer command_buffer, vk::ImageView target_view,
                vk::Extent2D target_extent, vk::Format target_format);
    std::uint64_t publicApiCallCountForTesting() const noexcept;
};

} // namespace Pelican
