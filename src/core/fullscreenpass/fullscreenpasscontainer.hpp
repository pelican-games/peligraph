#pragma once

#include "../container.hpp"
#include "fullscreeninputbindingmismatch.hpp"
#include "../renderingpass/passinputdescriptorvariant.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <cstdint>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetImageViewResolver;
class FrameGraphResourceContainer;

// One image input of a fullscreen / generic-raster pass. The per-input
// metadata travels as one value so the "every array is either empty or
// exactly as long as the target array" invariant holds by construction
// instead of being re-checked at every entry point.
struct FullscreenPassInput {
    GlobalRenderTargetId target = noRenderTargetId();
    bool history = false;
    FullscreenInputSampling sampling{};
    PassInputViewDimension view =
        PassInputViewDimension::shared_2d;
    std::optional<ImageSubresourceRange> subresource;
    // Absent derives the descriptor view from `view`: layered and family
    // array inputs bind a 2D array view, everything else a plain 2D view.
    std::optional<ImageSubresourceViewDimension>
        view_dimension;
    bool local_read = false;

    ImageSubresourceViewDimension
    resolvedViewDimension() const {
        if (view_dimension) return *view_dimension;
        return view == PassInputViewDimension::
                           layered_2d_array ||
                       view ==
                           PassInputViewDimension::
                               family_2d_array
                   ? ImageSubresourceViewDimension::
                         two_d_array
                   : ImageSubresourceViewDimension::two_d;
    }
};

// Q-14f: a pass-input binding the shader declares and the pass does not
// supply, bound to one of the container's default resources instead.
struct FullscreenFilledPassInput {
    std::uint32_t binding = 0;
    vk::DescriptorType type = vk::DescriptorType::eCombinedImageSampler;
    std::uint32_t count = 1;
    ReflectedImageViewDimension image_view_dimension =
        ReflectedImageViewDimension::none;
    // VI-1: one of the shader's variadic inputs. Filling it is what an
    // unconnected variadic input is for, so it is not warned about.
    bool variadic = false;
};

// VI-1: a pass's pass-input set as its pipeline's shader declares it, and
// how many inputs the pass supplies (get_frame_plan's pass_inputs).
struct FullscreenPassInputReport {
    // Binding order.
    std::vector<FullscreenShaderPassInput> declarations;
    std::optional<PassVariadicInputRange> variadic;
    std::size_t texture_inputs = 0;
    std::size_t buffer_inputs = 0;
    // Where the pass's buffer inputs start (passBufferInputBinding).
    std::uint32_t buffer_binding_base = 0;
    // Set when the published shader breaks the variadic layout rule (after
    // a shader reload): the refusal the pass compiler would give.
    std::optional<std::string> layout_refusal;
};

DECLARE_MODULE(FullscreenPassContainer) {
  public:
    PELICAN_DEFINE_HANDLE(PipelineId, uint32_t)

  private:
    vk::Device device;

    // Q-14f: what an unsupplied pass-input binding reads: a 1x1 image of
    // zeros (0,0,0,0) seen as 2D, 2D array and cube, and a zero-filled
    // storage buffer. Created the first time a pass needs them.
    struct DefaultPassInputResources;
    std::unique_ptr<DefaultPassInputResources> default_inputs;
    const DefaultPassInputResources &defaultPassInputResources();
    // Writes the defaults into `set` and returns how many descriptor writes
    // it submitted; defaultPassInputResources() must have been called first.
    std::size_t writeFilledPassInputs(
        vk::DescriptorSet set,
        std::span<const FullscreenFilledPassInput> filled) const;

    std::array<vk::UniqueSampler, 6> input_samplers;
    vk::UniqueDescriptorPool desc_pool;

    std::unordered_map<PipelineId, PipelineHandle, PipelineId::Hash> pipelines;
    std::vector<PipelineId> registration_order;
    uint32_t next_pipeline_id = 0;

    struct InputTextureInfo {
        std::vector<PassInputDescriptorVariant> variants;
        std::vector<FullscreenPassInput> inputs;
        std::vector<FrameGraphBufferId> input_buffer_ids;
        std::optional<FrameGraphBufferId> index_buffer;
        GraphicsPipelineViewContract view;
        std::uint32_t logical_view_count = 1;
        uint64_t binding_revision = 0;
        // Q-14f: declared bindings past the inputs, bound to defaults.
        std::vector<FullscreenFilledPassInput> filled;
        std::optional<FullscreenInputBindingMismatchInfo> fill_report;
        // VI-1: the shader's variadic group; it places the buffer inputs.
        std::optional<PassVariadicInputRange> variadic;
        // Descriptor writes of defaults, over every variant and parity.
        std::size_t filled_descriptor_writes = 0;
    };
    std::unordered_map<int, InputTextureInfo> input_textures;
    uint64_t next_binding_revision = 1;

    // Descriptor-set construction for one pass: one set per (variant,
    // history parity) over the already validated input list.
    void buildInputDescriptorVariants(
        InputTextureInfo &info,
        vk::DescriptorSetLayout layout,
        const RenderTargetImageViewResolver &rt_views,
        const FrameGraphResourceContainer
            &frame_graph_resources,
        std::uint32_t variant_count) const;

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
                                      GraphicsPipelineViewContract view = {},
                                      bool allow_ray_query = true);
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
            resource_interface = {},
        bool allow_ray_query = true);
    // Generic procedural raster passes share descriptor ownership and
    // registration lifetime with the legacy fullscreen path. The complete
    // pipeline description is produced by a backend adapter before entering
    // this storage/execution container.
    PipelineId registerRasterPass(
        GraphicsPipelineDesc desc);
    void bindResource(vk::CommandBuffer cmd_buf, PassId pass_id,
                      RenderPassViewInvocation invocation = {});
    void setIndexBuffer(PassId pass_id, FrameGraphBufferId buffer);
    void setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                          const RenderTargetImageViewResolver &rt_views);
    void setInputResources(
        PassId pass_id,
        const std::vector<FullscreenPassInput> &inputs,
        const std::vector<std::string> &input_buffers,
        const RenderTargetImageViewResolver &rt_views,
        const FrameGraphResourceContainer &frame_graph_resources,
        GraphicsPipelineViewContract view = {},
        std::uint32_t logical_view_count = 0);
    void setInputResourcesById(
        PassId pass_id,
        const std::vector<FullscreenPassInput> &inputs,
        const std::vector<FrameGraphBufferId> &input_buffers,
        const RenderTargetImageViewResolver &rt_views,
        const FrameGraphResourceContainer &frame_graph_resources,
        GraphicsPipelineViewContract view = {},
        std::uint32_t logical_view_count = 0);
    void rebindInputResources(
        PassId pass_id,
        const RenderTargetImageViewResolver &rt_views,
        const FrameGraphResourceContainer &frame_graph_resources);
    // Q-14f: the bindings of the pass's shader that no input supplies and
    // that are bound to a default resource instead, in binding order. Empty
    // when the shader declares only what the pass supplies.
    std::vector<FullscreenFilledPassInput> filledPassInputs(
        PassId pass_id) const;
    // The lowest filled binding, in the words a shader_input_unsupplied
    // refusal would use (without the pass and fragment, which the pass
    // compiler adds); nullopt when nothing is filled.
    std::optional<FullscreenInputBindingMismatchInfo> filledPassInputReport(
        PassId pass_id) const;
    std::size_t filledDescriptorWritesForTesting(PassId pass_id) const;
    // VI-1: the pass's pipeline's pass-input declarations and the inputs
    // the pass supplies. Throws when the pass has no pipeline.
    FullscreenPassInputReport passInputReport(PassId pass_id) const;
    std::vector<vk::ImageView> boundInputImageViewsForTesting(
        PassId pass_id,
        std::uint32_t view_index = 0) const;
    std::vector<FullscreenInputSampling>
    inputSamplingForTesting(PassId pass_id) const;
    ShaderBundleId fragmentShaderForTesting(
        PassId pass_id) const;
    GraphicsPipelineDesc graphicsPipelineDescForTesting(
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
