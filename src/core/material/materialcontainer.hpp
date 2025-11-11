#pragma once

#include "../container.hpp"
#include "material.hpp"
#include <glm/glm.hpp>
#include <map>
#include <span>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct PushConstantStruct {
    glm::mat4 mvp;
};

class MaterialContainer {
    vk::Device device;
    vk::UniquePipelineLayout pipeline_layout;

    struct InternalMaterialInfo {
        vk::UniquePipeline pipeline;
        // vk::DescriptorSet descset;
    };
    std::map<GlobalMaterialId, InternalMaterialInfo> materials;
    std::map<GlobalShaderId, vk::UniqueShaderModule> shaders;

  public:
    MaterialContainer(DependencyContainer &con);
    ~MaterialContainer();

    GlobalShaderId registerShader(/* TODO */);
    GlobalTextuerId registerTexture(/* TODO */);
    GlobalMaterialId registerMaterial(MaterialInfo info);

    void bindResource(vk::CommandBuffer cmd_buf, GlobalMaterialId material) const;
    vk::PipelineLayout getPipelineLayout() const;
};

} // namespace Pelican