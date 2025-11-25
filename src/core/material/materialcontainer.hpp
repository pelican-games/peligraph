#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
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

DECLARE_MODULE(MaterialContainer) {

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
    ResourceContainer<GlobalTextureId, InternalTextureResource> textures;

    PELICAN_DEFINE_HANDLE(PipelineId, uint32_t)
    struct InternalMaterialInfo {
        PipelineId pipeline;
        GlobalTextureId base_color_texture;
    };
    std::unordered_map<PipelineId, vk::UniquePipeline, PipelineId::Hash> pipelines;
    ResourceContainer<GlobalMaterialId, InternalMaterialInfo> materials;

    vk::UniqueDescriptorSet model_mat_buf_descset;

  public:
    MaterialContainer();
    ~MaterialContainer();

    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data);
    GlobalMaterialId registerMaterial(MaterialInfo info);

    void setModelMatBuf(const BufferWrapper &buf);

    bool isRenderRequired(PassId pass_id, GlobalMaterialId material) const;
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id, GlobalMaterialId material,
                      GlobalMaterialId prev_material_id) const;
    vk::PipelineLayout getPipelineLayout() const;
};

} // namespace Pelican