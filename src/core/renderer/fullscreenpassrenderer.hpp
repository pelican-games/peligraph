#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FullscreenPassContainer;
class FrameResources;

struct FullscreenPassRendererDependencies {
    FullscreenPassContainer &fullscreen_pass_container;
    const FrameResources &frame_resources;
};

DECLARE_MODULE(FullscreenPassRenderer) {
  public:
    FullscreenPassRenderer();
    ~FullscreenPassRenderer();

    void render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                const FullscreenPassRendererDependencies &dependencies) const;
};

} // namespace Pelican
