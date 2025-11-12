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

struct CommonVertStruct {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 texcoord;
    glm::vec3 color;
};

struct CommonSkinningVertStruct {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec2 texcoord;
    glm::vec3 color;
    glm::i16vec4 joint;
    glm::vec4 weight;
};

class VertBufContainer {
    DependencyContainer &con;

    uint32_t indices_offset;
    int32_t vertices_offset;
    uint32_t indices_cap;
    uint32_t vertices_cap;
    BufferWrapper indices_mem_pool;
    BufferWrapper vertices_mem_pool;

  public:
    VertBufContainer(DependencyContainer &con);
    ModelTemplate::PrimitiveRefInfo addPrimitiveEntry(CommonPolygonVertData &&data);
    void removePrimitiveEntry(/* TODO */);

    void bindVertexBuffer(vk::CommandBuffer cmd_buf /* TODO */) const;

    struct CommonVertDataDescription {
        std::vector<vk::VertexInputBindingDescription> binding_descs;
        std::vector<vk::VertexInputAttributeDescription> attr_descs;
    };
    static CommonVertDataDescription getDescription();
};

} // namespace Pelican
