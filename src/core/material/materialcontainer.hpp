#pragma once

#include "../container.hpp"
#include "../vkcore/buf.hpp"
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
    DependencyContainer &con;

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

    using PipelineId = uint32_t;
    struct InternalMaterialInfo {
        PipelineId pipeline;
        GlobalTextureId base_color_texture;
    };
    std::map<GlobalShaderId, vk::UniqueShaderModule> shaders;
    std::map<PipelineId, vk::UniquePipeline> pipelines;
    std::map<GlobalMaterialId, InternalMaterialInfo> materials;

    vk::UniqueDescriptorSet model_mat_buf_descset;

  public:
    MaterialContainer(DependencyContainer &con);
    ~MaterialContainer();

    GlobalShaderId registerShader(size_t len, const char *data);
    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data);
    GlobalMaterialId registerMaterial(MaterialInfo info);

    void setModelMatBuf(const BufferWrapper &buf);

    void bindResource(vk::CommandBuffer cmd_buf, GlobalMaterialId material, GlobalMaterialId prev_material_id) const;
    vk::PipelineLayout getPipelineLayout() const;
};

} // namespace Pelican