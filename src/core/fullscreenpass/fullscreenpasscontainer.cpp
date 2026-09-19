#include "fullscreenpasscontainer.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

namespace {

// Descriptor-pool sizing estimate, not a per-pass ABI limit. A pass may use
// more bindings; the selected device/pipeline layout owns the real limit.
constexpr uint32_t estimatedInputBindingsPerPass = 8;

FullscreenPassContainer::PipelineId requireFullscreenPipelineId(PassId pass_id) {
    if (pass_id.value < 0) {
        throw std::runtime_error("Fullscreen pass id is invalid");
    }
    return FullscreenPassContainer::PipelineId{static_cast<uint32_t>(pass_id.value)};
}

PipelineHandle requirePipelineHandle(
    PassId pass_id,
    const std::unordered_map<FullscreenPassContainer::PipelineId, PipelineHandle,
                             FullscreenPassContainer::PipelineId::Hash> &pipelines) {
    const auto pipeline_id = requireFullscreenPipelineId(pass_id);
    auto found = pipelines.find(pipeline_id);
    if (found == pipelines.end()) {
        throw std::runtime_error("Fullscreen pipeline not found");
    }
    return found->second;
}

bool hasInputBinding(
    const ShaderReflection &reflection,
    uint32_t binding, bool local_read) {
    const auto expected =
        local_read
            ? vk::DescriptorType::eInputAttachment
            : vk::DescriptorType::
                  eCombinedImageSampler;
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set == PELICAN_SET_PASS_INPUT && reflected.binding == binding &&
            reflected.type == expected) {
            return true;
        }
    }
    return false;
}

bool hasStorageBufferBinding(const ShaderReflection &reflection, uint32_t binding) {
    for (const auto &reflected : reflection.bindings) {
        if (reflected.set == PELICAN_SET_PASS_INPUT && reflected.binding == binding &&
            reflected.type == vk::DescriptorType::eStorageBuffer) {
            return true;
        }
    }
    return false;
}

void requireInputBindings(
    const ShaderReflection &reflection,
    std::span<const FullscreenPassInput> inputs,
    size_t buffer_count) {
    const auto input_count = inputs.size() + buffer_count;
    for (uint32_t binding = 0; binding < input_count; ++binding) {
        if (binding < inputs.size()) {
            if (!hasInputBinding(
                    reflection, binding,
                    inputs[binding].local_read)) {
                throw std::runtime_error(
                    "Fullscreen pass input texture/local-read contract "
                    "does not match shader reflection");
            }
        } else if (!hasStorageBufferBinding(reflection, binding)) {
            throw std::runtime_error("Fullscreen pass input buffer does not match shader reflection");
        }
    }
}

vk::UniqueDescriptorPool createDescPool(vk::Device device,
                                        uint32_t maxSets = 1024) {
    std::array<vk::DescriptorPoolSize, 3> pool_sizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eCombinedImageSampler,
                               maxSets * estimatedInputBindingsPerPass},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eInputAttachment,
            maxSets * estimatedInputBindingsPerPass},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer,
                               maxSets * estimatedInputBindingsPerPass},
    };

    vk::DescriptorPoolCreateInfo ci{};
    // Passes are rebound independently while sharing this pool. Keep
    // individual-set freeing so lease-retired sets return capacity without
    // invalidating descriptor sets owned by unrelated live passes.
    ci.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    ci.maxSets = maxSets;
    ci.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(ci);
}

vk::SamplerAddressMode samplerAddressMode(
    FullscreenInputAddressMode mode) {
    switch (mode) {
    case FullscreenInputAddressMode::repeat:
        return vk::SamplerAddressMode::eRepeat;
    case FullscreenInputAddressMode::mirrored_repeat:
        return vk::SamplerAddressMode::eMirroredRepeat;
    case FullscreenInputAddressMode::clamp_to_edge:
        return vk::SamplerAddressMode::eClampToEdge;
    }
    throw std::runtime_error(
        "Unknown fullscreen sampler address mode");
}

std::size_t samplerIndex(FullscreenInputSampling sampling) {
    constexpr std::size_t address_mode_count = 3;
    return static_cast<std::size_t>(sampling.filter) *
               address_mode_count +
           static_cast<std::size_t>(sampling.address_mode);
}

vk::UniqueSampler createSampler(
    vk::Device device, FullscreenInputSampling sampling) {
    const auto filter =
        sampling.filter == FullscreenInputFilter::nearest
            ? vk::Filter::eNearest
            : vk::Filter::eLinear;
    const auto address_mode =
        samplerAddressMode(sampling.address_mode);
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = filter;
    create_info.minFilter = filter;
    create_info.mipmapMode =
        sampling.filter ==
                FullscreenInputFilter::nearest
            ? vk::SamplerMipmapMode::eNearest
            : vk::SamplerMipmapMode::eLinear;
    create_info.addressModeU = address_mode;
    create_info.addressModeV = address_mode;
    create_info.addressModeW = address_mode;
    create_info.mipLodBias = 0.0f;
    create_info.anisotropyEnable = false;
    create_info.maxAnisotropy = 1.0f;
    create_info.compareEnable = false;
    create_info.compareOp = vk::CompareOp::eAlways;
    create_info.minLod = 0.0f;
    create_info.maxLod = VK_LOD_CLAMP_NONE;
    create_info.borderColor = vk::BorderColor::eIntOpaqueBlack;
    create_info.unnormalizedCoordinates = false;
    return device.createSamplerUnique(create_info);
}

std::uint32_t inputLayerCapacity(
    const FullscreenPassInput &input,
    const RenderTargetImageViewResolver &rt_views) {
    return input.subresource
               ? input.subresource->layer_count
               : rt_views.arrayLayers(input.target);
}

// An explicit request wins. Otherwise a multiview pass follows its pipeline
// contract, and a single-view pass widens to the layer capacity of every
// input that is not shared across views.
std::uint32_t deriveLogicalViewCount(
    std::span<const FullscreenPassInput> inputs,
    const GraphicsPipelineViewContract &view,
    const RenderTargetImageViewResolver &rt_views,
    std::uint32_t requested) {
    if (requested != 0) return requested;
    auto logical_view_count =
        view.execution ==
                GraphicsPipelineViewExecution::multiview
            ? view.view_count
            : 1u;
    const auto has_per_view_input = std::any_of(
        inputs.begin(), inputs.end(),
        [](const FullscreenPassInput &input) {
            return input.view ==
                       PassInputViewDimension::
                           sequential_2d ||
                   input.view ==
                       PassInputViewDimension::
                           layered_2d_array;
        });
    if (view.execution !=
            GraphicsPipelineViewExecution::single_view ||
        !has_per_view_input) {
        return logical_view_count;
    }
    for (const auto &input : inputs) {
        if (input.view ==
                PassInputViewDimension::shared_2d ||
            input.view ==
                PassInputViewDimension::
                    family_2d_array) {
            continue;
        }
        logical_view_count = std::max(
            logical_view_count,
            inputLayerCapacity(input, rt_views));
    }
    return logical_view_count;
}

// How many descriptor sets a single-view pass needs so each invocation can
// bind its own layer. Multiview binds one set for the whole view family.
std::uint32_t deriveVariantCount(
    std::span<const FullscreenPassInput> inputs,
    const GraphicsPipelineViewContract &view,
    const RenderTargetImageViewResolver &rt_views,
    std::uint32_t logical_view_count) {
    std::uint32_t variant_count = 1;
    if (view.execution !=
        GraphicsPipelineViewExecution::single_view) {
        return variant_count;
    }
    for (const auto &input : inputs) {
        if (input.view ==
            PassInputViewDimension::
                layered_2d_array) {
            variant_count = std::max(
                variant_count, logical_view_count);
            continue;
        }
        if (input.view !=
            PassInputViewDimension::sequential_2d) {
            continue;
        }
        const auto selected_layers =
            inputLayerCapacity(input, rt_views);
        // Secondary view-family scopes are compiled as one-view
        // templates and expanded only after providers publish their
        // runtime cardinality. Preserve the authored array capacity so
        // those later invocations can still bind their own 2D layer.
        if (selected_layers > 1) {
            variant_count = std::max(
                variant_count, selected_layers);
        }
    }
    return variant_count;
}

// Per-input rules that need the derived view and variant counts.
void validateResolvedPassInputs(
    std::span<const FullscreenPassInput> inputs,
    const RenderTargetImageViewResolver &rt_views,
    std::uint32_t logical_view_count,
    std::uint32_t variant_count) {
    // Like the material path, a rejected input is named by the resource it
    // binds, not only by where it sat in the authored list.
    const auto named =
        [&rt_views](const FullscreenPassInput &input,
                    std::string message) {
            return std::runtime_error{
                std::move(message) + ": " +
                std::string{rt_views.name(input.target)}};
        };
    for (const auto &input : inputs) {
        if (input.local_read && input.history) {
            throw named(
                input,
                "Fullscreen local-read input cannot reference history");
        }
        if (input.local_read &&
            input.resolvedViewDimension() ==
                ImageSubresourceViewDimension::cube) {
            throw named(
                input,
                "Fullscreen local-read input cannot use a cube "
                "descriptor view");
        }
        const auto layers =
            rt_views.arrayLayers(input.target);
        const auto selected_layers =
            input.subresource
                ? input.subresource->layer_count
                : layers;
        if (input.view ==
            PassInputViewDimension::
                layered_2d_array) {
            if (selected_layers <
                logical_view_count) {
                throw named(
                    input,
                    "Fullscreen per_view input has too few array layers");
            }
            if (input.subresource &&
                selected_layers !=
                    logical_view_count) {
                throw named(
                    input,
                    "Fullscreen per_view subresource must select "
                    "exactly the logical view count");
            }
        } else if (
            input.view ==
                PassInputViewDimension::
                    family_2d_array) {
            if (selected_layers == 0) {
                throw named(
                    input,
                    "Fullscreen family_array input must select at least "
                    "one producer-family layer");
            }
        } else if (
            input.view ==
                PassInputViewDimension::
                    sequential_2d) {
            if (selected_layers != 1 &&
                selected_layers < variant_count) {
                throw named(
                    input,
                    "Fullscreen sequential_2d input must provide one "
                    "reusable layer or the descriptor view capacity");
            }
            if (input.subresource &&
                selected_layers != 1 &&
                selected_layers != variant_count) {
                throw named(
                    input,
                    "Fullscreen sequential_2d subresource must select one "
                    "reusable layer or exactly the descriptor view capacity");
            }
        }
        if (input.resolvedViewDimension() ==
                ImageSubresourceViewDimension::cube &&
            (rt_views.dimension(input.target) !=
                 ImageResourceDimension::cube ||
             (input.subresource &&
              (input.subresource->base_array_layer != 0 ||
               input.subresource->layer_count != 6)))) {
            throw named(
                input,
                "Fullscreen cube input requires a cube target and "
                "all six faces");
        }
        // The material path has always rejected an out-of-range
        // subresource up front; without this the range only fails much
        // later, inside view creation, with no input identity attached.
        if (input.subresource &&
            !validImageSubresourceRange(
                *input.subresource,
                rt_views.mipLevels(input.target),
                layers)) {
            throw named(
                input,
                "Fullscreen pass input " +
                    std::to_string(
                        &input - inputs.data()) +
                    " has an out-of-range subresource");
        }
    }
}

} // namespace

FullscreenPassContainer::FullscreenPassContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      desc_pool{createDescPool(device)} {
    for (const auto filter :
         {FullscreenInputFilter::linear,
          FullscreenInputFilter::nearest}) {
        for (const auto address_mode :
             {FullscreenInputAddressMode::repeat,
              FullscreenInputAddressMode::mirrored_repeat,
              FullscreenInputAddressMode::clamp_to_edge}) {
            const FullscreenInputSampling sampling{
                filter, address_mode};
            input_samplers.at(samplerIndex(sampling)) =
                createSampler(device, sampling);
        }
    }
}

FullscreenPassContainer::~FullscreenPassContainer() {}

FullscreenPassContainer::PipelineId
FullscreenPassContainer::registerFullscreenPass(vk::Format colorFormat, ShaderBundleId vertShader,
                                                ShaderBundleId fragShader,
                                                std::vector<std::string> shader_defines,
                                                vk::SampleCountFlagBits samples,
                                                GraphicsPipelineViewContract view,
                                                bool allow_ray_query) {
    return registerFullscreenPass(
        {colorFormat}, std::nullopt,
        vertShader, fragShader,
        std::move(shader_defines),
        samples, view, {}, {}, allow_ray_query);
}

FullscreenPassContainer::PipelineId
FullscreenPassContainer::registerFullscreenPass(
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
        resource_interface,
    bool allow_ray_query) {
    GraphicsPipelineDesc desc;
    desc.allow_ray_query = allow_ray_query;
    desc.vert = vert_shader;
    desc.frag = frag_shader;
    desc.color_formats =
        std::move(color_formats);
    desc.depth_format = depth_format;
    desc.shader_defines =
        std::move(shader_defines);
    desc.rasterization_samples = samples;
    desc.view = view;
    desc.local_read =
        std::move(local_read);
    desc.resource_interface =
        std::move(resource_interface);
    return registerRasterPass(std::move(desc));
}

FullscreenPassContainer::PipelineId
FullscreenPassContainer::registerRasterPass(
    GraphicsPipelineDesc desc) {
    registration_order.reserve(
        registration_order.size() + 1);
    if (next_pipeline_id >
        static_cast<uint32_t>(
            std::numeric_limits<int>::max())) {
        throw std::runtime_error(
            "Raster pipeline handle table is exhausted");
    }
    PipelineId pipeline_id = {next_pipeline_id++};

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline_handle = pipeline_factory.create(desc);
    if (!pipelines.insert({pipeline_id, pipeline_handle}).second) {
        throw std::runtime_error(
            "Raster pipeline handle table changed during registration");
    }
    registration_order.push_back(pipeline_id);

    return pipeline_id;
}

void FullscreenPassContainer::setIndexBuffer(PassId pass_id, FrameGraphBufferId buffer) {
    input_textures.at(pass_id.value).index_buffer = buffer;
}

void FullscreenPassContainer::bindResource(
    vk::CommandBuffer cmd_buf, PassId pass_id,
    RenderPassViewInvocation invocation) {
    const auto pipeline_handle = requirePipelineHandle(pass_id, pipelines);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);

    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(pipeline_handle));

    auto it = input_textures.find(pass_id.value);
    if (it != input_textures.end()) {
        const auto parity = GET_MODULE(RenderTargetContainer).historyFrameIndex();
        const auto &info = it->second;
        if (info.index_buffer)
            cmd_buf.bindIndexBuffer(GET_MODULE(FrameGraphResourceContainer).buffer(*info.index_buffer).buffer.get(),
                0, vk::IndexType::eUint32);
        std::size_t variant_index = 0;
        if (info.view.execution ==
            GraphicsPipelineViewExecution::multiview) {
            if (invocation.view_index != 0 ||
                invocation.logical_view_count !=
                    info.view.view_count) {
                throw std::runtime_error(
                    "fullscreen multiview descriptor invocation does not "
                    "match its pipeline");
            }
        } else if (info.variants.size() > 1) {
            if (invocation.logical_view_count >
                    info.variants.size() ||
                invocation.view_index >=
                    invocation.logical_view_count) {
                throw std::runtime_error(
                    "fullscreen sequential descriptor invocation does not "
                    "fit its layered-input capacity");
            }
            variant_index = invocation.view_index;
        }
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_factory.layout(pipeline_handle),
                                   PELICAN_SET_PASS_INPUT,
                                   info.variants.at(variant_index)
                                       .descsets[parity]
                                       .get(),
                                   {});
    }
}

void FullscreenPassContainer::setInputTextures(PassId pass_id, const std::vector<GlobalRenderTargetId> &input_rts,
                                               const RenderTargetImageViewResolver &rt_views) {
    std::vector<FullscreenPassInput> inputs;
    inputs.reserve(input_rts.size());
    for (const auto target : input_rts) {
        inputs.push_back(
            FullscreenPassInput{.target = target});
    }
    setInputResources(
        pass_id, inputs, {}, rt_views,
        GET_MODULE(FrameGraphResourceContainer));
}

void FullscreenPassContainer::setInputResources(
    PassId pass_id,
    const std::vector<FullscreenPassInput> &inputs,
    const std::vector<std::string> &input_buffers,
    const RenderTargetImageViewResolver &rt_views,
    const FrameGraphResourceContainer &frame_graph_resources,
    GraphicsPipelineViewContract view,
    std::uint32_t logical_view_count) {
    std::vector<FrameGraphBufferId> buffer_ids;
    buffer_ids.reserve(input_buffers.size());
    for (const auto &name : input_buffers) {
        const auto id =
            frame_graph_resources.getBufferIdByName(name);
        if (!isValidFrameGraphBufferId(id)) {
            throw std::runtime_error(
                "Fullscreen pass input buffer not found: " +
                name);
        }
        buffer_ids.push_back(id);
    }
    setInputResourcesById(
        pass_id, inputs, buffer_ids, rt_views,
        frame_graph_resources, view,
        logical_view_count);
}

void FullscreenPassContainer::setInputResourcesById(
    PassId pass_id,
    const std::vector<FullscreenPassInput> &inputs,
    const std::vector<FrameGraphBufferId> &input_buffers,
    const RenderTargetImageViewResolver &rt_views,
    const FrameGraphResourceContainer &frame_graph_resources,
    GraphicsPipelineViewContract view,
    std::uint32_t logical_view_count) {
    const auto pipeline_handle = requirePipelineHandle(pass_id, pipelines);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    requireInputBindings(
        pipeline_factory.reflection(
            pipeline_handle),
        inputs, input_buffers.size());
    if (view.execution ==
            GraphicsPipelineViewExecution::multiview &&
        std::any_of(
            inputs.begin(), inputs.end(),
            [](const FullscreenPassInput &input) {
                return input.view ==
                       PassInputViewDimension::
                           sequential_2d;
            })) {
        throw std::runtime_error(
            "Fullscreen multiview pass cannot bind a sequential-only input");
    }

    const auto input_count = inputs.size() + input_buffers.size();
    if (input_count == 0) {
        const auto existing =
            input_textures.find(pass_id.value);
        if (existing != input_textures.end()) {
            deferRetiredDescriptorResources(
                GET_MODULE(DeletionQueue),
                std::move(existing->second));
            input_textures.erase(existing);
        }
        return;
    }

    vk::DescriptorSetLayout layout = pipeline_factory.descriptorSetLayout(pipeline_handle, PELICAN_SET_PASS_INPUT);

    InputTextureInfo info;
    info.inputs = inputs;
    info.input_buffer_ids = input_buffers;
    info.view = view;
    logical_view_count = deriveLogicalViewCount(
        info.inputs, view, rt_views,
        logical_view_count);
    if (logical_view_count == 0 ||
        (view.execution ==
             GraphicsPipelineViewExecution::multiview &&
         logical_view_count != view.view_count)) {
        throw std::runtime_error(
            "Fullscreen pass logical view count is inconsistent");
    }
    info.logical_view_count = logical_view_count;
    info.binding_revision = next_binding_revision++;
    if (std::any_of(
            inputs.begin(), inputs.end(),
            [](const FullscreenPassInput &input) {
                return !isConcreteRenderTarget(
                    input.target);
            })) {
        throw std::runtime_error(
            "Fullscreen pass input texture must be a render target");
    }
    const auto variant_count = deriveVariantCount(
        info.inputs, view, rt_views,
        logical_view_count);
    validateResolvedPassInputs(
        info.inputs, rt_views, logical_view_count,
        variant_count);
    buildInputDescriptorVariants(
        info, layout, rt_views,
        frame_graph_resources, variant_count);
    const auto existing =
        input_textures.find(pass_id.value);
    if (existing == input_textures.end()) {
        input_textures.emplace(
            pass_id.value, std::move(info));
        return;
    }

    deferRetiredDescriptorResources(
        GET_MODULE(DeletionQueue),
        std::move(existing->second));
    existing->second = std::move(info);
}

void FullscreenPassContainer::buildInputDescriptorVariants(
    InputTextureInfo &info,
    vk::DescriptorSetLayout layout,
    const RenderTargetImageViewResolver &rt_views,
    const FrameGraphResourceContainer
        &frame_graph_resources,
    std::uint32_t variant_count) const {
    const auto &inputs = info.inputs;
    const auto &input_buffers = info.input_buffer_ids;
    const auto &view = info.view;
    const auto logical_view_count =
        view.execution ==
                GraphicsPipelineViewExecution::multiview
            ? std::optional{info.logical_view_count}
            : std::nullopt;

    // Resolved once per input instead of once per (variant, parity,
    // input): the descriptor type and sampler are properties of the
    // input, as they already are on the material path.
    struct ResolvedFullscreenInput {
        ResolvedPassInput view_request;
        vk::DescriptorType descriptor_type =
            vk::DescriptorType::eCombinedImageSampler;
        vk::Sampler sampler;
    };
    std::vector<ResolvedFullscreenInput> resolved;
    resolved.reserve(inputs.size());
    for (const auto &input : inputs) {
        if (!isConcreteRenderTarget(input.target)) {
            throw std::runtime_error("Fullscreen pass input texture must be a render target");
        }
        const auto descriptor_dimension =
            input.resolvedViewDimension();
        const auto selected_layers =
            input.subresource
                ? input.subresource->layer_count
                : rt_views.arrayLayers(input.target);
        // Fullscreen keeps its fallback: a sequential input that cannot
        // cover the variant count reuses one layer for every variant.
        const auto separate_view_layers =
            input.view ==
                PassInputViewDimension::
                    layered_2d_array ||
            (input.view ==
                 PassInputViewDimension::sequential_2d &&
             selected_layers >= variant_count &&
             variant_count > 1);
        const auto layer_binding =
            input.view ==
                    PassInputViewDimension::
                        family_2d_array
                ? PassInputLayerBinding::whole_array
            : descriptor_dimension !=
                        ImageSubresourceViewDimension::
                            cube &&
                    view.execution ==
                        GraphicsPipelineViewExecution::
                            single_view &&
                    separate_view_layers
                ? PassInputLayerBinding::per_variant
                : PassInputLayerBinding::shared;
        resolved.push_back(ResolvedFullscreenInput{
            .view_request =
                ResolvedPassInput{
                    .target = input.target,
                    .history = input.history,
                    .view = input.view,
                    .descriptor_dimension =
                        descriptor_dimension,
                    .subresource = input.subresource,
                    .layer_binding = layer_binding,
                    .logical_view_count =
                        logical_view_count,
                },
            .descriptor_type =
                input.local_read
                    ? vk::DescriptorType::
                          eInputAttachment
                    : vk::DescriptorType::
                          eCombinedImageSampler,
            .sampler =
                input.local_read
                    ? vk::Sampler{}
                    : input_samplers
                          .at(samplerIndex(
                              input.sampling))
                          .get(),
        });
    }

    info.variants.resize(variant_count);
    for (std::uint32_t variant = 0;
         variant < variant_count; ++variant) {
        for (uint32_t parity = 0; parity < 2; ++parity) {
            auto &descriptor_variant =
                info.variants[variant];
            vk::DescriptorSetAllocateInfo alloc_info;
            alloc_info.descriptorPool = desc_pool.get();
            alloc_info.descriptorSetCount = 1;
            alloc_info.pSetLayouts = &layout;
            descriptor_variant.descsets[parity] =
                std::move(device.allocateDescriptorSetsUnique(alloc_info).front());

            PassInputDescriptorWrites writes{
                descriptor_variant, parity,
                inputs.size(), input_buffers.size()};
            for (uint32_t i = 0; i < resolved.size(); ++i) {
                auto request =
                    resolved[i].view_request;
                request.variant = variant;
                request.parity = parity;
                writes.appendImage(
                    i, resolved[i].descriptor_type,
                    resolved[i].sampler,
                    resolvePassInputImageView(
                        request, rt_views));
            }
            for (uint32_t i = 0; i < input_buffers.size(); ++i) {
                const auto binding = static_cast<uint32_t>(inputs.size()) + i;
                writes.appendBuffer(
                    binding,
                    frame_graph_resources.descriptorInfo(
                        input_buffers[i]));
            }
            writes.submit(device);
        }
    }
}

void FullscreenPassContainer::rebindInputResources(
    PassId pass_id,
    const RenderTargetImageViewResolver &rt_views,
    const FrameGraphResourceContainer &frame_graph_resources) {
    const auto found = input_textures.find(pass_id.value);
    if (found == input_textures.end()) return;
    const auto inputs = found->second.inputs;
    const auto input_buffers =
        found->second.input_buffer_ids;
    const auto index_buffer = found->second.index_buffer;
    const auto logical_view_count =
        found->second.logical_view_count;
    const auto view = found->second.view;
    setInputResourcesById(
        pass_id, inputs, input_buffers, rt_views,
        frame_graph_resources, view,
        logical_view_count);
    if (index_buffer) setIndexBuffer(pass_id, *index_buffer);
}

std::vector<vk::ImageView>
FullscreenPassContainer::boundInputImageViewsForTesting(
    PassId pass_id,
    std::uint32_t view_index) const {
    const auto found = input_textures.find(pass_id.value);
    if (found == input_textures.end()) {
        return {};
    }
    return found->second.variants.at(view_index).bound_image_views[
        GET_MODULE(RenderTargetContainer).historyFrameIndex()];
}

uint64_t FullscreenPassContainer::inputBindingRevisionForTesting(PassId pass_id) const {
    const auto found = input_textures.find(pass_id.value);
    return found == input_textures.end() ? 0 : found->second.binding_revision;
}

vk::PipelineLayout FullscreenPassContainer::getPipelineLayout(PassId pass_id) const {
    return GET_MODULE(PipelineFactory).layout(requirePipelineHandle(pass_id, pipelines));
}

std::vector<FullscreenInputSampling>
FullscreenPassContainer::inputSamplingForTesting(
    PassId pass_id) const {
    const auto found = input_textures.find(pass_id.value);
    if (found == input_textures.end()) return {};
    std::vector<FullscreenInputSampling> sampling;
    sampling.reserve(found->second.inputs.size());
    for (const auto &input : found->second.inputs) {
        sampling.push_back(input.sampling);
    }
    return sampling;
}

ShaderBundleId
FullscreenPassContainer::fragmentShaderForTesting(
    PassId pass_id) const {
    const auto desc = GET_MODULE(PipelineFactory).graphicsDesc(
        requirePipelineHandle(pass_id, pipelines));
    if (!desc.frag) {
        throw std::runtime_error(
            "Fullscreen pipeline has no fragment shader");
    }
    return *desc.frag;
}

GraphicsPipelineDesc
FullscreenPassContainer::graphicsPipelineDescForTesting(
    PassId pass_id) const {
    return GET_MODULE(PipelineFactory).graphicsDesc(
        requirePipelineHandle(pass_id, pipelines));
}

std::vector<ShaderResourceInterfaceBinding>
FullscreenPassContainer::resourceInterfaceForTesting(
    PassId pass_id) const {
    return GET_MODULE(PipelineFactory)
        .graphicsDesc(
            requirePipelineHandle(pass_id, pipelines))
        .resource_interface;
}

std::vector<bool>
FullscreenPassContainer::inputLocalReadsForTesting(
    PassId pass_id) const {
    const auto found =
        input_textures.find(pass_id.value);
    if (found == input_textures.end()) return {};
    std::vector<bool> local_reads;
    local_reads.reserve(found->second.inputs.size());
    for (const auto &input : found->second.inputs) {
        local_reads.push_back(input.local_read);
    }
    return local_reads;
}

FullscreenPassContainer::RegistrationCheckpoint
FullscreenPassContainer::checkpointRegistrations() const noexcept {
    return RegistrationCheckpoint{
        registration_order.size(), next_binding_revision,
        next_pipeline_id};
}

void FullscreenPassContainer::rollbackRegistrations(
    RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Fullscreen pass registration checkpoint is invalid");
    }
    while (registration_order.size() >
           checkpoint.registration_count) {
        const auto id = registration_order.back();
        input_textures.erase(static_cast<int>(id.value));
        pipelines.erase(id);
        registration_order.pop_back();
    }
    next_binding_revision =
        checkpoint.next_binding_revision;
    next_pipeline_id = checkpoint.next_pipeline_id;
}

std::vector<FullscreenPassContainer::PipelineId>
FullscreenPassContainer::registrationsSince(
    RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count >
        registration_order.size()) {
        throw std::runtime_error(
            "Fullscreen pass registration checkpoint is invalid");
    }
    return {
        registration_order.begin() +
            static_cast<std::ptrdiff_t>(
                checkpoint.registration_count),
        registration_order.end()};
}

void FullscreenPassContainer::retireRegistrations(
    const std::vector<PipelineId> &ids) noexcept {
    for (const auto id : ids) {
        const auto textures =
            input_textures.find(static_cast<int>(id.value));
        if (textures != input_textures.end()) {
            auto retired = std::move(textures->second);
            input_textures.erase(textures);
            try {
                auto *queue =
                    FastModuleContainer::tryGet<DeletionQueue>();
                if (queue != nullptr &&
                    queue->acceptingResources()) {
                    deferRetiredDescriptorResources(
                        *queue, std::move(retired));
                }
            } catch (...) {
            }
        }
        pipelines.erase(id);
        std::erase(registration_order, id);
    }
}

} // namespace Pelican
