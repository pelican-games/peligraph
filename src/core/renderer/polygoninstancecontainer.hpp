#pragma once

#include "../container.hpp"
#include "../material/material.hpp"
#include "../model/modeltemplate.hpp"
#include "../vkcore/buf.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct PolygonInstance {
    vk::DrawIndexedIndirectCommand command;
    GlobalMaterialId material;
};

struct DrawIndirectInfo {
    GlobalMaterialId material;
    vk::DeviceSize offset, draw_count, stride;
};

struct ModelInstanceId {};

class PolygonInstanceContainer {
    DependencyContainer &con;
    std::vector<PolygonInstance> instances;
    BufferWrapper indirect_buf;
    std::vector<DrawIndirectInfo> draw_calls;

  public:
    PolygonInstanceContainer(DependencyContainer &con);
    ModelInstanceId placeModelInstance(ModelTemplate &model);
    void removeModelInstance(ModelInstanceId id);
    void triggerUpdate();
    const std::vector<PolygonInstance> &getPolygons() const;
    const BufferWrapper &getIndirectBuf() const;
    const std::vector<DrawIndirectInfo> &getDrawCalls() const;
};

} // namespace Pelican
