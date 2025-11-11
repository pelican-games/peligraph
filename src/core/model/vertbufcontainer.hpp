#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
#include "modeltemplate.hpp"
#include <glm/glm.hpp>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct CommonPolygonVertData {
    std::vector<uint32_t> indices;
    std::vector<glm::vec3> pos;
    std::vector<glm::vec3> normal;
    std::vector<glm::vec2> texcoord;
    std::vector<glm::vec3> color;
    std::vector<glm::i16vec4> joint;
    std::vector<glm::vec4> weight;
};

class VertBufContainer {
    DependencyContainer &con;
    BufferWrapper indices_mem_pool;
    BufferWrapper vertices_mem_pool;

    uint32_t indices_offset;
    uint32_t vertices_offset;

  public:
    VertBufContainer(DependencyContainer &con);
    ModelTemplate::PrimitiveRefInfo addPrimitiveEntry(CommonPolygonVertData &&data);
    void removePrimitiveEntry(/* TODO */);

    void bindVertexBuffer(vk::CommandBuffer cmd_buf /* TODO */) const;

    struct CommonVertDataDescription {
        std::vector<vk::VertexInputBindingDescription> binding_descs;
        std::vector<vk::VertexInputAttributeDescription> attr_descs;
    };
    CommonVertDataDescription getDescription() const;
};

} // namespace Pelican
