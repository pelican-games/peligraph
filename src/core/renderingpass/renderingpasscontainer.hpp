#pragma once

#include "../container.hpp"
#include "renderingpass.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(RenderingPassContainer) {
  public:
    std::span<PassId> getPasses(RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican
