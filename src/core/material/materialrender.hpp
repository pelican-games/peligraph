#pragma once

#include "../container.hpp"
#include <map>
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct Material {
    uint32_t pipeline_id;
};

class MaterialRenderer {
    DependencyContainer &con;

    vk::UniquePipelineLayout pipeline_layout;
    std::unordered_map<uint32_t, vk::UniqueShaderModule> shaders;
    std::unordered_map<uint32_t, vk::UniquePipeline> pipelines;

    std::map<uint32_t, Material> materials;

  public:
    MaterialRenderer(DependencyContainer &con);
    void render(vk::CommandBuffer cmd_buf) const;
};

} // namespace Pelican
