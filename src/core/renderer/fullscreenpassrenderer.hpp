#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct FullscreenPassCameraData {
    glm::vec3 position{0.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 view{1.0f};
};

DECLARE_MODULE(FullscreenPassRenderer) {
  public:
    FullscreenPassRenderer();
    ~FullscreenPassRenderer();

    void render(vk::CommandBuffer cmd_buf, PassId pass_id, const PassDefinition &pass_def,
                const FullscreenPassCameraData &camera_data) const;
};

} // namespace Pelican
