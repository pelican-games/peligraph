#pragma once

#include "../container.hpp"
#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
#include "../vkcore/buf.hpp"
#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct RenderCommand {
    vk::DrawIndexedIndirectCommand command;
    GlobalMaterialId material;
};

struct DrawIndirectInfo {
    GlobalMaterialId material;
    vk::DeviceSize offset;
    uint32_t draw_count, stride;
};

struct ModelInstanceId {
    uint32_t value;
};

DECLARE_MODULE(PolygonInstanceContainer) {
    
    std::vector<RenderCommand> render_commands;
    BufferWrapper indirect_buf;
    std::vector<DrawIndirectInfo> draw_calls;

    std::vector<glm::mat4> model_instances_data;
    BufferWrapper model_data_buffer;

  public:
    PolygonInstanceContainer();
    ModelInstanceId placeModelInstance(ModelTemplate &model);
    void removeModelInstance(ModelInstanceId id);
    void triggerUpdate();

    const BufferWrapper &getIndirectBuf() const;
    const BufferWrapper &getObjectBuf() const;
    const std::vector<DrawIndirectInfo> &getDrawCalls() const;
};

} // namespace Pelican
