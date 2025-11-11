#pragma once

#include "../container.hpp"
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
  public:
    VertBufContainer(DependencyContainer &con);
    ModelTemplate::PrimitiveRefInfo addPrimitiveEntry(CommonPolygonVertData &&data);
    void removePrimitiveEntry(/* TODO */);
    vk::Buffer getVertexBuffer(/* TODO */) const;
};

} // namespace Pelican
