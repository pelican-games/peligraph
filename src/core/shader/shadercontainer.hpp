#pragma once

#include "../container.hpp"
#include "shader.hpp"
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(ShaderContainer) {
    ResourceContainer<GlobalShaderId, vk::UniqueShaderModule> shaders;

  public:
    ShaderContainer();

    GlobalShaderId registerShader(size_t len, const char *data);
    vk::ShaderModule getShader(GlobalShaderId id) const;
};

} // namespace Pelican
