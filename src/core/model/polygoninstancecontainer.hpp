#pragma once

#include "../container.hpp"
#include "../material/material.hpp"
#include "modeltemplate.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct PolygonInstance {
    vk::DrawIndexedIndirectCommand command;
    GlobalMaterialId material;
};

struct ModelInstanceId {};

class PolygonInstanceContainer {
  public:
    PolygonInstanceContainer(DependencyContainer &con);
    ModelInstanceId placeModelInstance(ModelTemplate &model);
    void removeModelInstance(ModelInstanceId id);
    void triggerOptimize();
    const std::vector<PolygonInstance> &getPolygons() const;
};

} // namespace Pelican
