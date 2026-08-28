#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FullscreenPassContainer;
class FrameResources;

struct FullscreenDrawCommand {
    std::uint32_t vertex_count = 6;
    std::uint32_t instance_count = 1;
    std::uint32_t first_vertex = 0;
    std::uint32_t first_instance = 0;

    bool operator==(const FullscreenDrawCommand &) const = default;
};

FullscreenDrawCommand fullscreenDrawCommand(
    const FullscreenPassInfo &fullscreen_info) noexcept;

struct FullscreenPassRendererDependencies {
    FullscreenPassContainer &fullscreen_pass_container;
    const FrameResources &frame_resources;
};

DECLARE_MODULE(FullscreenPassRenderer) {
  public:
    FullscreenPassRenderer();
    ~FullscreenPassRenderer();

    void render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                const FullscreenPassRendererDependencies &dependencies,
                RenderPassViewInvocation invocation = {}) const;
};

} // namespace Pelican
