#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class Camera;
class FrameResources;
class LightContainer;
class MaterialContainer;
class PolygonInstanceContainer;
class ShadowDepthPassContainer;
class VelocityPassContainer;
class VertBufContainer;

struct MaterialRendererDependencies {
    PolygonInstanceContainer &instance_container;
    const VertBufContainer &vert_buf_container;
    const MaterialContainer &material_container;
    const FrameResources &frame_resources;
    LightContainer &light_container;
    const Camera &camera;
    glm::mat4 view_projection{1.0f};
    bool first_person_view = false;
};

DECLARE_MODULE(MaterialRenderer) {
  public:
    MaterialRenderer();
    void render(vk::CommandBuffer cmd_buf, PassId pass_id,
                const PassDefinition &pass,
                const MaterialRendererDependencies &dependencies) const;

    void renderWithMaterialRange(vk::CommandBuffer cmd_buf, PassId pass_id,
                                 const PassDefinition &pass,
                                 uint32_t material_start, uint32_t material_count,
                                 const MaterialRendererDependencies &dependencies) const;
    void renderShadowDepth(vk::CommandBuffer cmd_buf, PassId pass_id,
                           const ShadowDepthPassContainer &shadow_depth_pass_container,
                           const MaterialRendererDependencies &dependencies) const;
    void renderVelocity(vk::CommandBuffer cmd_buf, PassId pass_id,
                        const VelocityPassContainer &velocity_pass_container,
                        const MaterialRendererDependencies &dependencies) const;
};

} // namespace Pelican
