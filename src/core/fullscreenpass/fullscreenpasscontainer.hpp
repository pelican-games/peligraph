#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <cstdint>
#include <array>
#include <optional>
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

    std::array<vk::UniqueSampler, 6> input_samplers;
    vk::UniqueDescriptorPool desc_pool;

    std::unordered_map<PipelineId, PipelineHandle, PipelineId::Hash> pipelines;
    std::vector<PipelineId> registration_order;
    uint32_t next_pipeline_id = 0;

    struct InputTextureInfo {
        struct DescriptorVariant {
            std::array<vk::UniqueDescriptorSet, 2> descsets;
            std::array<std::vector<vk::ImageView>, 2>
                bound_image_views;
        };
        std::vector<DescriptorVariant> variants;
        std::vector<GlobalRenderTargetId> input_rt_ids;
        std::vector<bool> input_rt_history;
        std::vector<PassInputViewDimension> input_rt_views;
        std::vector<ImageSubresourceViewDimension>
            input_view_dimensions;
        std::vector<std::optional<ImageSubresourceRange>>
            input_rt_subresources;
        std::vector<FullscreenInputSampling> input_sampling;
        std::vector<bool> input_local_reads;
        std::vector<FrameGraphBufferId> input_buffer_ids;
        GraphicsPipelineViewContract view;
        std::uint32_t logical_view_count = 1;
        uint64_t binding_revision = 0;
    };
    std::unordered_map<int, InputTextureInfo> input_textures;
    uint64_t next_binding_revision = 1;

  public:
    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
        std::uint64_t next_binding_revision = 1;
        uint32_t next_pipeline_id = 0;
    };

    FullscreenPassContainer();
    ~FullscreenPassContainer();

    PipelineId registerFullscreenPass(vk::Format colorFormat, ShaderBundleId vertShader, ShaderBundleId fragShader,
                                      std::vector<std::string> shader_defines = {},
                                      vk::SampleCountFlagBits samples =
                                          vk::SampleCountFlagBits::e1,
                                      GraphicsPipelineViewContract view = {});
    PipelineId registerFullscreenPass(
        std::vector<vk::Format> color_formats,
        std::optional<vk::Format> depth_format,
        ShaderBundleId vert_shader,
        ShaderBundleId frag_shader,
        std::vector<std::string> shader_defines,
        vk::SampleCountFlagBits samples,
        GraphicsPipelineViewContract view,
        GraphicsPipelineRenderingLocalReadContract
            local_read,
        std::vector<ShaderResourceInterfaceBinding>
            resource_interface = {});
    // Generic procedural raster passes share descriptor ownership and
    // registration lifetime with the legacy fullscreen path. The complete
    // pipeline description is produced by a backend adapter before entering
    // this storage/execution container.
    PipelineId registerRasterPass(
        GraphicsPipelineDesc desc);
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id,
                      RenderPassViewInvocation invocation = {});
    void setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                          const RenderTargetImageViewResolver &rt_views);
    void setInputResources(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                           const std::vector<bool> &input_rt_history,
                           const std::vector<std::string> &input_buffers,
                           const RenderTargetImageViewResolver &rt_views,
                           const FrameGraphResourceContainer &frame_graph_resources,
                           const std::vector<FullscreenInputSampling> &input_sampling = {},
                           const std::vector<PassInputViewDimension> &input_views = {},
                           GraphicsPipelineViewContract view = {},
                           const std::vector<bool> &input_local_reads = {},
                           const std::vector<std::optional<ImageSubresourceRange>>
                               &input_subresources = {},
                           std::uint32_t logical_view_count = 0,
                           const std::vector<
                               ImageSubresourceViewDimension>
                               &input_view_dimensions = {});
    void setInputResourcesById(
        PassId pass_id,
        const std::vector<GlobalRenderTargetId> &input_rts,
        const std::vector<bool> &input_rt_history,
        const std::vector<FrameGraphBufferId> &input_buffers,
        const RenderTargetImageViewResolver &rt_views,
        const FrameGraphResourceContainer &frame_graph_resources,
        const std::vector<FullscreenInputSampling> &input_sampling = {},
        const std::vector<PassInputViewDimension> &input_views = {},
        GraphicsPipelineViewContract view = {},
        const std::vector<bool> &input_local_reads = {},
        const std::vector<std::optional<ImageSubresourceRange>>
            &input_subresources = {},
        std::uint32_t logical_view_count = 0,
        const std::vector<
            ImageSubresourceViewDimension>
            &input_view_dimensions = {});
    void rebindInputResources(
        PassId pass_id,
        const RenderTargetImageViewResolver &rt_views,
        const FrameGraphResourceContainer &frame_graph_resources);
    std::vector<vk::ImageView> boundInputImageViewsForTesting(
        PassId pass_id,
        std::uint32_t view_index = 0) const;
    std::vector<FullscreenInputSampling>
    inputSamplingForTesting(PassId pass_id) const;
    ShaderBundleId fragmentShaderForTesting(
        PassId pass_id) const;
    std::vector<ShaderResourceInterfaceBinding>
    resourceInterfaceForTesting(PassId pass_id) const;
    std::vector<bool>
    inputLocalReadsForTesting(PassId pass_id) const;
    uint64_t inputBindingRevisionForTesting(PassId pass_id) const;
    vk::PipelineLayout getPipelineLayout(PassId pass_id) const;

    RegistrationCheckpoint checkpointRegistrations() const noexcept;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<PipelineId>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
    void retireRegistrations(
        const std::vector<PipelineId> &ids) noexcept;
};

} // namespace Pelican
