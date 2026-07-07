#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

class Camera;
class LightContainer;
class MaterialContainer;
class PolygonInstanceContainer;
class ShadowDepthPassContainer;
class VertBufContainer;

struct MaterialRendererDependencies {
    PolygonInstanceContainer &instance_container;
    const VertBufContainer &vert_buf_container;
    const MaterialContainer &material_container;
    LightContainer &light_container;
    const Camera &camera;
    double time_seconds = 0.0;
};

DECLARE_MODULE(MaterialRenderer) {
  public:
    MaterialRenderer();
    void render(vk::CommandBuffer cmd_buf, PassId pass_id,
                const MaterialRendererDependencies &dependencies) const;

    void renderWithMaterialRange(vk::CommandBuffer cmd_buf, PassId pass_id,
                                 uint32_t material_start, uint32_t material_count,
                                 const MaterialRendererDependencies &dependencies) const;
    void renderShadowDepth(vk::CommandBuffer cmd_buf, PassId pass_id,
                           const ShadowDepthPassContainer &shadow_depth_pass_container,
                           const MaterialRendererDependencies &dependencies) const;
};

} // namespace Pelican
