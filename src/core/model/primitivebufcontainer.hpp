#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

class PrimitiveBufContainer {
  public:
    PrimitiveBufContainer(DependencyContainer &con);
    uint32_t addPrimitiveGroup(/* TODO */);
    void addPrimitive(/* TODO */);
    void removePrimitive(/* TODO */);
    void removePrimitiveGroup(/* TODO */);
    vk::Buffer getIndirectDrawBuffer(/* TODO */) const;
    vk::Buffer getInstanceBuffer(/* TODO */) const;
};

} // namespace Pelican
