#pragma once

#include "../container.hpp"
#include "../vkcore/image.hpp"
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
    DependencyContainer&con;

    vk::Device device;
    std::vector<vk::UniqueDescriptorSetLayout> descset_layouts;
    vk::UniquePipelineLayout pipeline_layout;

    vk::UniqueSampler nearest_sampler, linear_sampler;
    vk::UniqueDescriptorPool desc_pool;

    struct InternalTextureResource {
        ImageWrapper image;
        vk::UniqueImageView image_view;
        vk::UniqueDescriptorSet descset;
    };
    std::map<GlobalTextureId, InternalTextureResource> textures;

    struct InternalMaterialInfo {
        vk::UniquePipeline pipeline;
        GlobalTextureId base_color_texture;
    };
    std::map<GlobalMaterialId, InternalMaterialInfo> materials;
    std::map<GlobalShaderId, vk::UniqueShaderModule> shaders;

  public:
    MaterialContainer(DependencyContainer &con);
    ~MaterialContainer();

    GlobalShaderId registerShader(/* TODO */);
    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data);
    GlobalMaterialId registerMaterial(MaterialInfo info);

    void bindResource(vk::CommandBuffer cmd_buf, GlobalMaterialId material) const;
    vk::PipelineLayout getPipelineLayout() const;
};

} // namespace Pelican