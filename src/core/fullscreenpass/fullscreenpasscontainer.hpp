#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetImageViewResolver;

DECLARE_MODULE(FullscreenPassContainer) {
  public:
    PELICAN_DEFINE_HANDLE(PipelineId, uint32_t)

  private:
    vk::Device device;

    vk::UniqueSampler nearest_sampler, linear_sampler;
    vk::UniqueDescriptorPool desc_pool;

    std::unordered_map<PipelineId, PipelineHandle, PipelineId::Hash> pipelines;

    struct InputTextureInfo {
        vk::UniqueDescriptorSet descset;
        std::vector<GlobalRenderTargetId> input_rt_ids;
    };
    std::unordered_map<int, InputTextureInfo> input_textures;

  public:
    FullscreenPassContainer();
    ~FullscreenPassContainer();

    PipelineId registerFullscreenPass(vk::Format colorFormat, ShaderBundleId vertShader, ShaderBundleId fragShader,
                                      std::vector<std::string> shader_defines = {});
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id);
    void setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                          const RenderTargetImageViewResolver &rt_views);
    vk::PipelineLayout getPipelineLayout(PassId pass_id) const;
};

} // namespace Pelican
