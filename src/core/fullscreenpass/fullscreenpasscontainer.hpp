#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(FullscreenPassContainer) {
    
    vk::Device device;
    std::vector<vk::UniqueDescriptorSetLayout> descset_layouts;
    vk::UniquePipelineLayout pipeline_layout;

    vk::UniqueSampler nearest_sampler, linear_sampler;
    vk::UniqueDescriptorPool desc_pool;

    PELICAN_DEFINE_HANDLE(PipelineId, uint32_t)
    std::unordered_map<PipelineId, vk::UniquePipeline, PipelineId::Hash> pipelines;

    // パスごとの descriptor set と入力情報を保持
    struct InputTextureInfo {
        vk::UniqueDescriptorSet descset;
        std::vector<GlobalRenderTargetId> input_rt_ids;
    };
    std::unordered_map<int, InputTextureInfo> input_textures;  // pass_id -> InputTextureInfo

  public:
    FullscreenPassContainer();
    ~FullscreenPassContainer();

    PipelineId registerFullscreenPass(vk::Format colorFormat, vk::ShaderModule vertShader, vk::ShaderModule fragShader);
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id);
    void setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId>& input_rts);
    vk::PipelineLayout getPipelineLayout() const { return pipeline_layout.get(); }
};

} // namespace Pelican
