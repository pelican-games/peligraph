#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

using RenderingPassId = uint64_t;
using PassId = uint64_t;

DECLARE_MODULE(RenderingPassContainer) {
  public:
    std::span<PassId> getPasses(RenderingPassId rendering_pass_id) const;
};

} // namespace Pelican
