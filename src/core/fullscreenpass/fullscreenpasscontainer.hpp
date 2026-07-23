#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <cstdint>
#include <array>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetImageViewResolver;
class FrameGraphResourceContainer;

DECLARE_MODULE(FullscreenPassContainer) {
  public:
    PELICAN_DEFINE_HANDLE(PipelineId, uint32_t)

  private:
    vk::Device device;

    vk::UniqueSampler nearest_sampler, linear_sampler;
    vk::UniqueDescriptorPool desc_pool;

    std::unordered_map<PipelineId, PipelineHandle, PipelineId::Hash> pipelines;

    struct InputTextureInfo {
        std::array<vk::UniqueDescriptorSet, 2> descsets;
        std::vector<GlobalRenderTargetId> input_rt_ids;
        std::vector<bool> input_rt_history;
        std::vector<std::string> input_buffer_names;
        std::array<std::vector<vk::ImageView>, 2> bound_image_views;
        uint64_t binding_revision = 0;
    };
    std::unordered_map<int, InputTextureInfo> input_textures;
    uint64_t next_binding_revision = 1;

  public:
    FullscreenPassContainer();
    ~FullscreenPassContainer();

    PipelineId registerFullscreenPass(vk::Format colorFormat, ShaderBundleId vertShader, ShaderBundleId fragShader,
                                      std::vector<std::string> shader_defines = {},
                                      vk::SampleCountFlagBits samples =
                                          vk::SampleCountFlagBits::e1);
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id);
    void setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                          const RenderTargetImageViewResolver &rt_views);
    void setInputResources(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                           const std::vector<bool> &input_rt_history,
                           const std::vector<std::string> &input_buffers,
                           const RenderTargetImageViewResolver &rt_views,
                           const FrameGraphResourceContainer &frame_graph_resources);
    std::vector<vk::ImageView> boundInputImageViewsForTesting(PassId pass_id) const;
    uint64_t inputBindingRevisionForTesting(PassId pass_id) const;
    vk::PipelineLayout getPipelineLayout(PassId pass_id) const;
};

} // namespace Pelican
