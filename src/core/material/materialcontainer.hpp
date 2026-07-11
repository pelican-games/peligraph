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
#include <utility>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct PushConstantStruct {
    glm::mat4 mvp;
};

static_assert(sizeof(PushConstantStruct) == PELICAN_PUSH_ENGINE_BYTES);

struct MaterialIndexPushConstant {
    uint32_t material_index = 0;
};

static_assert(sizeof(MaterialIndexPushConstant) <= PELICAN_PUSH_SHADER_BYTES);

DECLARE_MODULE(MaterialContainer) {

    vk::Device device;

    vk::UniqueSampler nearest_sampler, linear_sampler;
    vk::UniqueDescriptorPool desc_pool;

    struct InternalTextureResource {
        ImageWrapper image;
        vk::UniqueImageView linear_view;
        vk::UniqueImageView srgb_view;
    };
    ResourceContainer<GlobalTextureId, InternalTextureResource> textures;

    struct InternalMaterialInfo {
        PipelineHandle pipeline;
        GlobalTextureId base_color_texture;
        GlobalTextureId metallic_roughness_texture;
        GlobalTextureId normal_texture;
        GlobalTextureId emissive_texture;
        std::optional<MaterialInfo::VatPlaybackInfo> vat;
        vk::UniqueDescriptorSet descset;
    };
    std::unordered_map<std::string, PipelineHandle> pipelines;
    std::optional<PipelineHandle> default_pipeline;
    ResourceContainer<GlobalMaterialId, InternalMaterialInfo> materials;
    BufferWrapper material_buffer;

  public:
    MaterialContainer();
    ~MaterialContainer();

    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data);
    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data, vk::Format format,
                                    vk::DeviceSize bytes_num);
    GlobalMaterialId registerMaterial(MaterialInfo info);
    std::pair<vk::ImageView, vk::ImageView> textureViewsForTesting(GlobalTextureId texture) const;
    size_t textureCountForTesting() const { return textures.size(); }
    size_t materialCountForTesting() const { return materials.size(); }

    bool isRenderRequired(PassId pass_id, GlobalMaterialId material) const;
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id, GlobalMaterialId material,
                      GlobalMaterialId prev_material_id) const;
    vk::PipelineLayout getPipelineLayout() const;
    vk::PipelineLayout pipelineLayout(GlobalMaterialId material) const;
};

} // namespace Pelican
