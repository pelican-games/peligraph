#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

class VertBufContainer {
  public:
    VertBufContainer(DependencyContainer &con);
    uint32_t addPolygonEntry(/* TODO */);
    void removePolygonEntry(/* TODO */);
    vk::Buffer getVertexBuffer(/* TODO */) const;
};

} // namespace Pelican
