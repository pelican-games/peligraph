#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../resourcecontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/image.hpp"
#include "material.hpp"
#include <glm/glm.hpp>
#include <map>
#include <optional>
#include <span>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct PushConstantStruct {
    glm::mat4 mvp;
};

static_assert(sizeof(PushConstantStruct) == PELICAN_PUSH_ENGINE_BYTES);

DECLARE_MODULE(MaterialContainer) {

    vk::Device device;

    vk::UniqueSampler nearest_sampler, linear_sampler;
    vk::UniqueDescriptorPool desc_pool;

    struct InternalTextureResource {
        ImageWrapper image;
        vk::UniqueImageView image_view;
    };
    ResourceContainer<GlobalTextureId, InternalTextureResource> textures;

    struct InternalMaterialInfo {
        PipelineHandle pipeline;
        GlobalTextureId base_color_texture;
        GlobalTextureId metallic_roughness_texture;
        GlobalTextureId normal_texture;
        GlobalTextureId emissive_texture;
        vk::UniqueDescriptorSet descset;
    };
    std::unordered_map<uint64_t, PipelineHandle> pipelines;
    std::optional<PipelineHandle> default_pipeline;
    ResourceContainer<GlobalMaterialId, InternalMaterialInfo> materials;

    vk::Buffer model_mat_buffer;
    vk::UniqueDescriptorSet model_mat_buf_descset;

  public:
    MaterialContainer();
    ~MaterialContainer();

    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data);
    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data, vk::Format format,
                                    vk::DeviceSize bytes_num);
    GlobalMaterialId registerMaterial(MaterialInfo info);

    void setModelMatBuf(const BufferWrapper &buf);

    bool isRenderRequired(PassId pass_id, GlobalMaterialId material) const;
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id, GlobalMaterialId material,
                      GlobalMaterialId prev_material_id) const;
    vk::PipelineLayout getPipelineLayout() const;
};

} // namespace Pelican
