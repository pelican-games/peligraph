#include "materialcontainer.hpp"
#include "../loader/imageloader.hpp"
#include "../renderingpass/computetask.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/materialpassattachments.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../shader/pelican_sets.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../shader/surfacecompiler.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../vkcore/rendertarget.hpp"
#include "../vkcore/util.hpp"
#include "../watch/reloadservice.hpp"
#include "standardmaterialresource.hpp"
#include "materialvaluesreloadhandler.hpp"
#include "texturereloadhandler.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Pelican {


constexpr uint32_t imageDescriptorSetNumber = PELICAN_SET_MATERIAL;
constexpr uint32_t baseColorBinding = 0;
constexpr uint32_t metallicRoughnessBinding = 1;
constexpr uint32_t normalBinding = 2;
constexpr uint32_t emissiveBinding = 3;
constexpr uint32_t vatPositionBinding = 4;
constexpr uint32_t vatNormalBinding = 5;
constexpr uint32_t materialBufferBinding = PELICAN_MATERIAL_BUFFER_BINDING;
constexpr uint32_t baseMaterialTextureBindingCount = 4;
constexpr uint32_t vatMaterialTextureBindingCount = 6;
constexpr size_t maxMaterials = 1024;
constexpr uint32_t maxMaterialPassInputs = 8;
constexpr uint32_t maxMaterialPassDescriptors = 32;

static bool supportsDepthComparisonSampling(
    vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return true;
    default:
        return false;
    }
}

static bool isIntegerColorFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eX8D24UnormPack32:
    case vk::Format::eD32Sfloat:
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        return false;
    default:
        break;
    }
    const auto name = vk::to_string(format);
    return name.find("Uint") != std::string::npos ||
           name.find("Sint") != std::string::npos;
}

static void validateMaterialInputAttachmentFormat(
    std::string_view input_name,
    GlobalRenderTargetId target) {
    const auto format =
        GET_MODULE(RenderTargetContainer)
            .getMetadata(target)
            .format;
    if (isIntegerColorFormat(format)) {
        throw std::runtime_error(
            "material input '" + std::string{input_name} +
            "' cannot use integer input attachment format " +
            vk::to_string(format) +
            "; the generated material image accessor currently "
            "returns vec4");
    }
}

struct MaterialPipelineRenderingContract {
    std::vector<vk::Format> color_formats;
    std::optional<vk::Format> depth_format;
    vk::SampleCountFlagBits rasterization_samples =
        vk::SampleCountFlagBits::e1;
    GraphicsPipelineRenderingLocalReadContract local_read;
    std::vector<MaterialOutputAttachmentState>
        output_states;

    bool operator==(
        const MaterialPipelineRenderingContract &) const =
        default;
};

static MaterialPipelineRenderingContract
defaultMaterialPipelineRenderingContract(
    MaterialShaderContract shader_contract,
    vk::SampleCountFlagBits samples) {
    MaterialPipelineRenderingContract result;
    if (shader_contract ==
        MaterialShaderContract::forward_scene_color_v1) {
        result.color_formats.push_back(
            forwardMaterialPassColorAttachmentFormat);
    } else {
        const auto &formats =
            materialPassColorAttachmentFormats(
                GET_MODULE(RenderingPassContainer)
                    .isFeatureEnabled("hdr"));
        result.color_formats.assign(
            formats.begin(), formats.end());
    }
    result.depth_format =
        materialPassDepthAttachmentFormat;
    result.rasterization_samples = samples;
    return result;
}

static vk::Format resolveMaterialColorFormat(
    GlobalRenderTargetId target) {
    if (isConcreteRenderTarget(target)) {
        return GET_MODULE(RenderTargetContainer)
            .getMetadata(target)
            .format;
    }
    if (isSwapchainRenderTarget(target)) {
        return GET_MODULE(RenderTarget)
            .getSwapchainFormat();
    }
    throw std::runtime_error(
        "material physical rendering contract has an invalid "
        "color target");
}

static MaterialPipelineRenderingContract
resolveMaterialPassRenderingBinding(
    const MaterialPassRenderingBinding &binding,
    MaterialShaderContract shader_contract) {
    if (binding.rendering.color_attachments.empty()) {
        auto result =
            defaultMaterialPipelineRenderingContract(
                shader_contract,
                binding.rasterization_samples);
        result.output_states = binding.output_states;
        return result;
    }

    MaterialPipelineRenderingContract result;
    result.rasterization_samples =
        binding.rasterization_samples;
    result.output_states = binding.output_states;
    result.color_formats.reserve(
        binding.rendering.color_attachments.size());
    for (const auto target :
         binding.rendering.color_attachments) {
        result.color_formats.push_back(
            resolveMaterialColorFormat(target));
    }
    if (isConcreteRenderTarget(
            binding.rendering.depth_attachment)) {
        result.depth_format =
            GET_MODULE(RenderTargetContainer)
                .getMetadata(
                    binding.rendering.depth_attachment)
                .format;
    } else if (isSwapchainRenderTarget(
                   binding.rendering.depth_attachment)) {
        throw std::runtime_error(
            "material physical rendering contract cannot use the "
            "swapchain as a depth attachment: " +
            binding.pass_name);
    }

    result.local_read.enabled =
        binding.rendering.local_read_scope;
    if (!result.local_read.enabled) {
        return result;
    }
    result.local_read.color_attachment_locations =
        binding.rendering.color_attachment_locations;
    result.local_read.color_attachment_input_indices =
        binding.rendering
            .color_attachment_input_indices;
    result.local_read.depth_attachment_input_index =
        binding.rendering
            .depth_attachment_input_index;
    return result;
}

static MaterialPipelineRenderingContract
resolveMaterialPipelineRenderingContract(
    const MaterialInfo &info) {
    const auto &rendering_passes =
        GET_MODULE(RenderingPassContainer);
    const auto bindings =
        rendering_passes
            .materialPassRenderingBindings(
                info.route, info.shader_contract,
                info.exact_pass);
    if (bindings.empty()) {
        if (rendering_passes.hasMaterialPasses()) {
            const auto selected =
                info.exact_pass
                    ? " exact pass '" +
                          *info.exact_pass + "'"
                    : std::string{};
            throw std::runtime_error(
                "material route '" +
                std::string{
                    materialRouteClassName(
                        info.route)} +
                "' with shader contract '" +
                std::string{
                    materialShaderContractName(
                        info.shader_contract)} +
                "' has no compatible registered material pass" +
                selected);
        }
        return defaultMaterialPipelineRenderingContract(
            info.shader_contract,
            rendering_passes
                .materialRasterizationSamples(
                    info.shader_contract));
    }

    for (const auto &binding : bindings) {
        if (binding.output_schema !=
            info.output_schema) {
            const auto shader_schema =
                info.output_schema
                    ? "'" + info.output_schema->name + "'"
                    : std::string{"<built-in>"};
            const auto pass_schema =
                binding.output_schema
                    ? "'" + binding.output_schema->name + "'"
                    : std::string{"<built-in>"};
            throw std::runtime_error(
                "material fragment-output schema " +
                shader_schema +
                " is incompatible with pass '" +
                binding.pass_name + "' schema " +
                pass_schema);
        }
    }

    auto result =
        resolveMaterialPassRenderingBinding(
            bindings.front(),
            info.shader_contract);
    for (std::size_t index = 1;
         index < bindings.size(); ++index) {
        const auto candidate =
            resolveMaterialPassRenderingBinding(
                bindings[index],
                info.shader_contract);
        if (candidate != result) {
            throw std::runtime_error(
                "material route resolves to pipeline-incompatible "
                "physical rendering contracts: " +
                bindings.front().pass_name + " and " +
                bindings[index].pass_name);
        }
    }
    return result;
}

static std::string makePipelineKey(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract
        &rendering) {
    std::ostringstream key;
    key << info.vert_shader.value << ':' << info.frag_shader.value << ':'
        << info.skinned << ':'
        << static_cast<int>(info.shader_contract) << ':'
        << static_cast<int>(info.render_state.blend) << ':'
        << static_cast<int>(info.render_state.cull) << ':'
        << info.render_state.depth_test << ':' << info.render_state.depth_write << ':'
        << static_cast<int>(info.render_state.depth_compare)
        << ":samples="
        << static_cast<std::uint32_t>(
               rendering.rasterization_samples)
        << ":outputs=";
    if (info.output_schema) {
        key << materialOutputSchemaFingerprint(
            *info.output_schema);
    } else {
        key << "built-in";
    }
    key << ":colors=";
    for (const auto format :
         rendering.color_formats) {
        key << static_cast<std::uint32_t>(format)
            << ',';
    }
    key << ":depth=";
    if (rendering.depth_format) {
        key << static_cast<std::uint32_t>(
            *rendering.depth_format);
    } else {
        key << "none";
    }
    key << ":local="
        << rendering.local_read.enabled
        << ":locations=";
    for (const auto location :
         rendering.local_read
             .color_attachment_locations) {
        key << location << ',';
    }
    key << ":inputs=";
    for (const auto input :
         rendering.local_read
             .color_attachment_input_indices) {
        key << input << ',';
    }
    key << ":depth_input="
        << rendering.local_read
               .depth_attachment_input_index;
    key << ":attachment_states=";
    if (!rendering.output_states.empty()) {
        if (!info.output_schema) {
            throw std::logic_error(
                "material output attachment states require an "
                "output schema");
        }
        key << materialOutputAttachmentStatesFingerprint(
            rendering.output_states,
            *info.output_schema);
    } else {
        key << "inherit";
    }
    return key.str();
}

static vk::CompareOp toVkCompare(SurfaceDepthCompare compare) {
    switch (compare) {
    case SurfaceDepthCompare::never: return vk::CompareOp::eNever;
    case SurfaceDepthCompare::less: return vk::CompareOp::eLess;
    case SurfaceDepthCompare::equal: return vk::CompareOp::eEqual;
    case SurfaceDepthCompare::less_equal: return vk::CompareOp::eLessOrEqual;
    case SurfaceDepthCompare::greater: return vk::CompareOp::eGreater;
    case SurfaceDepthCompare::not_equal: return vk::CompareOp::eNotEqual;
    case SurfaceDepthCompare::greater_equal: return vk::CompareOp::eGreaterOrEqual;
    case SurfaceDepthCompare::always: return vk::CompareOp::eAlways;
    }
    throw std::runtime_error("unknown material depth compare state");
}

static vk::CullModeFlags toVkCull(SurfaceCullMode cull) {
    switch (cull) {
    case SurfaceCullMode::none: return vk::CullModeFlagBits::eNone;
    case SurfaceCullMode::front: return vk::CullModeFlagBits::eFront;
    case SurfaceCullMode::back: return vk::CullModeFlagBits::eBack;
    }
    throw std::runtime_error("unknown material cull state");
}

static vk::ShaderStageFlags materialResourceStages(
    SurfaceResourcePortStage stage) {
    switch (stage) {
    case SurfaceResourcePortStage::vertex:
        return vk::ShaderStageFlagBits::eVertex;
    case SurfaceResourcePortStage::fragment:
        return vk::ShaderStageFlagBits::eFragment;
    case SurfaceResourcePortStage::vertex_fragment:
        return vk::ShaderStageFlagBits::eVertex |
               vk::ShaderStageFlagBits::eFragment;
    }
    throw std::runtime_error(
        "unknown material resource port stage");
}

static std::vector<ShaderResourceInterfaceBinding>
resolveMaterialResourceInterface(
    const MaterialInfo &info) {
    const auto &shader_library =
        GET_MODULE(ShaderLibrary);
    const std::array reflections{
        shader_library.get(info.vert_shader).reflection,
        shader_library.get(info.frag_shader).reflection,
    };
    const auto reflection = merge(reflections);
    const auto &compiler_resources =
        shader_library.get(
            info.frag_shader)
            .compiler_resource_interface;

    std::vector<ShaderResourceInterfaceBinding>
        result;
    result.reserve(
        info.resource_ports.size() +
        compiler_resources.size());
    std::unordered_set<std::string> declared_names;
    for (const auto &port : info.resource_ports) {
        if (!declared_names.insert(port.name).second) {
            throw std::runtime_error(
                "material resource port is declared more than once: " +
                port.name);
        }
        const auto descriptor_name =
            shaderResourcePortVariableName(port.name);
        const auto found = std::find_if(
            reflection.bindings.begin(),
            reflection.bindings.end(),
            [&](const auto &binding) {
                return binding.set ==
                           PELICAN_SET_PASS_INPUT &&
                       binding.name == descriptor_name;
            });
        if (found == reflection.bindings.end()) {
            throw std::runtime_error(
                "material resource port '" +
                port.name +
                "' is absent from shader reflection");
        }
        const auto image =
            port.kind ==
            SurfaceResourcePortKind::image;
        const auto input_attachment =
            image &&
            found->type ==
                vk::DescriptorType::
                    eInputAttachment;
        if (image &&
            found->type !=
                vk::DescriptorType::
                    eCombinedImageSampler &&
            !input_attachment) {
            throw std::runtime_error(
                "material resource port '" +
                port.name +
                "' reflects an unsupported image descriptor type " +
                vk::to_string(found->type));
        }
        if (input_attachment &&
            port.stage !=
                SurfaceResourcePortStage::fragment) {
            throw std::runtime_error(
                "material resource port '" +
                port.name +
                "' input attachment must be fragment-only");
        }
        result.push_back(
            ShaderResourceInterfaceBinding{
                .port =
                    ShaderResourcePortDefinition{
                        .name = port.name,
                        .resource = port.name,
                        .kind =
                            image
                                ? ShaderResourcePortKind::
                                      image
                                : ShaderResourcePortKind::
                                      buffer,
                        .buffer_element =
                            image
                                ? std::nullopt
                                : std::optional{
                                      port.element},
                        .access =
                            image
                                ? ShaderResourcePortAccess::
                                      sampled
                                : ShaderResourcePortAccess::
                                      storage,
                        .view =
                            image &&
                                    !input_attachment &&
                                    found
                                            ->image_view_dimension ==
                                        ReflectedImageViewDimension::
                                            cube
                                ? ShaderResourcePortView::
                                      cube
                            : image &&
                                    !input_attachment &&
                                    found
                                            ->image_view_dimension ==
                                        ReflectedImageViewDimension::
                                            two_d_array
                                ? ShaderResourcePortView::
                                      family_array
                                : ShaderResourcePortView::
                                      shared_2d,
                    },
                .binding = found->binding,
                .descriptor =
                    image
                        ? input_attachment
                              ? ShaderResourceDescriptorKind::
                                    input_attachment
                              : ShaderResourceDescriptorKind::
                                    combined_image_sampler
                        : ShaderResourceDescriptorKind::
                              storage_buffer,
                .image_view_dimension =
                    image
                                ? input_attachment
                                      ? ReflectedImageViewDimension::
                                            two_d
                                      : found
                                            ->image_view_dimension
                                : ReflectedImageViewDimension::
                                      none,
                .input_attachment_index =
                    found->input_attachment_index,
                .buffer_element = port.element,
                .expected_stages =
                    materialResourceStages(port.stage),
                .readable = true,
                .writable = false,
            });
    }

    for (const auto &binding :
         compiler_resources) {
        if (!declared_names.insert(
                binding.port.name)
                 .second) {
            throw std::runtime_error(
                "compiler-owned material resource port collides with "
                "an authored port: " +
                binding.port.name);
        }
        result.push_back(binding);
    }

    for (const auto &binding : reflection.bindings) {
        constexpr std::string_view prefix =
            "pelican_resource_";
        if (binding.set != PELICAN_SET_PASS_INPUT ||
            !binding.name.starts_with(prefix)) {
            continue;
        }
        const auto declared = std::any_of(
            result.begin(), result.end(),
            [&](const auto &candidate) {
                return candidate.binding ==
                           binding.binding &&
                       shaderResourcePortVariableName(
                           candidate.port.name) ==
                           binding.name;
            });
        if (!declared) {
            throw std::runtime_error(
                "material shader reflects undeclared resource port '" +
                binding.name + "'");
        }
    }
    validateShaderResourceInterfaceReflection(
        result, reflection,
        PELICAN_SET_PASS_INPUT);
    return result;
}

static vk::BlendFactor toVkBlendFactor(
    MaterialOutputBlendFactor factor) {
    switch (factor) {
    case MaterialOutputBlendFactor::zero:
        return vk::BlendFactor::eZero;
    case MaterialOutputBlendFactor::one:
        return vk::BlendFactor::eOne;
    case MaterialOutputBlendFactor::source_color:
        return vk::BlendFactor::eSrcColor;
    case MaterialOutputBlendFactor::one_minus_source_color:
        return vk::BlendFactor::eOneMinusSrcColor;
    case MaterialOutputBlendFactor::destination_color:
        return vk::BlendFactor::eDstColor;
    case MaterialOutputBlendFactor::
        one_minus_destination_color:
        return vk::BlendFactor::eOneMinusDstColor;
    case MaterialOutputBlendFactor::source_alpha:
        return vk::BlendFactor::eSrcAlpha;
    case MaterialOutputBlendFactor::one_minus_source_alpha:
        return vk::BlendFactor::eOneMinusSrcAlpha;
    case MaterialOutputBlendFactor::destination_alpha:
        return vk::BlendFactor::eDstAlpha;
    case MaterialOutputBlendFactor::
        one_minus_destination_alpha:
        return vk::BlendFactor::eOneMinusDstAlpha;
    case MaterialOutputBlendFactor::source_alpha_saturate:
        return vk::BlendFactor::eSrcAlphaSaturate;
    }
    throw std::runtime_error(
        "unknown material output blend factor");
}

static vk::BlendOp toVkBlendOperation(
    MaterialOutputBlendOperation operation) {
    switch (operation) {
    case MaterialOutputBlendOperation::add:
        return vk::BlendOp::eAdd;
    case MaterialOutputBlendOperation::subtract:
        return vk::BlendOp::eSubtract;
    case MaterialOutputBlendOperation::reverse_subtract:
        return vk::BlendOp::eReverseSubtract;
    case MaterialOutputBlendOperation::minimum:
        return vk::BlendOp::eMin;
    case MaterialOutputBlendOperation::maximum:
        return vk::BlendOp::eMax;
    }
    throw std::runtime_error(
        "unknown material output blend operation");
}

static vk::ColorComponentFlags toVkWriteMask(
    std::uint8_t mask) {
    vk::ColorComponentFlags result;
    if ((mask & materialOutputWriteRed) != 0)
        result |= vk::ColorComponentFlagBits::eR;
    if ((mask & materialOutputWriteGreen) != 0)
        result |= vk::ColorComponentFlagBits::eG;
    if ((mask & materialOutputWriteBlue) != 0)
        result |= vk::ColorComponentFlagBits::eB;
    if ((mask & materialOutputWriteAlpha) != 0)
        result |= vk::ColorComponentFlagBits::eA;
    return result;
}

static GraphicsPipelineColorAttachmentState
defaultMaterialColorAttachmentState(
    SurfaceBlendMode blend) {
    GraphicsPipelineColorAttachmentState result;
    if (blend == SurfaceBlendMode::blend) {
        result.blend_enabled = true;
        result.source_color =
            vk::BlendFactor::eSrcAlpha;
        result.destination_color =
            vk::BlendFactor::eOneMinusSrcAlpha;
        result.source_alpha =
            vk::BlendFactor::eOne;
        result.destination_alpha =
            vk::BlendFactor::eOneMinusSrcAlpha;
    } else if (blend ==
               SurfaceBlendMode::additive) {
        result.blend_enabled = true;
        result.source_color =
            vk::BlendFactor::eSrcAlpha;
        result.destination_color =
            vk::BlendFactor::eOne;
        result.source_alpha =
            vk::BlendFactor::eOne;
        result.destination_alpha =
            vk::BlendFactor::eOne;
    }
    return result;
}

static std::vector<GraphicsPipelineColorAttachmentState>
resolveMaterialColorAttachmentStates(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract &rendering) {
    std::vector<GraphicsPipelineColorAttachmentState>
        result(
            rendering.color_formats.size(),
            defaultMaterialColorAttachmentState(
                info.render_state.blend));
    if (rendering.output_states.empty()) {
        return result;
    }
    if (!info.output_schema) {
        throw std::logic_error(
            "material output attachment states require an "
            "output schema");
    }
    if (info.output_schema->outputs.size() !=
        result.size()) {
        throw std::runtime_error(
            "material output attachment state count is "
            "incompatible with the physical color attachments");
    }
    validateMaterialOutputAttachmentStates(
        rendering.output_states,
        *info.output_schema,
        "material pipeline output states");
    for (const auto &override :
         rendering.output_states) {
        const auto output = std::find_if(
            info.output_schema->outputs.begin(),
            info.output_schema->outputs.end(),
            [&](const auto &field) {
                return field.name == override.output;
            });
        if (output ==
            info.output_schema->outputs.end()) {
            throw std::logic_error(
                "validated material output state is absent "
                "from its schema");
        }
        const auto location =
            static_cast<std::size_t>(
                std::distance(
                    info.output_schema->outputs.begin(),
                    output));
        auto &state = result[location];
        if (override.blend) {
            state.blend_enabled =
                override.blend->enabled;
            state.source_color =
                toVkBlendFactor(
                    override.blend->color.source);
            state.destination_color =
                toVkBlendFactor(
                    override.blend->color.destination);
            state.color_operation =
                toVkBlendOperation(
                    override.blend->color.operation);
            state.source_alpha =
                toVkBlendFactor(
                    override.blend->alpha.source);
            state.destination_alpha =
                toVkBlendFactor(
                    override.blend->alpha.destination);
            state.alpha_operation =
                toVkBlendOperation(
                    override.blend->alpha.operation);
        }
        if (override.write_mask) {
            state.write_mask =
                toVkWriteMask(
                    *override.write_mask);
        }
    }
    return result;
}

static GraphicsPipelineDesc makeMaterialPipelineDesc(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract
        &rendering,
    std::vector<ShaderResourceInterfaceBinding>
        resource_interface) {
    GraphicsPipelineDesc desc;
    desc.vert = info.vert_shader;
    desc.frag = info.frag_shader;
    desc.color_formats = rendering.color_formats;
    desc.depth_format = rendering.depth_format;
    desc.use_engine_vertex_layout = !info.skinned;
    desc.use_skinned_vertex_layout = info.skinned;
    desc.depth_test = info.render_state.depth_test;
    desc.depth_write = info.render_state.depth_write;
    desc.depth_compare = toVkCompare(info.render_state.depth_compare);
    desc.cull_mode = toVkCull(info.render_state.cull);
    desc.front_face = vk::FrontFace::eClockwise;
    desc.rasterization_samples =
        rendering.rasterization_samples;
    desc.local_read = rendering.local_read;
    desc.resource_interface =
        std::move(resource_interface);
    if (!rendering.output_states.empty()) {
        desc.color_attachment_states =
            resolveMaterialColorAttachmentStates(
                info, rendering);
    }
    if (info.render_state.blend == SurfaceBlendMode::blend) {
        desc.blend = true;
        desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
        desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
        desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
        desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    } else if (info.render_state.blend == SurfaceBlendMode::additive) {
        desc.blend = true;
        desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
        desc.dst_color_blend_factor = vk::BlendFactor::eOne;
        desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
        desc.dst_alpha_blend_factor = vk::BlendFactor::eOne;
    }
    return desc;
}

static void validateMaterialCapabilities(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract
        &rendering) {
    const auto physical_device = GET_MODULE(VulkanManageCore).getPhysDevice();
    const auto limits = physical_device.getProperties().limits;
    if (rendering.color_formats.size() >
        limits.maxColorAttachments) {
        throw std::runtime_error(
            "material output schema requires " +
            std::to_string(
                rendering.color_formats.size()) +
            " color attachments but device "
            "maxColorAttachments is " +
            std::to_string(limits.maxColorAttachments));
    }
    const auto reserved = info.vat ? vatMaterialTextureBindingCount : baseMaterialTextureBindingCount;
    const auto sampler_count = static_cast<std::uint32_t>(reserved + info.custom_textures.size());
    if (sampler_count > limits.maxPerStageDescriptorSamplers ||
        sampler_count > limits.maxDescriptorSetSamplers) {
        throw std::runtime_error("material custom textures require " +
                                 std::to_string(sampler_count) +
                                 " samplers but device descriptor limit is " +
                                 std::to_string(std::min(limits.maxPerStageDescriptorSamplers,
                                                         limits.maxDescriptorSetSamplers)));
    }
    if (!info.render_state.depth_test && info.render_state.depth_write) {
        throw std::runtime_error(
            "material render_state requests depth_write while depth_test is disabled");
    }
    const auto attachment_states =
        resolveMaterialColorAttachmentStates(
            info, rendering);
    const auto independent_states =
        attachment_states.size() > 1 &&
        std::any_of(
            attachment_states.begin() + 1,
            attachment_states.end(),
            [&](const auto &state) {
                return state !=
                       attachment_states.front();
            });
    if (independent_states &&
        !GET_MODULE(VulkanManageCore)
             .getRuntimeCapabilities()
             .independent_blend) {
        throw std::runtime_error(
            "material output attachment states differ but the "
            "device does not support independentBlend");
    }
    for (std::size_t location = 0;
         location < rendering.color_formats.size();
         ++location) {
        const auto format =
            rendering.color_formats[location];
        const auto features =
            physical_device.getFormatProperties(format)
                .optimalTilingFeatures;
        if (!(features &
              vk::FormatFeatureFlagBits::
                  eColorAttachment)) {
            throw std::runtime_error(
                "material output format lacks device color "
                "attachment capability: " +
                vk::to_string(format));
        }
        if (attachment_states[location]
                .blend_enabled) {
            if (!(features & vk::FormatFeatureFlagBits::eColorAttachmentBlend)) {
                throw std::runtime_error(
                    "material output blend lacks device "
                    "capability for color format " +
                    vk::to_string(format) +
                    " at attachment " +
                    std::to_string(location));
            }
        }
    }
    if (info.render_state.depth_test ||
        info.render_state.depth_write) {
        if (!rendering.depth_format) {
            throw std::runtime_error(
                "material render_state requires a depth attachment");
        }
        const auto depth_features =
            physical_device
                .getFormatProperties(
                    *rendering.depth_format)
                .optimalTilingFeatures;
        if (!(depth_features &
              vk::FormatFeatureFlagBits::
                  eDepthStencilAttachment)) {
            throw std::runtime_error(
                "material render_state depth lacks device "
                "capability for format " +
                vk::to_string(
                    *rendering.depth_format));
        }
    }
}

static ReflectedImageViewDimension reflectedTextureDimension(
    SurfaceTextureDimension dimension) {
    switch (dimension) {
    case SurfaceTextureDimension::two_d:
        return ReflectedImageViewDimension::two_d;
    case SurfaceTextureDimension::cube:
        return ReflectedImageViewDimension::cube;
    case SurfaceTextureDimension::two_d_array:
        return ReflectedImageViewDimension::two_d_array;
    case SurfaceTextureDimension::three_d:
        return ReflectedImageViewDimension::three_d;
    }
    throw std::runtime_error(
        "unknown material texture dimension");
}

static void validateMaterialTextureReflection(
    const MaterialInfo &info,
    const ShaderReflection &reflection,
    bool split_custom_samplers) {
    for (std::size_t index = 0;
         index < info.custom_textures.size(); ++index) {
        const auto &texture = info.custom_textures[index];
        const auto image_binding =
            materialCustomTextureFirstBinding +
            static_cast<std::uint32_t>(
                index *
                (split_custom_samplers ? 2u : 1u));
        const auto expected_type =
            split_custom_samplers
                ? vk::DescriptorType::eSampledImage
                : vk::DescriptorType::
                      eCombinedImageSampler;
        const auto image = std::find_if(
            reflection.bindings.begin(),
            reflection.bindings.end(),
            [&](const auto &binding) {
                return binding.set ==
                           imageDescriptorSetNumber &&
                       binding.binding ==
                           image_binding;
            });
        if (image == reflection.bindings.end()) {
            throw std::runtime_error(
                "material texture '" + texture.name +
                "' is absent from shader reflection at binding " +
                std::to_string(image_binding));
        }
        const auto expected_dimension =
            reflectedTextureDimension(
                texture.dimension);
        if (image->count != 1 ||
            image->type != expected_type ||
            image->image_view_dimension !=
                expected_dimension) {
            throw std::runtime_error(
                "material texture '" + texture.name +
                "' shader reflection mismatch at binding " +
                std::to_string(image_binding) +
                ": expected " +
                vk::to_string(expected_type) + " " +
                std::string{
                    reflectedImageViewDimensionName(
                        expected_dimension)} +
                ", found " +
                vk::to_string(image->type) + " " +
                std::string{
                    reflectedImageViewDimensionName(
                        image->image_view_dimension)});
        }
        if (!split_custom_samplers) {
            continue;
        }
        const auto sampler_binding =
            image_binding + 1;
        const auto sampler = std::find_if(
            reflection.bindings.begin(),
            reflection.bindings.end(),
            [&](const auto &binding) {
                return binding.set ==
                           imageDescriptorSetNumber &&
                       binding.binding ==
                           sampler_binding;
            });
        if (sampler == reflection.bindings.end() ||
            sampler->count != 1 ||
            sampler->type !=
                vk::DescriptorType::eSampler) {
            throw std::runtime_error(
                "material texture '" + texture.name +
                "' split sampler reflection mismatch at binding " +
                std::to_string(sampler_binding));
        }
    }
}

struct ReflectedMaterialPassInput {
    MaterialPassInputContract contract;
    vk::DescriptorType descriptor_type =
        vk::DescriptorType::eCombinedImageSampler;
    std::optional<std::uint32_t>
        input_attachment_index;
};

static std::vector<ReflectedMaterialPassInput>
resolveReflectedMaterialPassInputs(
    PipelineHandle pipeline,
    std::span<const MaterialScreenInputContract>
        declared_screen_inputs,
    std::span<const ShaderResourceInterfaceBinding>
        resource_interface) {
    const auto &reflection =
        GET_MODULE(PipelineFactory)
            .reflection(pipeline);
    std::vector<MaterialScreenInputReflectionBinding>
        bindings;
    bindings.reserve(reflection.bindings.size());
    for (const auto &binding :
         reflection.bindings) {
        if (binding.set !=
            PELICAN_SET_PASS_INPUT) {
            continue;
        }
        if (std::any_of(
                resource_interface.begin(),
                resource_interface.end(),
                [&](const auto &resource) {
                    return binding.set ==
                               PELICAN_SET_PASS_INPUT &&
                           binding.binding ==
                               resource.binding;
                })) {
            continue;
        }
        bindings.push_back({
            .set = binding.set,
            .binding = binding.binding,
            .kind =
                binding.type ==
                        vk::DescriptorType::
                            eCombinedImageSampler
                    ? MaterialScreenInputReflectionKind::
                          combined_image_sampler
                    : binding.type ==
                              vk::DescriptorType::
                                  eInputAttachment
                          ? MaterialScreenInputReflectionKind::
                                input_attachment
                    : MaterialScreenInputReflectionKind::
                          unsupported,
            .name = binding.name,
            .input_attachment_index =
                binding.input_attachment_index,
        });
    }
    std::sort(
        bindings.begin(), bindings.end(),
        [](const auto &left, const auto &right) {
            if (left.set != right.set) {
                return left.set < right.set;
            }
            return left.binding < right.binding;
        });
    auto resolved =
        resolveMaterialPassInputInterfaceReflection(
            makeBuiltinLogicalTypeRegistry(),
            declared_screen_inputs, bindings,
            PELICAN_SET_PASS_INPUT);
    if (resolved.size() > maxMaterialPassInputs) {
        throw std::runtime_error(
            "material has too many pass inputs");
    }
    std::vector<ReflectedMaterialPassInput> result;
    result.reserve(resolved.size());
    for (std::size_t index = 0;
         index < resolved.size(); ++index) {
        const auto &binding = bindings.at(index);
        result.push_back({
            .contract = std::move(resolved[index]),
            .descriptor_type =
                binding.kind ==
                        MaterialScreenInputReflectionKind::
                            input_attachment
                    ? vk::DescriptorType::
                          eInputAttachment
                    : vk::DescriptorType::
                          eCombinedImageSampler,
            .input_attachment_index =
                binding.input_attachment_index,
        });
    }
    return result;
}

MaterialGpuData makeMaterialGpuData(const MaterialInfo &info) {
    MaterialGpuData data;
    data.base_color_factor = info.base_color_factor;
    data.emissive_factor = glm::vec4{info.emissive_factor, 1.0f};
    data.surface_factors =
        glm::vec4{info.metallic_factor, info.roughness_factor, info.normal_scale, info.occlusion_strength};
    if (info.vat) {
        const auto &vat = *info.vat;
        data.vat_bounds_min_frame_count =
            glm::vec4{vat.bounds_min, static_cast<float>(vat.frame_count)};
        data.vat_bounds_extent_fps = glm::vec4{vat.bounds_max - vat.bounds_min, vat.fps};
        data.vat_flags = glm::ivec4{vat.base_vertex, vat.loop ? 1 : 0, vat.has_normal ? 1 : 0, 0};
    }
    if (info.custom_values.size() > materialCustomValueCapacity) {
        throw std::runtime_error("material custom values size " +
                                 std::to_string(info.custom_values.size()) +
                                 " exceeds MaterialBuffer capacity " +
                                 std::to_string(materialCustomValueCapacity));
    }
    if (!info.custom_values.empty()) {
        std::memcpy(data.custom_values.data(), info.custom_values.data(), info.custom_values.size());
    }
    return data;
}

static vk::UniqueDescriptorPool createDescriptorPool(vk::Device device, bool split_custom_samplers) {
    std::vector<vk::DescriptorPoolSize> pool_sizes{
        {vk::DescriptorType::eStorageBuffer, 1024},
        {vk::DescriptorType::eCombinedImageSampler, 32768},
    };
    if (split_custom_samplers) {
        pool_sizes.push_back({vk::DescriptorType::eSampledImage, 16384});
        pool_sizes.push_back({vk::DescriptorType::eSampler, 16384});
    }

    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = 1024;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

static vk::UniqueDescriptorPool createScreenInputDescriptorPool(
    vk::Device device, uint32_t max_sets = maxMaterials * 8) {
    const std::array pool_sizes{
        vk::DescriptorPoolSize{
            vk::DescriptorType::eCombinedImageSampler,
            max_sets * maxMaterialPassInputs},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eInputAttachment,
            max_sets * maxMaterialPassInputs},
    };
    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags =
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = max_sets;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

static vk::UniqueDescriptorPool
createMaterialResourceDescriptorPool(
    vk::Device device,
    uint32_t max_sets = maxMaterials * 8) {
    const std::array pool_sizes{
        vk::DescriptorPoolSize{
            vk::DescriptorType::
                eCombinedImageSampler,
            max_sets *
                maxMaterialPassDescriptors},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eInputAttachment,
            max_sets *
                maxMaterialPassDescriptors},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eStorageBuffer,
            max_sets *
                maxMaterialPassDescriptors},
    };
    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = max_sets;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

static vk::UniqueSampler createSampler(vk::Device device, vk::Filter filter) {
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = filter;
    create_info.minFilter = filter;
    create_info.mipmapMode = vk::SamplerMipmapMode::eLinear;
    create_info.addressModeU = vk::SamplerAddressMode::eRepeat;
    create_info.addressModeV = vk::SamplerAddressMode::eRepeat;
    create_info.addressModeW = vk::SamplerAddressMode::eRepeat;
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

static vk::Filter materialTextureFilter(
    SurfaceTextureFilter filter) {
    return filter == SurfaceTextureFilter::nearest
               ? vk::Filter::eNearest
               : vk::Filter::eLinear;
}

static vk::SamplerMipmapMode materialTextureMipmapMode(
    SurfaceTextureFilter filter) {
    return filter == SurfaceTextureFilter::nearest
               ? vk::SamplerMipmapMode::eNearest
               : vk::SamplerMipmapMode::eLinear;
}

static vk::SamplerAddressMode materialTextureAddressMode(
    SurfaceTextureAddressMode address) {
    switch (address) {
    case SurfaceTextureAddressMode::repeat:
        return vk::SamplerAddressMode::eRepeat;
    case SurfaceTextureAddressMode::mirrored_repeat:
        return vk::SamplerAddressMode::eMirroredRepeat;
    case SurfaceTextureAddressMode::clamp_to_edge:
        return vk::SamplerAddressMode::eClampToEdge;
    }
    throw std::runtime_error(
        "unknown material texture address mode");
}

static vk::CompareOp materialTextureCompareOp(
    SurfaceTextureCompare compare) {
    switch (compare) {
    case SurfaceTextureCompare::none:
    case SurfaceTextureCompare::always:
        return vk::CompareOp::eAlways;
    case SurfaceTextureCompare::never:
        return vk::CompareOp::eNever;
    case SurfaceTextureCompare::less:
        return vk::CompareOp::eLess;
    case SurfaceTextureCompare::equal:
        return vk::CompareOp::eEqual;
    case SurfaceTextureCompare::less_equal:
        return vk::CompareOp::eLessOrEqual;
    case SurfaceTextureCompare::greater:
        return vk::CompareOp::eGreater;
    case SurfaceTextureCompare::not_equal:
        return vk::CompareOp::eNotEqual;
    case SurfaceTextureCompare::greater_equal:
        return vk::CompareOp::eGreaterOrEqual;
    }
    throw std::runtime_error(
        "unknown material texture comparison");
}

static vk::UniqueSampler createScreenSampler(vk::Device device,
                                             vk::Filter filter) {
    vk::SamplerCreateInfo create_info;
    create_info.magFilter = filter;
    create_info.minFilter = filter;
    create_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    create_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    create_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    create_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    create_info.minLod = 0.0f;
    create_info.maxLod = 0.0f;
    return device.createSamplerUnique(create_info);
}

static vk::SamplerAddressMode materialResourceAddressMode(
    ShaderResourcePortAddressMode mode) {
    switch (mode) {
    case ShaderResourcePortAddressMode::repeat:
        return vk::SamplerAddressMode::eRepeat;
    case ShaderResourcePortAddressMode::mirrored_repeat:
        return vk::SamplerAddressMode::
            eMirroredRepeat;
    case ShaderResourcePortAddressMode::clamp_to_edge:
        return vk::SamplerAddressMode::eClampToEdge;
    }
    throw std::runtime_error(
        "unknown material resource sampler address mode");
}

static std::size_t materialResourceSamplerIndex(
    ShaderResourcePortSampling sampling) {
    const auto filter =
        sampling.filter ==
                ShaderResourcePortFilter::nearest
            ? std::size_t{1}
            : std::size_t{0};
    std::size_t address = 0;
    switch (sampling.address_mode) {
    case ShaderResourcePortAddressMode::repeat:
        address = 0;
        break;
    case ShaderResourcePortAddressMode::mirrored_repeat:
        address = 1;
        break;
    case ShaderResourcePortAddressMode::clamp_to_edge:
        address = 2;
        break;
    }
    return filter * 3 + address;
}

static vk::UniqueSampler
createMaterialResourceSampler(
    vk::Device device,
    ShaderResourcePortSampling sampling) {
    vk::SamplerCreateInfo create_info;
    create_info.magFilter =
        sampling.filter ==
                ShaderResourcePortFilter::nearest
            ? vk::Filter::eNearest
            : vk::Filter::eLinear;
    create_info.minFilter =
        create_info.magFilter;
    create_info.mipmapMode =
        sampling.filter ==
                ShaderResourcePortFilter::nearest
            ? vk::SamplerMipmapMode::eNearest
            : vk::SamplerMipmapMode::eLinear;
    create_info.addressModeU =
        materialResourceAddressMode(
            sampling.address_mode);
    create_info.addressModeV =
        create_info.addressModeU;
    create_info.addressModeW =
        create_info.addressModeU;
    create_info.minLod = 0.0f;
    create_info.maxLod = VK_LOD_CLAMP_NONE;
    return device.createSamplerUnique(
        create_info);
}

static vk::ImageViewType materialTextureViewType(
    SurfaceTextureDimension dimension) {
    switch (dimension) {
    case SurfaceTextureDimension::two_d:
        return vk::ImageViewType::e2D;
    case SurfaceTextureDimension::cube:
        return vk::ImageViewType::eCube;
    case SurfaceTextureDimension::two_d_array:
        return vk::ImageViewType::e2DArray;
    case SurfaceTextureDimension::three_d:
        return vk::ImageViewType::e3D;
    }
    throw std::runtime_error(
        "unknown material texture dimension");
}

static vk::UniqueImageView createImageView(
    vk::Device device, const ImageWrapper &image,
    vk::Format format,
    SurfaceTextureDimension dimension =
        SurfaceTextureDimension::two_d) {
    vk::ImageViewCreateInfo create_info;
    create_info.image = image.image.get();
    create_info.viewType =
        materialTextureViewType(dimension);
    create_info.format = format;
    create_info.components.r = vk::ComponentSwizzle::eR;
    create_info.components.g = vk::ComponentSwizzle::eG;
    create_info.components.b = vk::ComponentSwizzle::eB;
    create_info.components.a = vk::ComponentSwizzle::eA;
    create_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    create_info.subresourceRange.baseMipLevel = 0;
    create_info.subresourceRange.levelCount = image.mip_levels;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount =
        dimension ==
                SurfaceTextureDimension::three_d
            ? 1
            : image.array_layers;

    return device.createImageViewUnique(create_info);
}

MaterialContainer::MaterialContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      split_custom_samplers{surfaceSpvLinkExperimentalEnabled()},
      nearest_sampler{createSampler(device, vk::Filter::eNearest)},
      linear_sampler{createSampler(device, vk::Filter::eLinear)},
      screen_nearest_sampler{createScreenSampler(device, vk::Filter::eNearest)},
      screen_linear_sampler{createScreenSampler(device, vk::Filter::eLinear)},
      desc_pool{createDescriptorPool(device, split_custom_samplers)},
      screen_input_desc_pool{createScreenInputDescriptorPool(device)},
      material_buffer{GET_MODULE(VulkanManageCore).allocBuf(
          sizeof(MaterialGpuData) * maxMaterials, vk::BufferUsageFlagBits::eStorageBuffer,
          vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite)} {}
MaterialContainer::~MaterialContainer() {
    deferred_callbacks.closeAndWait();
}

vk::Sampler MaterialContainer::materialResourceSampler(
    ShaderResourcePortSampling sampling) const {
    auto &sampler =
        material_resource_samplers.at(
            materialResourceSamplerIndex(
                sampling));
    if (!sampler) {
        sampler =
            createMaterialResourceSampler(
                device, sampling);
    }
    return sampler.get();
}

vk::Sampler MaterialContainer::materialTextureSampler(
    const ResolvedMaterialSampler &sampler) {
    const MaterialSamplerKey key{
        sampler.filter, sampler.mip_filter, sampler.address,
        sampler.compare, sampler.anisotropy_enabled,
        sampler.max_anisotropy};
    const auto existing =
        custom_texture_samplers.find(key);
    if (existing != custom_texture_samplers.end()) {
        return existing->second.get();
    }

    vk::SamplerCreateInfo create_info;
    create_info.magFilter =
        materialTextureFilter(sampler.filter);
    create_info.minFilter = create_info.magFilter;
    create_info.mipmapMode =
        materialTextureMipmapMode(sampler.mip_filter);
    create_info.addressModeU =
        materialTextureAddressMode(sampler.address);
    create_info.addressModeV = create_info.addressModeU;
    create_info.addressModeW = create_info.addressModeU;
    create_info.mipLodBias = 0.0f;
    create_info.anisotropyEnable =
        sampler.anisotropy_enabled;
    create_info.maxAnisotropy =
        sampler.anisotropy_enabled
            ? sampler.max_anisotropy
            : 1.0f;
    create_info.compareEnable =
        sampler.compare != SurfaceTextureCompare::none;
    create_info.compareOp =
        materialTextureCompareOp(sampler.compare);
    create_info.minLod = 0.0f;
    create_info.maxLod = VK_LOD_CLAMP_NONE;
    create_info.borderColor =
        vk::BorderColor::eIntOpaqueBlack;
    create_info.unnormalizedCoordinates = false;
    auto created =
        device.createSamplerUnique(create_info);
    const auto handle = created.get();
    custom_texture_samplers.emplace(
        key, std::move(created));
    return handle;
}

GlobalTextureId MaterialContainer::registerTexture(vk::Extent3D extent, const void *data) {
    return registerTexture(extent, data, vk::Format::eR8G8B8A8Unorm,
                           static_cast<vk::DeviceSize>(extent.width) * extent.height * extent.depth * 4);
}

GlobalTextureId MaterialContainer::registerTexture(vk::Extent3D extent, const void *data, vk::Format format,
                                                   vk::DeviceSize bytes_num) {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    const bool rgba8 = format == vk::Format::eR8G8B8A8Unorm;
    const std::array mutable_formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb};
    auto image = vkcore.allocImage(extent, format,
                                   vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst |
                                       vk::ImageUsageFlagBits::eTransferSrc,
                                   vma::MemoryUsage::eAutoPreferDevice, {}, VulkanProcessType::graphics,
                                   rgba8 ? std::span<const vk::Format>{mutable_formats}
                                         : std::span<const vk::Format>{});

    auto &vkutil = GET_MODULE(VulkanUtils);
    vkutil.safeTransferMemoryToImage(image, data, bytes_num,
                                     VulkanUtils::ImageTransferInfo{
                                         .old_layout = vk::ImageLayout::eUndefined,
                                         .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                         .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                                                      vk::PipelineStageFlagBits::eFragmentShader,
                                         .dst_access = vk::AccessFlagBits::eShaderRead,
                                     });

    auto linear_view = createImageView(device, image, format);
    vk::UniqueImageView srgb_view;
    if (rgba8) {
        const auto features = vkcore.getPhysDevice()
                                  .getFormatProperties(vk::Format::eR8G8B8A8Srgb)
                                  .optimalTilingFeatures;
        const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if ((features & required) != required) {
            throw std::runtime_error("SRGB material texture lacks sampled/linear-filter support");
        }
        srgb_view = createImageView(device, image, vk::Format::eR8G8B8A8Srgb);
    }

    return textures.reg(InternalTextureResource{
        .image = std::move(image),
        .linear_view = std::move(linear_view),
        .srgb_view = std::move(srgb_view),
        .dimension = SurfaceTextureDimension::two_d,
        .view_type = vk::ImageViewType::e2D,
    });
}

namespace {

vk::Format vkFormatForLoaded(ImagePixelFormat format) {
    switch (format) {
    case ImagePixelFormat::Rgba8Unorm: return vk::Format::eR8G8B8A8Unorm;
    case ImagePixelFormat::Rgba8Srgb: return vk::Format::eR8G8B8A8Srgb;
    case ImagePixelFormat::Rgba16Sfloat: return vk::Format::eR16G16B16A16Sfloat;
    case ImagePixelFormat::Rgba32Sfloat: return vk::Format::eR32G32B32A32Sfloat;
    case ImagePixelFormat::Bc5Unorm: return vk::Format::eBc5UnormBlock;
    case ImagePixelFormat::Bc7Unorm: return vk::Format::eBc7UnormBlock;
    case ImagePixelFormat::Bc7Srgb: return vk::Format::eBc7SrgbBlock;
    }
    throw std::runtime_error("Unknown loaded image format");
}

SurfaceTextureDimension surfaceTextureDimension(
    LoadedImageDimension dimension) {
    switch (dimension) {
    case LoadedImageDimension::TwoD:
        return SurfaceTextureDimension::two_d;
    case LoadedImageDimension::Cube:
        return SurfaceTextureDimension::cube;
    case LoadedImageDimension::TwoDArray:
        return SurfaceTextureDimension::two_d_array;
    case LoadedImageDimension::ThreeD:
        return SurfaceTextureDimension::three_d;
    }
    throw std::runtime_error(
        "Unknown loaded image dimension");
}

vk::ImageType materialTextureImageType(
    SurfaceTextureDimension dimension) {
    return dimension ==
                   SurfaceTextureDimension::three_d
               ? vk::ImageType::e3D
               : vk::ImageType::e2D;
}

vk::ImageCreateFlags materialTextureImageFlags(
    SurfaceTextureDimension dimension) {
    return dimension ==
                   SurfaceTextureDimension::cube
               ? vk::ImageCreateFlagBits::eCubeCompatible
               : vk::ImageCreateFlags{};
}

void validateLoadedTextureShape(
    const LoadedImage &loaded, std::string_view name,
    const vk::PhysicalDeviceLimits &limits) {
    if (loaded.width == 0 || loaded.height == 0 ||
        loaded.depth == 0 || loaded.array_layers == 0) {
        throw std::runtime_error(
            "Texture '" + std::string{name} +
            "' has a zero-sized dimension or array layer count");
    }
    switch (loaded.dimension) {
    case LoadedImageDimension::TwoD:
        if (loaded.depth != 1 || loaded.array_layers != 1) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d requires depth=1 and "
                "array_layers=1");
        }
        if (loaded.width > limits.maxImageDimension2D ||
            loaded.height > limits.maxImageDimension2D) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d exceeds device "
                "maxImageDimension2D " +
                std::to_string(limits.maxImageDimension2D));
        }
        break;
    case LoadedImageDimension::Cube:
        if (loaded.width != loaded.height ||
            loaded.depth != 1 ||
            loaded.array_layers != 6) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension cube requires square faces, "
                "depth=1, and exactly 6 array layers");
        }
        if (loaded.width > limits.maxImageDimensionCube) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension cube exceeds device "
                "maxImageDimensionCube " +
                std::to_string(limits.maxImageDimensionCube));
        }
        break;
    case LoadedImageDimension::TwoDArray:
        if (loaded.depth != 1) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d_array requires depth=1");
        }
        if (loaded.width > limits.maxImageDimension2D ||
            loaded.height > limits.maxImageDimension2D ||
            loaded.array_layers > limits.maxImageArrayLayers) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 2d_array exceeds device 2D "
                "dimension or array-layer limits");
        }
        break;
    case LoadedImageDimension::ThreeD:
        if (loaded.array_layers != 1) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 3d requires array_layers=1");
        }
        if (loaded.width > limits.maxImageDimension3D ||
            loaded.height > limits.maxImageDimension3D ||
            loaded.depth > limits.maxImageDimension3D) {
            throw std::runtime_error(
                "Texture '" + std::string{name} +
                "' dimension 3d exceeds device "
                "maxImageDimension3D " +
                std::to_string(limits.maxImageDimension3D));
        }
        break;
    }
}

void requireCompressedTextureFeatures(vk::PhysicalDevice physical_device, vk::Format format,
                                      std::string_view name) {
    if (format != vk::Format::eBc5UnormBlock && format != vk::Format::eBc7UnormBlock &&
        format != vk::Format::eBc7SrgbBlock) return;
    const auto features = physical_device.getFormatProperties(format).optimalTilingFeatures;
    const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                          vk::FormatFeatureFlagBits::eSampledImageFilterLinear |
                          vk::FormatFeatureFlagBits::eTransferDst;
    if ((features & required) != required)
        throw std::runtime_error("KTX2 texture '" + std::string{name} + "' format " +
                                 vk::to_string(format) +
                                 " lacks sampled/linear-filter/transfer-dst GPU support");
}

} // namespace

MaterialContainer::InternalTextureResource
MaterialContainer::createTextureResource(const LoadedImage &loaded, std::string_view name) const {
    if (loaded.pixels.empty() || loaded.levels.empty())
        throw std::runtime_error("Texture '" + std::string{name} + "' has no image levels");
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    const auto format = vkFormatForLoaded(loaded.format);
    requireCompressedTextureFeatures(vkcore.getPhysDevice(), format, name);
    validateLoadedTextureShape(
        loaded, name,
        vkcore.getPhysDevice().getProperties().limits);
    const auto dimension =
        surfaceTextureDimension(loaded.dimension);
    const auto image_type =
        materialTextureImageType(dimension);
    const auto image_flags =
        materialTextureImageFlags(dimension);
    const vk::Extent3D extent{
        loaded.width, loaded.height, loaded.depth};

    const bool rgba8 = loaded.format == ImagePixelFormat::Rgba8Unorm ||
                       loaded.format == ImagePixelFormat::Rgba8Srgb;
    const bool bc7 = loaded.format == ImagePixelFormat::Bc7Unorm ||
                     loaded.format == ImagePixelFormat::Bc7Srgb;
    const std::array rgba_formats{vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Srgb};
    const std::array bc7_formats{vk::Format::eBc7UnormBlock, vk::Format::eBc7SrgbBlock};
    const auto compatible = rgba8 ? std::span<const vk::Format>{rgba_formats}
                                  : bc7 ? std::span<const vk::Format>{bc7_formats}
                                        : std::span<const vk::Format>{};
    auto image = vkcore.allocImage(
        extent, format,
        vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst |
            vk::ImageUsageFlagBits::eTransferSrc,
        vma::MemoryUsage::eAutoPreferDevice, {},
        VulkanProcessType::graphics, compatible,
        loaded.mipLevels(), vk::SampleCountFlagBits::e1,
        loaded.array_layers, {}, image_flags, image_type);
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(loaded.levels.size());
    for (std::uint32_t mip = 0; mip < loaded.levels.size(); ++mip) {
        const auto &level = loaded.levels[mip];
        vk::BufferImageCopy copy;
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {
            vk::ImageAspectFlagBits::eColor, mip, 0,
            loaded.dimension ==
                    LoadedImageDimension::ThreeD
                ? 1u
                : loaded.array_layers};
        copy.imageExtent = vk::Extent3D{
            level.width, level.height, level.depth};
        regions.push_back(copy);
    }
    GET_MODULE(VulkanUtils).safeTransferMemoryToImageLevels(
        image, loaded.pixels.data(), loaded.pixels.size(), regions,
        VulkanUtils::ImageTransferInfo{.old_layout = vk::ImageLayout::eUndefined,
                                       .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                       .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                                                    vk::PipelineStageFlagBits::eFragmentShader,
                                       .dst_access = vk::AccessFlagBits::eShaderRead});

    const auto linear_format = rgba8 ? vk::Format::eR8G8B8A8Unorm
                                     : bc7 ? vk::Format::eBc7UnormBlock : format;
    requireCompressedTextureFeatures(vkcore.getPhysDevice(), linear_format, name);
    auto linear_view =
        createImageView(device, image, linear_format,
                        dimension);
    vk::UniqueImageView srgb_view;
    if (rgba8 || bc7) {
        const auto srgb_format = rgba8 ? vk::Format::eR8G8B8A8Srgb : vk::Format::eBc7SrgbBlock;
        const auto features = vkcore.getPhysDevice().getFormatProperties(srgb_format).optimalTilingFeatures;
        const auto required = vk::FormatFeatureFlagBits::eSampledImage |
                              vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
        if ((features & required) != required)
            throw std::runtime_error("KTX2 texture '" + std::string{name} + "' SRGB view " +
                                     vk::to_string(srgb_format) + " lacks sampled/linear-filter GPU support");
        srgb_view = createImageView(
            device, image, srgb_format, dimension);
    }
    return {
        std::move(image), std::move(linear_view),
        std::move(srgb_view), dimension,
        materialTextureViewType(dimension)};
}

GlobalTextureId MaterialContainer::registerTexture(const LoadedImage &loaded, std::string_view name) {
    return textures.reg(createTextureResource(loaded, name));
}

GlobalTextureId MaterialContainer::registerTextureFile(const std::filesystem::path &path) {
    return registerTexture(loadImageFile(path), path.string());
}

GlobalTextureId MaterialContainer::registerReloadableTextureFile(
    const watch::AssetKey &key, const std::filesystem::path &path) {
    const auto texture = registerTextureFile(path);
    auto &coordinator = GET_MODULE(watch::ReloadService).transactions();
    if (!texture_reload_handler) {
        texture_reload_handler = std::make_unique<TextureReloadHandler>(*this, coordinator);
    }
    texture_reload_handler->track(key, path, texture);
    return texture;
}

GlobalMaterialId MaterialContainer::registerMaterial(MaterialInfo info) {
    if (materials.size() >= maxMaterials) {
        throw std::runtime_error("Material capacity exceeded");
    }
    info.tags = canonicalizeMaterialDrawTags(
        std::move(info.tags), "registered material");
    const auto &fragment_bundle =
        GET_MODULE(ShaderLibrary).get(info.frag_shader);
    if (info.output_schema &&
        fragment_bundle.material_output_schema &&
        info.output_schema !=
            fragment_bundle.material_output_schema) {
        throw std::runtime_error(
            "material registration output_schema disagrees "
            "with the generated fragment shader");
    }
    if (!info.output_schema) {
        info.output_schema =
            fragment_bundle.material_output_schema;
    }
    if (info.output_schema) {
        validateFragmentOutputSchema(
            fragment_bundle.reflection,
            *info.output_schema,
            "registered material fragment shader");
    }
    const auto rendering =
        resolveMaterialPipelineRenderingContract(info);
    validateMaterialCapabilities(info, rendering);
    auto resource_interface =
        resolveMaterialResourceInterface(info);
    const auto pipeline_key =
        makePipelineKey(info, rendering);
    auto pipeline_it = pipelines.find(pipeline_key);
    if (pipeline_it == pipelines.end()) {
        const auto pipeline_handle =
            GET_MODULE(PipelineFactory)
                .create(makeMaterialPipelineDesc(
                    info, rendering,
                    resource_interface));
        pipeline_it = pipelines.emplace(pipeline_key, pipeline_handle).first;
        if (!default_pipeline) {
            default_pipeline = pipeline_handle;
        }
    }
    const auto pipeline = pipeline_it->second;
    const auto &pipeline_reflection =
        GET_MODULE(PipelineFactory)
            .reflection(pipeline);
    validateMaterialTextureReflection(
        info, pipeline_reflection,
        split_custom_samplers);
    validateShaderResourceInterfaceReflection(
        resource_interface, pipeline_reflection,
        PELICAN_SET_PASS_INPUT);
    auto reflected_pass_inputs =
        resolveReflectedMaterialPassInputs(
            pipeline, info.screen_inputs,
            resource_interface);
    std::vector<
        InternalMaterialInfo::PassInput>
        pass_inputs;
    pass_inputs.reserve(
        reflected_pass_inputs.size());
    for (auto &input :
         reflected_pass_inputs) {
        pass_inputs.push_back({
            .contract =
                std::move(input.contract),
            .descriptor_type =
                input.descriptor_type,
            .input_attachment_index =
                input.input_attachment_index,
        });
    }

    vk::DescriptorSetAllocateInfo desc_alloc_info;
    desc_alloc_info.descriptorPool = desc_pool.get();
    const auto material_set_layout = GET_MODULE(PipelineFactory).descriptorSetLayout(pipeline, imageDescriptorSetNumber);
    desc_alloc_info.setSetLayouts({material_set_layout});

    auto descsets = device.allocateDescriptorSetsUnique(desc_alloc_info);
    auto &descset = descsets[0];

    const auto texture_binding_count =
        info.vat ? vatMaterialTextureBindingCount : baseMaterialTextureBindingCount;
    const auto image_info_count = materialCustomTextureFirstBinding +
        info.custom_textures.size() * (split_custom_samplers ? 2 : 1);
    std::vector<vk::DescriptorImageInfo> image_infos(image_info_count);
    std::vector<InternalMaterialInfo::TextureBinding> texture_bindings;
    texture_bindings.reserve(texture_binding_count +
                             info.custom_textures.size() * (split_custom_samplers ? 2 : 1));
    std::vector<std::pair<std::string,
                          ResolvedMaterialSampler>>
        custom_sampler_resolutions;
    custom_sampler_resolutions.reserve(
        info.custom_textures.size());

    const auto setImageInfo = [&](uint32_t binding, GlobalTextureId texture, vk::Sampler sampler,
                                  bool srgb,
                                  SurfaceTextureDimension
                                      expected_dimension =
                                          SurfaceTextureDimension::two_d,
                                  std::optional<ResolvedMaterialSampler>
                                      sampler_resolution =
                                          std::nullopt) {
        const auto &tex = textures.get(texture);
        if (tex.dimension != expected_dimension) {
            throw std::runtime_error(
                "declared dimension " +
                std::string{surfaceTextureDimensionName(
                    expected_dimension)} +
                " does not match loaded texture dimension " +
                std::string{surfaceTextureDimensionName(
                    tex.dimension)});
        }
        if (srgb && !tex.srgb_view) {
            throw std::runtime_error("Color texture does not provide an SRGB view");
        }
        image_infos[binding].imageView = srgb ? tex.srgb_view.get() : tex.linear_view.get();
        image_infos[binding].imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        image_infos[binding].sampler = sampler;
        texture_bindings.push_back({texture, binding,
                                    vk::DescriptorType::eCombinedImageSampler,
                                    sampler, srgb,
                                    expected_dimension,
                                    std::move(
                                        sampler_resolution)});
    };

    setImageInfo(baseColorBinding, info.base_color_texture, linear_sampler.get(), true);
    setImageInfo(metallicRoughnessBinding, info.metallic_roughness_texture, linear_sampler.get(), false);
    setImageInfo(normalBinding, info.normal_texture, linear_sampler.get(), false);
    setImageInfo(emissiveBinding, info.emissive_texture, linear_sampler.get(), true);
    if (info.vat) {
        setImageInfo(vatPositionBinding, info.vat->position_texture, nearest_sampler.get(), false);
        setImageInfo(vatNormalBinding, info.vat->normal_texture, nearest_sampler.get(), false);
    }
    for (std::size_t i = 0; i < info.custom_textures.size(); ++i) {
        const auto &custom = info.custom_textures[i];
        if (!custom.texture &&
            custom.dimension !=
                SurfaceTextureDimension::two_d) {
            throw std::runtime_error(
                "material texture '" + custom.name +
                "' declares dimension " +
                std::string{surfaceTextureDimensionName(
                    custom.dimension)} +
                " and requires a matching resolved texture; "
                "the 2d semantic dummy is not compatible");
        }
        const auto texture = custom.texture.value_or(
            GET_MODULE(StandardMaterialResource).defaultTexture(custom.missing_default));
        try {
            const auto &resource =
                textures.get(texture);
            const auto &runtime_capabilities =
                GET_MODULE(VulkanManageCore)
                    .getRuntimeCapabilities();
            const auto limits =
                GET_MODULE(VulkanManageCore)
                    .getPhysDevice()
                    .getProperties()
                    .limits;
            const auto resolved_sampler =
                resolveMaterialSampler(
                    custom.sampler,
                    MaterialSamplerCapabilities{
                        .comparison_sampling =
                            supportsDepthComparisonSampling(
                                resource.image.format),
                        .sampler_anisotropy =
                            runtime_capabilities
                                .sampler_anisotropy,
                        .max_sampler_anisotropy =
                            limits.maxSamplerAnisotropy,
                    },
                    "material texture '" + custom.name +
                        "'");
            if (resolved_sampler.resolution != "exact") {
                LOG_WARNING(
                    logger,
                    "Material texture '{}' sampler resolved with "
                    "reason '{}': requested anisotropy={}, "
                    "device max={}",
                    custom.name,
                    resolved_sampler.resolution,
                    custom.sampler.anisotropy,
                    limits.maxSamplerAnisotropy);
            }
            const auto custom_sampler =
                materialTextureSampler(resolved_sampler);
            const auto binding = materialCustomTextureFirstBinding + static_cast<std::uint32_t>(
                i * (split_custom_samplers ? 2 : 1));
            setImageInfo(
                binding, texture, custom_sampler,
                custom.role == SurfaceTextureRole::color,
                custom.dimension, resolved_sampler);
            custom_sampler_resolutions.emplace_back(
                custom.name, resolved_sampler);
            if (split_custom_samplers) {
                texture_bindings.back().descriptor_type = vk::DescriptorType::eSampledImage;
                image_infos[binding + 1] = image_infos[binding];
                texture_bindings.push_back({texture, binding + 1,
                                            vk::DescriptorType::eSampler,
                                            custom_sampler,
                                            custom.role == SurfaceTextureRole::color,
                                            custom.dimension,
                                            resolved_sampler});
            }
        } catch (const std::exception &error) {
            throw std::runtime_error("material texture '" + custom.name + "': " + error.what());
        }
    }

    std::vector<vk::WriteDescriptorSet> writes;
    writes.reserve(texture_binding_count + info.custom_textures.size() + 1);
    const auto addImageWrite = [&](uint32_t binding, vk::DescriptorType type) {
        vk::WriteDescriptorSet write;
        write.dstSet = descset.get();
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pImageInfo = &image_infos[binding];
        writes.push_back(write);
    };

    addImageWrite(baseColorBinding, vk::DescriptorType::eCombinedImageSampler);
    addImageWrite(metallicRoughnessBinding, vk::DescriptorType::eCombinedImageSampler);
    addImageWrite(normalBinding, vk::DescriptorType::eCombinedImageSampler);
    addImageWrite(emissiveBinding, vk::DescriptorType::eCombinedImageSampler);
    if (info.vat) {
        addImageWrite(vatPositionBinding, vk::DescriptorType::eCombinedImageSampler);
        addImageWrite(vatNormalBinding, vk::DescriptorType::eCombinedImageSampler);
    }
    for (std::size_t i = 0; i < info.custom_textures.size(); ++i) {
        const auto binding = materialCustomTextureFirstBinding + static_cast<std::uint32_t>(
            i * (split_custom_samplers ? 2 : 1));
        if (split_custom_samplers) {
            addImageWrite(binding, vk::DescriptorType::eSampledImage);
            addImageWrite(binding + 1, vk::DescriptorType::eSampler);
        } else {
            addImageWrite(binding, vk::DescriptorType::eCombinedImageSampler);
        }
    }

    vk::DescriptorBufferInfo material_buffer_info{material_buffer.buffer.get(), 0, vk::WholeSize};
    vk::WriteDescriptorSet material_write;
    material_write.dstSet = descset.get();
    material_write.dstBinding = materialBufferBinding;
    material_write.descriptorType = vk::DescriptorType::eStorageBuffer;
    material_write.setBufferInfo(material_buffer_info);
    writes.push_back(material_write);

    device.updateDescriptorSets(writes, {});

    const auto gpu_data = makeMaterialGpuData(info);
    const auto material_id = materials.reg(InternalMaterialInfo{
        .pipeline = pipeline,
        .pipeline_color_formats =
            rendering.color_formats,
        .pipeline_depth_format =
            rendering.depth_format,
        .pipeline_rasterization_samples =
            rendering.rasterization_samples,
        .pipeline_local_read =
            rendering.local_read,
        .pipeline_output_states =
            rendering.output_states,
        .tags = std::move(info.tags),
        .route = info.route,
        .shader_contract = info.shader_contract,
        .output_schema =
            std::move(info.output_schema),
        .exact_pass = std::move(info.exact_pass),
        .pass_inputs = std::move(pass_inputs),
        .resource_interface =
            std::move(resource_interface),
        .declared_screen_inputs =
            std::move(info.screen_inputs),
        .declared_resource_ports =
            std::move(info.resource_ports),
        .render_state = info.render_state,
        .skinned = info.skinned,
        .base_color_texture = info.base_color_texture,
        .metallic_roughness_texture = info.metallic_roughness_texture,
        .normal_texture = info.normal_texture,
        .emissive_texture = info.emissive_texture,
        .base_color_factor = info.base_color_factor,
        .emissive_factor = info.emissive_factor,
        .metallic_factor = info.metallic_factor,
        .roughness_factor = info.roughness_factor,
        .normal_scale = info.normal_scale,
        .occlusion_strength = info.occlusion_strength,
        .vat = info.vat,
        .texture_bindings = std::move(texture_bindings),
        .custom_sampler_resolutions =
            std::move(custom_sampler_resolutions),
        .custom_values_layout = info.custom_values_layout,
        .custom_values = info.custom_values,
        .descriptor_revision = 0,
        .descset = std::move(descset),
    }, static_cast<GlobalMaterialId::BaseType>(maxMaterials));
    try {
        if (material_id.value < 0 || static_cast<size_t>(material_id.value) >= maxMaterials) {
            throw std::runtime_error("Material capacity exceeded");
        }
        const auto &registered_passes = GET_MODULE(RenderingPassContainer);
        const auto bind_screen_inputs =
            [&](const CompiledRenderingPass &rendering_pass) {
                for (const auto &compiled :
                     rendering_pass.passes) {
                    if (compiled.definition.isMaterial() &&
                        compiled.definition.materialInfo()
                            .material_variant) {
                        continue;
                    }
                    if (isRenderRequired(
                            compiled.definition, material_id)) {
                        (void)ensureScreenInputDescriptor(
                            material_id, compiled.definition);
                    }
                }
            };
        if (const auto generation =
                registered_passes.snapshot()) {
            for (const auto rendering_pass_id :
                 generation->rendering_pass_ids) {
                const auto *program =
                    generation->find(rendering_pass_id);
                if (program == nullptr) {
                    throw std::logic_error(
                        "Published render pipeline pass table is inconsistent");
                }
                bind_screen_inputs(program->rendering_pass);
            }
        } else {
            for (const auto rendering_pass_id :
                 registered_passes.getRegisteredPassIds()) {
                bind_screen_inputs(
                    registered_passes
                        .getCompiledRenderingPass(
                            rendering_pass_id));
            }
        }
        GET_MODULE(VulkanManageCore)
            .writeBuf(material_buffer, &gpu_data, sizeof(MaterialGpuData) * material_id.value,
                      sizeof(gpu_data));
        for (const auto &binding : materials.get(material_id).texture_bindings) {
            texture_materials[binding.texture].insert(material_id);
        }
        if (texture_reload_handler) texture_reload_handler->materialRegistered(material_id);
    } catch (...) {
        for (auto it = texture_materials.begin(); it != texture_materials.end();) {
            it->second.erase(material_id);
            if (it->second.empty()) it = texture_materials.erase(it);
            else ++it;
        }
        (void)materials.extract(material_id);
        throw;
    }
    return material_id;
}

void MaterialContainer::registerMaterialVariants(
    GlobalMaterialId base,
    std::vector<NamedMaterialVariantRegistration> registrations) {
    const auto &registered_base = materials.get(base);
    const auto base_route = registered_base.route;
    const auto base_skinned = registered_base.skinned;
    const auto base_color_texture =
        registered_base.base_color_texture;
    const auto metallic_roughness_texture =
        registered_base.metallic_roughness_texture;
    const auto normal_texture =
        registered_base.normal_texture;
    const auto emissive_texture =
        registered_base.emissive_texture;
    const auto base_color_factor =
        registered_base.base_color_factor;
    const auto emissive_factor =
        registered_base.emissive_factor;
    const auto metallic_factor =
        registered_base.metallic_factor;
    const auto roughness_factor =
        registered_base.roughness_factor;
    const auto normal_scale =
        registered_base.normal_scale;
    const auto occlusion_strength =
        registered_base.occlusion_strength;
    const auto base_vat = registered_base.vat;
    std::map<std::string, GlobalMaterialId> next =
        registered_base.variants;
    std::vector<std::pair<std::string, GlobalMaterialId>> created;
    created.reserve(registrations.size());
    bool attached = false;

    for (const auto &registration : registrations) {
        validateMaterialVariantName(
            registration.name,
            "material " + std::to_string(base.value));
        if (next.contains(registration.name)) {
            throw std::runtime_error(
                "material " + std::to_string(base.value) +
                " has duplicate variant '" + registration.name + "'");
        }
        const auto base_is_transparent =
            base_route ==
            MaterialRouteClass::forward_transparent;
        const auto variant_is_transparent =
            registration.material.route ==
            MaterialRouteClass::forward_transparent;
        if (base_is_transparent != variant_is_transparent) {
            throw std::runtime_error(
                "material " + std::to_string(base.value) +
                " variant '" + registration.name +
                "' changes draw phase from " +
                std::string{base_is_transparent ? "transparent" : "opaque"} +
                " to " +
                std::string{variant_is_transparent ? "transparent" : "opaque"} +
                "; cross-phase variants require a variant-aware draw queue");
        }
        next.emplace(registration.name, invalidMaterialId());
    }

    try {
        for (auto &registration : registrations) {
            // A hidden runtime resource must never enter a draw queue through
            // the parent's selection tags.
            registration.material.tags.clear();
            registration.material.skinned = base_skinned;
            registration.material.base_color_texture =
                base_color_texture;
            registration.material.metallic_roughness_texture =
                metallic_roughness_texture;
            registration.material.normal_texture =
                normal_texture;
            registration.material.emissive_texture =
                emissive_texture;
            registration.material.base_color_factor =
                base_color_factor;
            registration.material.emissive_factor =
                emissive_factor;
            registration.material.metallic_factor =
                metallic_factor;
            registration.material.roughness_factor =
                roughness_factor;
            registration.material.normal_scale =
                normal_scale;
            registration.material.occlusion_strength =
                occlusion_strength;
            registration.material.vat = base_vat;
            const auto resource =
                registerMaterial(std::move(registration.material));
            created.emplace_back(registration.name, resource);
            next.at(registration.name) = resource;
        }

        auto &base_info = materials.get(base);
        base_info.variants.swap(next);
        attached = true;

        const auto prewarm =
            [&](const CompiledRenderingPass &rendering_pass) {
                for (const auto &compiled :
                     rendering_pass.passes) {
                    const auto &pass = compiled.definition;
                    if (!pass.isMaterial() ||
                        !pass.materialInfo().material_variant) {
                        continue;
                    }
                    if (!pass.materialInfo().material_filter) {
                        throw std::runtime_error(
                            "material variant pass '" + pass.name +
                            "' has no material_filter");
                    }
                    const auto &filter =
                        *pass.materialInfo().material_filter;
                    if (!materialDrawTagFilterMatches(
                            base_info.tags, filter)) {
                        continue;
                    }
                    if (isRenderRequired(pass, base)) {
                        const auto effective =
                            resolveMaterialForPass(pass, base);
                        (void)ensureScreenInputDescriptor(
                            effective, pass);
                    }
                }
            };
        const auto &registered_passes =
            GET_MODULE(RenderingPassContainer);
        if (const auto generation =
                registered_passes.snapshot()) {
            for (const auto rendering_pass_id :
                 generation->rendering_pass_ids) {
                const auto *program =
                    generation->find(rendering_pass_id);
                if (program == nullptr) {
                    throw std::logic_error(
                        "Published render pipeline pass table is inconsistent");
                }
                prewarm(program->rendering_pass);
            }
        } else {
            for (const auto rendering_pass_id :
                 registered_passes.getRegisteredPassIds()) {
                prewarm(
                    registered_passes.getCompiledRenderingPass(
                        rendering_pass_id));
            }
        }
    } catch (...) {
        if (attached && materials.contains(base)) {
            materials.get(base).variants.swap(next);
        }
        std::vector<GlobalMaterialId> resources;
        resources.reserve(created.size());
        for (const auto &[name, resource] : created) {
            (void)name;
            resources.push_back(resource);
        }
        releaseModelResources(std::move(resources), {}, false);
        throw;
    }
}

GlobalMaterialId MaterialContainer::materialVariantResource(
    GlobalMaterialId base, std::string_view name) const {
    const auto &base_info = materials.get(base);
    const auto found = base_info.variants.find(std::string{name});
    if (found == base_info.variants.end()) {
        throw std::runtime_error(
            "material " + std::to_string(base.value) +
            " has no variant '" + std::string{name} + "'");
    }
    return found->second;
}

void MaterialContainer::releaseModelResources(
    std::vector<GlobalMaterialId> material_ids,
    std::vector<GlobalTextureId> texture_ids, bool deferred) noexcept {
    try {
        std::vector<GlobalMaterialId> expanded_material_ids;
        std::unordered_set<GlobalMaterialId, GlobalMaterialId::Hash>
            seen_material_ids;
        for (const auto material : material_ids) {
            if (!seen_material_ids.insert(material).second) continue;
            expanded_material_ids.push_back(material);
            if (!materials.contains(material)) continue;
            for (const auto &[name, variant] :
                 materials.get(material).variants) {
                (void)name;
                if (seen_material_ids.insert(variant).second) {
                    expanded_material_ids.push_back(variant);
                }
            }
        }
        material_ids = std::move(expanded_material_ids);

        struct RetiredResources {
            std::vector<GlobalMaterialId> material_ids;
            std::vector<InternalMaterialInfo> materials;
            std::vector<InternalTextureResource> textures;
            std::function<void()> recycle_material_ids;

            RetiredResources() = default;
            RetiredResources(const RetiredResources &) = delete;
            RetiredResources &operator=(const RetiredResources &) = delete;
            RetiredResources(RetiredResources &&other) noexcept
                : material_ids{std::move(other.material_ids)},
                  materials{std::move(other.materials)},
                  textures{std::move(other.textures)},
                  recycle_material_ids{std::exchange(other.recycle_material_ids, {})} {}
            RetiredResources &operator=(RetiredResources &&) = delete;
            ~RetiredResources() noexcept {
                if (!recycle_material_ids) return;
                try {
                    recycle_material_ids();
                } catch (const std::exception &error) {
                    if (logger)
                        LOG_ERROR(logger, "failed to recycle retired model material slots: {}",
                                  error.what());
                } catch (...) {
                    if (logger)
                        LOG_ERROR(logger, "failed to recycle retired model material slots");
                }
            }
        } retired;
        retired.material_ids.reserve(material_ids.size());
        retired.materials.reserve(material_ids.size());
        retired.textures.reserve(texture_ids.size());

        for (const auto material : material_ids) {
            if (!materials.contains(material)) continue;
            const auto bindings = materials.get(material).texture_bindings;
            for (const auto &binding : bindings) {
                const auto found = texture_materials.find(binding.texture);
                if (found == texture_materials.end()) continue;
                found->second.erase(material);
                if (found->second.empty()) texture_materials.erase(found);
            }
            auto value = materials.extract(material, !deferred);
            if (value) {
                if (deferred) retired.material_ids.push_back(material);
                retired.materials.push_back(std::move(*value));
            }
        }

        for (const auto texture : texture_ids) {
            const auto reverse = texture_materials.find(texture);
            if (reverse != texture_materials.end() && !reverse->second.empty()) {
                if (logger) {
                    LOG_ERROR(logger,
                              "model texture {} still has {} material references during retirement",
                              texture.value, reverse->second.size());
                }
                continue;
            }
            texture_materials.erase(texture);
            auto value = textures.extract(texture);
            if (value) retired.textures.push_back(std::move(*value));
        }

        if (!retired.material_ids.empty()) {
            retired.recycle_material_ids =
                [this, callback = deferred_callbacks.callback(),
                 ids = retired.material_ids] {
                    const auto lease = callback.acquire();
                    if (!lease) return;
                    for (const auto id : ids) materials.recycle(id);
                };
        }
        if (deferred && (!retired.materials.empty() || !retired.textures.empty())) {
            if (auto *queue = FastModuleContainer::tryGet<DeletionQueue>()) {
                queue->defer(std::move(retired));
            }
        }
    } catch (const std::exception &error) {
        if (logger) LOG_ERROR(logger, "failed to release model material resources: {}", error.what());
    } catch (...) {
        if (logger) LOG_ERROR(logger, "failed to release model material resources");
    }
}

namespace {

bool sameValuesLayout(const Std140Layout &left, const Std140Layout &right) {
    if (left.size != right.size || left.alignment != right.alignment ||
        left.members.size() != right.members.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.members.size(); ++i) {
        const auto &a = left.members[i];
        const auto &b = right.members[i];
        if (a.name != b.name || a.type != b.type || a.offset != b.offset ||
            a.size != b.size || a.alignment != b.alignment) {
            return false;
        }
    }
    return true;
}

} // namespace

bool MaterialContainer::materialValuesLayoutMatches(GlobalMaterialId material,
                                                     const Std140Layout &layout) const {
    return sameValuesLayout(materials.get(material).custom_values_layout, layout);
}

void MaterialContainer::updateMaterialValues(GlobalMaterialId material,
                                             const Std140Layout &layout,
                                             std::span<const std::byte> values) {
    auto &info = materials.get(material);
    if (!sameValuesLayout(info.custom_values_layout, layout)) {
        throw std::runtime_error("material values update requires an identical std140 layout");
    }
    if (values.size() != layout.size || values.size() > materialCustomValueCapacity) {
        throw std::runtime_error("material values update byte count does not match its layout");
    }

    std::array<std::uint32_t, materialCustomValueCapacity / 4> packed{};
    if (!values.empty()) std::memcpy(packed.data(), values.data(), values.size());
    // MaterialBuffer is shared by in-flight frames. The frame-boundary caller
    // owns publication, while this wait protects readers of the live SSBO.
    GET_MODULE(VulkanManageCore).waitIdle();
    const auto offset = sizeof(MaterialGpuData) * material.value +
                        offsetof(MaterialGpuData, custom_values);
    GET_MODULE(VulkanManageCore).writeBuf(material_buffer, packed.data(), offset,
                                          sizeof(packed));
    info.custom_values.assign(values.begin(), values.end());
}

void MaterialContainer::updateMaterialValues(GlobalMaterialId material,
                                             std::span<const std::byte> values) {
    updateMaterialValues(material, materials.get(material).custom_values_layout, values);
}

void MaterialContainer::registerReloadableMaterialValuesFile(
    const watch::AssetKey &key, const std::filesystem::path &path,
    MaterialSurfaceCatalog surfaces,
    std::span<const ReloadableMaterialValuesBinding> bindings) {
    auto &coordinator = GET_MODULE(watch::ReloadService).transactions();
    if (!material_values_reload_handler) {
        material_values_reload_handler =
            std::make_unique<MaterialValuesReloadHandler>(*this, coordinator);
    }
    material_values_reload_handler->track(key, path, std::move(surfaces), bindings);
}

std::function<void()> MaterialContainer::prepareSurfaceMaterialReload(
    const std::map<watch::AssetKey, SurfaceFormatDocument> &surface_documents,
    std::span<const watch::AssetKey> material_documents) {
    if (!material_values_reload_handler) return {};
    return material_values_reload_handler->prepareSurfaceReload(
        surface_documents, material_documents);
}

bool MaterialContainer::textureShapeMatches(GlobalTextureId texture,
                                            const LoadedImage &image) const {
    const auto &resource = textures.get(texture);
    const auto &live = resource.image;
    return resource.dimension ==
               surfaceTextureDimension(image.dimension) &&
           live.extent ==
               vk::Extent3D{image.width, image.height,
                            image.depth} &&
           live.array_layers == image.array_layers &&
           live.format == vkFormatForLoaded(image.format) &&
           live.mip_levels == image.mipLevels();
}

void MaterialContainer::validateTextureReload(GlobalTextureId texture,
                                              const LoadedImage &image) const {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    validateLoadedTextureShape(
        image, "reload candidate",
        vkcore.getPhysDevice().getProperties().limits);
    const auto candidate_dimension =
        surfaceTextureDimension(image.dimension);
    const bool has_srgb_view = image.format == ImagePixelFormat::Rgba8Unorm ||
                               image.format == ImagePixelFormat::Rgba8Srgb ||
                               image.format == ImagePixelFormat::Bc7Unorm ||
                               image.format == ImagePixelFormat::Bc7Srgb;
    const auto candidate_format =
        vkFormatForLoaded(image.format);
    const auto reverse = texture_materials.find(texture);
    if (reverse == texture_materials.end()) return;
    for (const auto material_id : reverse->second) {
        const auto &material = materials.get(material_id);
        for (const auto &binding :
             material.texture_bindings) {
            if (binding.texture != texture) continue;
            if (binding.expected_dimension !=
                candidate_dimension) {
                throw std::runtime_error(
                    "reloaded texture dimension " +
                    std::string{surfaceTextureDimensionName(
                        candidate_dimension)} +
                    " does not match material declaration " +
                    std::string{surfaceTextureDimensionName(
                        binding.expected_dimension)});
            }
            if (!has_srgb_view && binding.srgb) {
                throw std::runtime_error(
                    "reloaded color texture format does not "
                    "provide an SRGB view");
            }
            if (binding.sampler_resolution &&
                binding.sampler_resolution->compare !=
                    SurfaceTextureCompare::none &&
                !supportsDepthComparisonSampling(
                    candidate_format)) {
                throw std::runtime_error(
                    "reloaded texture format is not a depth "
                    "format required by "
                    "its material sampler");
            }
        }
    }
}

void MaterialContainer::uploadTextureInPlace(GlobalTextureId texture,
                                             const LoadedImage &image) const {
    if (!textureShapeMatches(texture, image)) {
        throw std::runtime_error(
            "in-place texture upload requires identical "
            "dimension/extent/layers/format/mips");
    }
    // The logical image handle must stay unchanged, so there is no old image
    // to defer. Drain prior readers before writing the live allocation.
    GET_MODULE(VulkanManageCore).waitIdle();
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(image.levels.size());
    for (std::uint32_t mip = 0; mip < image.levels.size(); ++mip) {
        const auto &level = image.levels[mip];
        vk::BufferImageCopy copy;
        copy.bufferOffset = level.offset;
        copy.imageSubresource = {
            vk::ImageAspectFlagBits::eColor, mip, 0,
            image.dimension ==
                    LoadedImageDimension::ThreeD
                ? 1u
                : image.array_layers};
        copy.imageExtent = vk::Extent3D{
            level.width, level.height, level.depth};
        regions.push_back(copy);
    }
    GET_MODULE(VulkanUtils).safeTransferMemoryToImageLevels(
        textures.get(texture).image, image.pixels.data(), image.pixels.size(), regions,
        VulkanUtils::ImageTransferInfo{
            .old_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .new_layout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                         vk::PipelineStageFlagBits::eFragmentShader,
            .dst_access = vk::AccessFlagBits::eShaderRead});
}

std::vector<MaterialContainer::StagedMaterialDescriptor>
MaterialContainer::stageTextureRebind(
    GlobalTextureId texture, const InternalTextureResource &replacement) const {
    std::vector<StagedMaterialDescriptor> staged;
    const auto reverse = texture_materials.find(texture);
    if (reverse == texture_materials.end()) return staged;
    staged.reserve(reverse->second.size());
    for (const auto material_id : reverse->second) {
        const auto &material = materials.get(material_id);
        vk::DescriptorSetAllocateInfo allocation;
        allocation.descriptorPool = desc_pool.get();
        const auto layout = GET_MODULE(PipelineFactory).descriptorSetLayout(
            material.pipeline, imageDescriptorSetNumber);
        allocation.setSetLayouts({layout});
        auto descriptor = std::move(device.allocateDescriptorSetsUnique(allocation).front());

        std::vector<vk::DescriptorImageInfo> infos;
        std::vector<vk::WriteDescriptorSet> writes;
        infos.reserve(material.texture_bindings.size());
        writes.reserve(material.texture_bindings.size() + 1);
        for (const auto &binding : material.texture_bindings) {
            const auto &resource = binding.texture == texture
                                       ? replacement
                                       : textures.get(binding.texture);
            if (binding.srgb && !resource.srgb_view) {
                throw std::runtime_error("reloaded color texture does not provide an SRGB view");
            }
            infos.push_back(vk::DescriptorImageInfo{
                binding.sampler,
                binding.srgb ? resource.srgb_view.get() : resource.linear_view.get(),
                vk::ImageLayout::eShaderReadOnlyOptimal});
            vk::WriteDescriptorSet write;
            write.dstSet = descriptor.get();
            write.dstBinding = binding.binding;
            write.descriptorCount = 1;
            write.descriptorType = binding.descriptor_type;
            write.pImageInfo = &infos.back();
            writes.push_back(write);
        }

        vk::DescriptorBufferInfo buffer_info{material_buffer.buffer.get(), 0, vk::WholeSize};
        vk::WriteDescriptorSet buffer_write;
        buffer_write.dstSet = descriptor.get();
        buffer_write.dstBinding = materialBufferBinding;
        buffer_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        buffer_write.setBufferInfo(buffer_info);
        writes.push_back(buffer_write);
        device.updateDescriptorSets(writes, {});
        staged.push_back({material_id, std::move(descriptor)});
    }
    return staged;
}

MaterialContainer::RetiredTextureResources MaterialContainer::commitTextureRebind(
    GlobalTextureId texture, InternalTextureResource replacement,
    std::vector<StagedMaterialDescriptor> descriptors) {
    const auto reverse = texture_materials.find(texture);
    const auto expected = reverse == texture_materials.end() ? 0 : reverse->second.size();
    if (descriptors.size() != expected) {
        throw std::runtime_error("staged texture descriptor set is incomplete");
    }
    std::unordered_set<GlobalMaterialId, GlobalMaterialId::Hash> unique;
    for (const auto &descriptor : descriptors) {
        if (!descriptor.descriptor || !unique.insert(descriptor.material).second ||
            reverse == texture_materials.end() ||
            !reverse->second.contains(descriptor.material)) {
            throw std::runtime_error("staged texture descriptor set is invalid");
        }
        (void)materials.get(descriptor.material);
    }
    auto &slot = textures.get(texture);
    RetiredTextureResources retired{std::move(slot), {}};
    retired.descriptors.reserve(descriptors.size());
    slot = std::move(replacement);
    for (auto &descriptor : descriptors) {
        auto &material = materials.get(descriptor.material);
        retired.descriptors.push_back(std::move(material.descset));
        material.descset = std::move(descriptor.descriptor);
        ++material.descriptor_revision;
    }
    return retired;
}

size_t MaterialContainer::referencingMaterialCountForTesting(GlobalTextureId texture) const {
    const auto found = texture_materials.find(texture);
    return found == texture_materials.end() ? 0 : found->second.size();
}

size_t MaterialContainer::materialCapacityForTesting() const { return maxMaterials; }

bool MaterialContainer::handlesTextureReload(const watch::AssetKey &key) const {
    return texture_reload_handler && texture_reload_handler->handles(key);
}

bool MaterialContainer::enqueueTextureReload(const watch::ReloadRequest &request,
                                             watch::ReloadCoordinator &coordinator) {
    return texture_reload_handler && texture_reload_handler->enqueue(request, coordinator);
}

bool MaterialContainer::retireTextureReloadPayload(
    std::shared_ptr<const void> payload, watch::ReloadCoordinator &coordinator) noexcept {
    return texture_reload_handler &&
           texture_reload_handler->retire(std::move(payload), coordinator);
}

bool MaterialContainer::handlesMaterialValuesReload(const watch::AssetKey &key) const {
    return material_values_reload_handler && material_values_reload_handler->handles(key);
}

bool MaterialContainer::enqueueMaterialValuesReload(
    const watch::ReloadRequest &request, watch::ReloadCoordinator &coordinator) {
    return material_values_reload_handler &&
           material_values_reload_handler->enqueue(request, coordinator);
}

bool MaterialContainer::retireMaterialValuesReloadPayload(
    std::shared_ptr<const void> payload, watch::ReloadCoordinator &coordinator) noexcept {
    return material_values_reload_handler &&
           material_values_reload_handler->retire(std::move(payload), coordinator);
}

std::pair<vk::ImageView, vk::ImageView>
MaterialContainer::textureViewsForTesting(GlobalTextureId texture) const {
    const auto &resource = textures.get(texture);
    return {resource.linear_view.get(), resource.srgb_view.get()};
}

std::optional<ResolvedMaterialSampler>
MaterialContainer::materialSamplerResolutionForTesting(
    GlobalMaterialId material,
    std::string_view texture_name) const {
    const auto &resolutions =
        materials.get(material)
            .custom_sampler_resolutions;
    const auto found = std::find_if(
        resolutions.begin(), resolutions.end(),
        [&](const auto &entry) {
            return entry.first == texture_name;
        });
    return found == resolutions.end()
               ? std::nullopt
               : std::optional{found->second};
}

std::vector<uint8_t> MaterialContainer::texturePixelsForTesting(GlobalTextureId texture) const {
    const auto &resource = textures.get(texture);
    if (resource.dimension !=
        SurfaceTextureDimension::two_d) {
        throw std::runtime_error(
            "texture test readback only supports dimension 2d");
    }
    if (resource.image.format != vk::Format::eR8G8B8A8Unorm &&
        resource.image.format != vk::Format::eR8G8B8A8Srgb) {
        throw std::runtime_error("texture test readback only supports RGBA8");
    }
    const auto bytes = static_cast<vk::DeviceSize>(resource.image.extent.width) *
                       resource.image.extent.height * 4;
    auto &vkcore = GET_MODULE(VulkanManageCore);
    auto staging = vkcore.allocBuf(bytes, vk::BufferUsageFlagBits::eTransferDst,
                                   vma::MemoryUsage::eAutoPreferHost,
                                   vma::AllocationCreateFlagBits::eHostAccessRandom);
    auto &utils = GET_MODULE(VulkanUtils);
    utils.executeOneTimeCmd(
        [&](vk::CommandBuffer command) {
            utils.changeImageLayoutCmd(
                command, resource.image, vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::ImageLayout::eTransferSrcOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eVertexShader |
                              vk::PipelineStageFlagBits::eFragmentShader,
                 .dst_stage = vk::PipelineStageFlagBits::eTransfer,
                 .src_access = vk::AccessFlagBits::eShaderRead,
                 .dst_access = vk::AccessFlagBits::eTransferRead});
            vk::BufferImageCopy copy;
            copy.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
            copy.imageExtent = resource.image.extent;
            command.copyImageToBuffer(resource.image.image.get(),
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      staging.buffer.get(), copy);
            utils.changeImageLayoutCmd(
                command, resource.image, vk::ImageLayout::eTransferSrcOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                {.src_stage = vk::PipelineStageFlagBits::eTransfer,
                 .dst_stage = vk::PipelineStageFlagBits::eVertexShader |
                              vk::PipelineStageFlagBits::eFragmentShader,
                 .src_access = vk::AccessFlagBits::eTransferRead,
                 .dst_access = vk::AccessFlagBits::eShaderRead});
        },
        true);
    return vkcore.readBuf(staging, bytes);
}

std::vector<std::byte>
MaterialContainer::materialGpuValuesForTesting(GlobalMaterialId material) const {
    const auto &info = materials.get(material);
    const auto offset = sizeof(MaterialGpuData) * material.value +
                        offsetof(MaterialGpuData, custom_values);
    const auto bytes = GET_MODULE(VulkanManageCore).readBuf(
        material_buffer, offset + materialCustomValueCapacity);
    std::vector<std::byte> values(info.custom_values_layout.size);
    if (!values.empty()) {
        std::memcpy(values.data(), bytes.data() + offset, values.size());
    }
    return values;
}

std::vector<std::byte>
MaterialContainer::materialGpuRecordForTesting(GlobalMaterialId material) const {
    (void)materials.get(material);
    const auto offset = sizeof(MaterialGpuData) * material.value;
    const auto bytes = GET_MODULE(VulkanManageCore).readBuf(
        material_buffer, offset + sizeof(MaterialGpuData));
    std::vector<std::byte> record(sizeof(MaterialGpuData));
    std::memcpy(record.data(), bytes.data() + offset, record.size());
    return record;
}

static std::vector<MaterialPassRenderingBinding>
materialBindingsForGeneration(
    const RendererRuntimeGeneration &generation,
    MaterialRouteClass route,
    MaterialShaderContract shader_contract,
    const std::optional<std::string> &exact_pass) {
    std::vector<MaterialPassRenderingBinding> bindings;
    for (const auto rendering_pass_id :
         generation.rendering_pass_ids) {
        const auto *program =
            generation.find(rendering_pass_id);
        if (program == nullptr) {
            throw std::logic_error(
                "Render-pipeline candidate pass table is "
                "inconsistent");
        }
        for (const auto &compiled :
             program->rendering_pass.passes) {
            const auto &pass = compiled.definition;
            if (!pass.isMaterial() ||
                !materialPassAcceptsMaterial(
                    pass.materialInfo().contract,
                    pass.name, route,
                    shader_contract, exact_pass)) {
                continue;
            }

            std::vector<MaterialPassShaderInputBinding>
                shader_inputs;
            const auto input_attachment_index =
                [&](GlobalRenderTargetId target,
                    bool history,
                    const LogicalReadFootprint &footprint)
                -> std::optional<std::uint32_t> {
                if (history) {
                    return std::nullopt;
                }
                const auto target_position = std::find(
                    pass.input_targets.begin(),
                    pass.input_targets.end(), target);
                if (target_position ==
                    pass.input_targets.end()) {
                    throw std::runtime_error(
                        "material pass input is absent from "
                        "the positional physical input table: " +
                        pass.name);
                }
                const auto index =
                    static_cast<std::uint32_t>(
                        target_position -
                        pass.input_targets.begin());
                const auto local =
                    std::find(
                        compiled.rendering
                            .color_attachment_input_indices
                            .begin(),
                        compiled.rendering
                            .color_attachment_input_indices
                            .end(),
                        index) !=
                        compiled.rendering
                            .color_attachment_input_indices
                            .end() ||
                    compiled.rendering
                            .depth_attachment_input_index ==
                        index;
                if (!local) {
                    return std::nullopt;
                }
                if (footprint.kind !=
                    LogicalReadFootprintKind::same_pixel) {
                    throw std::logic_error(
                        "material pass selected a local "
                        "attachment for a non-same-pixel "
                        "input: " +
                        pass.name);
                }
                return index;
            };
            const auto append_input =
                [&](MaterialPassShaderInputKind kind,
                    std::string name,
                    GlobalRenderTargetId target,
                    bool history,
                    const LogicalReadFootprint &footprint,
                    std::optional<
                        ShaderResourcePortView>
                        resource_view =
                            std::nullopt) {
                    const auto target_position = std::find(
                        pass.input_targets.begin(),
                        pass.input_targets.end(), target);
                    auto view_dimension =
                        target_position !=
                                    pass.input_targets.end() &&
                                pass.input_target_views.size() ==
                                    pass.input_targets.size()
                            ? pass.input_target_views.at(
                                  static_cast<std::size_t>(
                                      target_position -
                                      pass.input_targets.begin()))
                            : PassInputViewDimension::shared_2d;
                    if (resource_view ==
                        ShaderResourcePortView::
                            family_array) {
                        view_dimension =
                            PassInputViewDimension::
                                family_2d_array;
                    } else if (
                        resource_view ==
                        ShaderResourcePortView::
                            shared_2d) {
                        view_dimension =
                            PassInputViewDimension::
                                shared_2d;
                    }
                    const auto attachment =
                        input_attachment_index(
                            target, history,
                            footprint);
                    const auto descriptor_dimension =
                        attachment
                            ? ImageSubresourceViewDimension::
                                  two_d
                        : resource_view ==
                                  ShaderResourcePortView::
                                      cube
                            ? ImageSubresourceViewDimension::
                                  cube
                        : view_dimension ==
                                  PassInputViewDimension::
                                      layered_2d_array ||
                              view_dimension ==
                                  PassInputViewDimension::
                                      family_2d_array
                            ? ImageSubresourceViewDimension::
                                  two_d_array
                            : ImageSubresourceViewDimension::
                                  two_d;
                    shader_inputs.push_back({
                        .input = {
                            .kind = kind,
                            .name = std::move(name),
                        },
                        .input_attachment_index =
                            attachment,
                        .view_dimension =
                            view_dimension,
                        .descriptor_dimension =
                            descriptor_dimension,
                    });
                };
            for (const auto &input :
                 pass.materialInfo().screen_inputs) {
                append_input(
                    MaterialPassShaderInputKind::
                        screen_input,
                    input.contract.name, input.target,
                    input.history,
                    input.contract.footprint);
            }
            for (const auto &resource :
                 pass.materialInfo()
                     .material_resources) {
                if (!resource.isImage()) {
                    continue;
                }
                append_input(
                    MaterialPassShaderInputKind::
                        material_resource,
                    resource.port.name,
                    resource.target,
                    resource.history,
                    resource.footprint,
                    resource.port.view);
            }

            bindings.push_back(
                MaterialPassRenderingBinding{
                    .pass_name = pass.name,
                    .rasterization_samples =
                        pass.rasterization_samples,
                    .rendering = compiled.rendering,
                    .output_schema =
                        pass.materialInfo()
                            .output_schema,
                    .output_states =
                        pass.materialInfo()
                            .output_states,
                    .shader_inputs =
                        std::move(shader_inputs),
                });
        }
    }
    return bindings;
}

static std::vector<MaterialPassShaderInputBinding>
resolveGenerationShaderInputs(
    std::span<const MaterialPassRenderingBinding> passes,
    std::span<const MaterialPassShaderInputRequest> inputs) {
    std::vector<MaterialPassShaderInputBinding> result;
    result.reserve(inputs.size());
    for (const auto &input : inputs) {
        result.push_back({
            .input = input,
            .input_attachment_index = std::nullopt,
            .view_dimension =
                PassInputViewDimension::shared_2d,
        });
    }
    if (passes.empty()) {
        return result;
    }

    for (std::size_t input_index = 0;
         input_index < inputs.size(); ++input_index) {
        std::optional<std::uint32_t> expected;
        auto expected_view =
            PassInputViewDimension::shared_2d;
        auto expected_descriptor_dimension =
            ImageSubresourceViewDimension::two_d;
        std::string expected_pass;
        bool initialized = false;
        for (const auto &pass : passes) {
            const auto found = std::find_if(
                pass.shader_inputs.begin(),
                pass.shader_inputs.end(),
                [&](const auto &candidate) {
                    return candidate.input ==
                           inputs[input_index];
                });
            if (found == pass.shader_inputs.end()) {
                throw std::runtime_error(
                    "material pass '" + pass.pass_name +
                    "' does not provide shader input '" +
                    inputs[input_index].name + "'");
            }
            if (!initialized) {
                expected =
                    found->input_attachment_index;
                expected_view =
                    found->view_dimension;
                expected_descriptor_dimension =
                    found->descriptor_dimension;
                expected_pass =
                    pass.pass_name;
                initialized = true;
                continue;
            }
            if (expected !=
                    found->input_attachment_index ||
                expected_view !=
                    found->view_dimension ||
                expected_descriptor_dimension !=
                    found->descriptor_dimension) {
                throw std::runtime_error(
                    "material shader input '" +
                    inputs[input_index].name +
                    "' resolves to different sampled/local-read ABIs "
                    "or image-view ABIs across render graph variants: '" +
                    expected_pass + "' (view " +
                    std::to_string(
                        static_cast<int>(
                            expected_view)) +
                    ", descriptor " +
                    std::string{
                        imageSubresourceViewDimensionName(
                            expected_descriptor_dimension)} +
                    ") and '" + pass.pass_name +
                    "' (view " +
                    std::to_string(
                        static_cast<int>(
                            found->view_dimension)) +
                    ", descriptor " +
                    std::string{
                        imageSubresourceViewDimensionName(
                            found->descriptor_dimension)} +
                    ")");
            }
        }
        result[input_index].input_attachment_index =
            expected;
        result[input_index].view_dimension =
            expected_view;
        result[input_index].descriptor_dimension =
            expected_descriptor_dimension;
    }
    return result;
}

void MaterialContainer::validateRuntimeGenerationCompatibility(
    const RendererRuntimeGeneration &generation) const {
    materials.forEach(
        [&](GlobalMaterialId material_id,
            const InternalMaterialInfo &material) {
            const auto bindings =
                materialBindingsForGeneration(
                    generation, material.route,
                material.shader_contract,
                material.exact_pass);
            if (bindings.empty()) {
                throw std::runtime_error(
                    "render-pipeline candidate has no compatible "
                    "pass for live material " +
                    std::to_string(material_id.value) +
                    " (route '" +
                    std::string{materialRouteClassName(
                        material.route)} +
                    "', shader contract '" +
                    std::string{materialShaderContractName(
                        material.shader_contract)} +
                    "')");
            }

            const MaterialPipelineRenderingContract live{
                .color_formats =
                    material.pipeline_color_formats,
                .depth_format =
                    material.pipeline_depth_format,
                .rasterization_samples =
                    material
                        .pipeline_rasterization_samples,
                .local_read =
                    material.pipeline_local_read,
                .output_states =
                    material.pipeline_output_states,
            };
            for (const auto &binding : bindings) {
                if (binding.output_schema !=
                    material.output_schema) {
                    const auto live_schema =
                        material.output_schema
                            ? "'" +
                                  material.output_schema
                                      ->name +
                                  "'"
                            : std::string{"<built-in>"};
                    const auto candidate_schema =
                        binding.output_schema
                            ? "'" +
                                  binding.output_schema->name +
                                  "'"
                            : std::string{"<built-in>"};
                    throw std::runtime_error(
                        "render-pipeline candidate pass '" +
                        binding.pass_name +
                        "' changes live material " +
                        std::to_string(material_id.value) +
                        " output schema from " +
                        live_schema + " to " +
                        candidate_schema +
                        "; reload the material surface "
                        "transactionally with the graph");
                }

                const auto candidate =
                    resolveMaterialPassRenderingBinding(
                        binding,
                        material.shader_contract);
                if (candidate != live) {
                    throw std::runtime_error(
                        "render-pipeline candidate pass '" +
                        binding.pass_name +
                        "' changes the physical rendering "
                        "contract of live material " +
                        std::to_string(material_id.value) +
                        "; color/depth formats, sample count, "
                        "local-read mapping, or attachment "
                        "state require a "
                        "transactional material pipeline "
                        "rebuild");
                }
            }
        });
}

MaterialRuntimeGenerationReloadPlan
MaterialContainer::prepareRuntimeGenerationReload(
    const RendererRuntimeGeneration &generation) {
    struct MetadataUpdate {
        GlobalMaterialId material;
        std::optional<MaterialOutputSchema>
            output_schema;
        MaterialPipelineRenderingContract rendering;
        std::vector<InternalMaterialInfo::PassInput>
            pass_inputs;
        std::vector<ShaderResourceInterfaceBinding>
            resource_interface;
        bool descriptor_abi_changed = false;
    };

    MaterialRuntimeGenerationReloadPlan plan;
    std::vector<MetadataUpdate> metadata_updates;
    auto &pipeline_factory =
        GET_MODULE(PipelineFactory);

    const auto append_shader_override =
        [&](SurfaceShaderReloadOverride candidate) {
            const auto found = std::find_if(
                plan.shader_overrides.begin(),
                plan.shader_overrides.end(),
                [&](const auto &existing) {
                    return existing.fragment ==
                           candidate.fragment;
                });
            if (found ==
                plan.shader_overrides.end()) {
                plan.shader_overrides.push_back(
                    std::move(candidate));
                return;
            }
            if (*found != candidate) {
                throw std::runtime_error(
                    "live materials sharing fragment shader " +
                    std::to_string(
                        candidate.fragment.value) +
                    " require incompatible render-graph "
                    "ABIs");
            }
        };
    const auto append_pipeline_override =
        [&](GraphicsPipelineReloadOverride candidate) {
            const auto found = std::find_if(
                plan.pipeline_overrides.begin(),
                plan.pipeline_overrides.end(),
                [&](const auto &existing) {
                    return existing.handle ==
                           candidate.handle;
                });
            if (found ==
                plan.pipeline_overrides.end()) {
                plan.pipeline_overrides.push_back(
                    std::move(candidate));
                return;
            }
            if (found->desc != candidate.desc) {
                throw std::runtime_error(
                    "live materials sharing pipeline " +
                    std::to_string(
                        candidate.handle.value) +
                    " require incompatible render-graph "
                    "contracts");
            }
        };

    materials.forEach(
        [&](GlobalMaterialId material_id,
            const InternalMaterialInfo &material) {
            const auto bindings =
                materialBindingsForGeneration(
                    generation, material.route,
                    material.shader_contract,
                    material.exact_pass);
            if (bindings.empty()) {
                throw std::runtime_error(
                    "render-pipeline candidate has no compatible "
                    "pass for live material " +
                    std::to_string(material_id.value) +
                    " (route '" +
                    std::string{materialRouteClassName(
                        material.route)} +
                    "', shader contract '" +
                    std::string{
                        materialShaderContractName(
                            material.shader_contract)} +
                    "')");
            }

            const auto candidate_schema =
                bindings.front().output_schema;
            auto candidate_rendering =
                resolveMaterialPassRenderingBinding(
                    bindings.front(),
                    material.shader_contract);
            for (std::size_t index = 1;
                 index < bindings.size(); ++index) {
                if (bindings[index].output_schema !=
                    candidate_schema) {
                    throw std::runtime_error(
                        "material route resolves to different "
                        "output schemas across render graph "
                        "variants: " +
                        bindings.front().pass_name + " and " +
                        bindings[index].pass_name);
                }
                const auto rendering =
                    resolveMaterialPassRenderingBinding(
                        bindings[index],
                        material.shader_contract);
                if (rendering !=
                    candidate_rendering) {
                    throw std::runtime_error(
                        "material route resolves to "
                        "pipeline-incompatible physical rendering "
                        "contracts: " +
                        bindings.front().pass_name + " and " +
                        bindings[index].pass_name);
                }
            }

            std::vector<MaterialPassShaderInputRequest>
                requested_inputs;
            requested_inputs.reserve(
                material.declared_screen_inputs.size() +
                material.declared_resource_ports.size());
            for (const auto &input :
                 material.declared_screen_inputs) {
                requested_inputs.push_back({
                    .kind =
                        MaterialPassShaderInputKind::
                            screen_input,
                    .name = input.name,
                });
            }
            std::vector<std::size_t>
                image_resource_indices;
            for (std::size_t index = 0;
                 index <
                 material.declared_resource_ports.size();
                 ++index) {
                const auto &resource =
                    material
                        .declared_resource_ports[index];
                if (resource.kind !=
                    SurfaceResourcePortKind::image) {
                    continue;
                }
                requested_inputs.push_back({
                    .kind =
                        MaterialPassShaderInputKind::
                            material_resource,
                    .name = resource.name,
                });
                image_resource_indices.push_back(index);
            }
            const auto physical_inputs =
                resolveGenerationShaderInputs(
                    bindings, requested_inputs);

            std::vector<std::string>
                current_physical_defines;
            std::vector<std::string>
                candidate_physical_defines;
            auto candidate_pass_inputs =
                material.pass_inputs;
            for (std::size_t index = 0;
                 index <
                 material.declared_screen_inputs.size();
                 ++index) {
                const auto &declared =
                    material.declared_screen_inputs[index];
                const auto current = std::find_if(
                    material.pass_inputs.begin(),
                    material.pass_inputs.end(),
                    [&](const auto &input) {
                        return input.contract.name ==
                               declared.name;
                    });
                const auto candidate = std::find_if(
                    candidate_pass_inputs.begin(),
                    candidate_pass_inputs.end(),
                    [&](const auto &input) {
                        return input.contract.name ==
                               declared.name;
                    });
                if (current ==
                        material.pass_inputs.end() ||
                    candidate ==
                        candidate_pass_inputs.end()) {
                    throw std::runtime_error(
                        "live material screen-input metadata is "
                        "incomplete for '" +
                        declared.name + "'");
                }
                if (current
                        ->input_attachment_index) {
                    current_physical_defines.push_back(
                        makeSurfaceScreenInputLocalReadDefine(
                            index,
                            *current
                                 ->input_attachment_index));
                }
                const auto attachment =
                    physical_inputs.at(index)
                        .input_attachment_index;
                candidate->descriptor_type =
                    attachment
                        ? vk::DescriptorType::
                              eInputAttachment
                        : vk::DescriptorType::
                              eCombinedImageSampler;
                candidate->input_attachment_index =
                    attachment;
                if (attachment) {
                    candidate_physical_defines.push_back(
                        makeSurfaceScreenInputLocalReadDefine(
                            index, *attachment));
                }
            }

            auto candidate_resource_interface =
                material.resource_interface;
            for (std::size_t position = 0;
                 position <
                 image_resource_indices.size();
                 ++position) {
                const auto resource_index =
                    image_resource_indices[position];
                const auto &declared =
                    material.declared_resource_ports
                        [resource_index];
                const auto current = std::find_if(
                    material.resource_interface.begin(),
                    material.resource_interface.end(),
                    [&](const auto &resource) {
                        return resource.port.name ==
                               declared.name;
                    });
                const auto candidate = std::find_if(
                    candidate_resource_interface.begin(),
                    candidate_resource_interface.end(),
                    [&](const auto &resource) {
                        return resource.port.name ==
                               declared.name;
                    });
                if (current ==
                        material.resource_interface.end() ||
                    candidate ==
                        candidate_resource_interface.end()) {
                    throw std::runtime_error(
                        "live material resource metadata is "
                        "incomplete for '" +
                        declared.name + "'");
                }
                if (current
                        ->input_attachment_index) {
                    current_physical_defines.push_back(
                        makeSurfaceResourceLocalReadDefine(
                            resource_index,
                            *current
                                 ->input_attachment_index));
                } else if (
                    current->image_view_dimension ==
                    ReflectedImageViewDimension::
                        cube) {
                    current_physical_defines.push_back(
                        makeSurfaceResourceCubeDefine(
                            resource_index));
                } else if (
                    current->image_view_dimension ==
                    ReflectedImageViewDimension::
                        two_d_array) {
                    current_physical_defines.push_back(
                        makeSurfaceResourceLayeredDefine(
                            resource_index));
                }
                const auto &physical =
                    physical_inputs.at(
                        material
                            .declared_screen_inputs
                            .size() +
                        position);
                const auto attachment =
                    physical.input_attachment_index;
                const auto cube =
                    !attachment &&
                    physical.descriptor_dimension ==
                        ImageSubresourceViewDimension::
                            cube;
                const auto layered =
                    !attachment && !cube &&
                    physical.descriptor_dimension ==
                        ImageSubresourceViewDimension::
                            two_d_array;
                candidate->descriptor =
                    attachment
                        ? ShaderResourceDescriptorKind::
                              input_attachment
                        : ShaderResourceDescriptorKind::
                              combined_image_sampler;
                candidate->input_attachment_index =
                    attachment;
                candidate->image_view_dimension =
                    cube
                        ? ReflectedImageViewDimension::
                              cube
                    : layered
                        ? ReflectedImageViewDimension::
                              two_d_array
                        : ReflectedImageViewDimension::
                              two_d;
                candidate->port.view =
                    cube
                        ? ShaderResourcePortView::
                              cube
                    : layered
                        ? ShaderResourcePortView::
                              family_array
                        : ShaderResourcePortView::
                              shared_2d;
                if (attachment) {
                    candidate_physical_defines.push_back(
                        makeSurfaceResourceLocalReadDefine(
                            resource_index,
                            *attachment));
                } else if (cube) {
                    candidate_physical_defines.push_back(
                        makeSurfaceResourceCubeDefine(
                            resource_index));
                } else if (layered) {
                    candidate_physical_defines.push_back(
                        makeSurfaceResourceLayeredDefine(
                            resource_index));
                }
            }

            auto pipeline_desc =
                pipeline_factory.graphicsDesc(
                    material.pipeline);
            if (!pipeline_desc.frag) {
                throw std::logic_error(
                    "material pipeline has no fragment shader");
            }
            if (candidate_schema !=
                    material.output_schema ||
                candidate_physical_defines !=
                    current_physical_defines) {
                append_shader_override({
                    .fragment =
                        *pipeline_desc.frag,
                    .physical_defines =
                        std::move(
                            candidate_physical_defines),
                    .material_output_schema =
                        candidate_schema,
                });
            }

            MaterialInfo validation_info;
            validation_info.output_schema =
                candidate_schema;
            validation_info.render_state =
                material.render_state;
            validation_info.vat = material.vat;
            validation_info.custom_textures.resize(
                material
                    .custom_sampler_resolutions
                    .size());
            validateMaterialCapabilities(
                validation_info,
                candidate_rendering);

            auto candidate_pipeline_desc =
                pipeline_desc;
            candidate_pipeline_desc.color_formats =
                candidate_rendering.color_formats;
            candidate_pipeline_desc.depth_format =
                candidate_rendering.depth_format;
            candidate_pipeline_desc
                .rasterization_samples =
                candidate_rendering
                    .rasterization_samples;
            candidate_pipeline_desc.local_read =
                candidate_rendering.local_read;
            candidate_pipeline_desc.resource_interface =
                candidate_resource_interface;
            candidate_pipeline_desc
                .color_attachment_states =
                candidate_rendering
                        .output_states.empty()
                    ? std::vector<
                          GraphicsPipelineColorAttachmentState>{}
                    : resolveMaterialColorAttachmentStates(
                          validation_info,
                          candidate_rendering);
            if (candidate_pipeline_desc !=
                pipeline_desc) {
                append_pipeline_override({
                    .handle = material.pipeline,
                    .desc = std::move(
                        candidate_pipeline_desc),
                });
            }

            const MaterialPipelineRenderingContract live{
                .color_formats =
                    material.pipeline_color_formats,
                .depth_format =
                    material.pipeline_depth_format,
                .rasterization_samples =
                    material
                        .pipeline_rasterization_samples,
                .local_read =
                    material.pipeline_local_read,
                .output_states =
                    material.pipeline_output_states,
            };
            const auto descriptor_abi_changed =
                candidate_pass_inputs !=
                    material.pass_inputs ||
                candidate_resource_interface !=
                    material.resource_interface;
            if (candidate_schema !=
                    material.output_schema ||
                candidate_rendering != live ||
                descriptor_abi_changed) {
                metadata_updates.push_back({
                    .material = material_id,
                    .output_schema =
                        candidate_schema,
                    .rendering =
                        std::move(
                            candidate_rendering),
                    .pass_inputs =
                        std::move(
                            candidate_pass_inputs),
                    .resource_interface =
                        std::move(
                            candidate_resource_interface),
                    .descriptor_abi_changed =
                        descriptor_abi_changed,
                });
            }
        });

    const auto overridden_pipelines =
        plan.pipeline_overrides;
    plan.commit =
        [this,
         updates = std::move(metadata_updates),
         overridden_pipelines]() mutable {
            const auto descriptor_abi_changed =
                std::any_of(
                    updates.begin(), updates.end(),
                    [](const auto &update) {
                        return update
                            .descriptor_abi_changed;
                    });
            if (descriptor_abi_changed) {
                GET_MODULE(VulkanManageCore)
                    .waitIdle();
            }
            for (auto &update : updates) {
                auto &material =
                    materials.get(update.material);
                material.output_schema =
                    std::move(
                        update.output_schema);
                material.pipeline_color_formats =
                    std::move(
                        update.rendering
                            .color_formats);
                material.pipeline_depth_format =
                    update.rendering.depth_format;
                material
                    .pipeline_rasterization_samples =
                    update.rendering
                        .rasterization_samples;
                material.pipeline_local_read =
                    std::move(
                        update.rendering.local_read);
                material.pipeline_output_states =
                    std::move(
                        update.rendering
                            .output_states);
                material.pass_inputs =
                    std::move(update.pass_inputs);
                material.resource_interface =
                    std::move(
                        update.resource_interface);
                if (update.descriptor_abi_changed) {
                    material
                        .screen_input_descriptors
                        .clear();
                    ++material.descriptor_revision;
                }
            }
            std::erase_if(
                pipelines,
                [&](const auto &entry) {
                    return std::any_of(
                        overridden_pipelines.begin(),
                        overridden_pipelines.end(),
                        [&](const auto &override) {
                            return entry.second ==
                                   override.handle;
                        });
                });
        };
    return plan;
}

bool MaterialContainer::isRenderRequired(const PassDefinition &pass,
                                         GlobalMaterialId material_id) const {
    if (!pass.isMaterial()) return false;
    if (pass.materialInfo().material_variant) {
        if (!pass.materialInfo().material_filter) {
            throw std::runtime_error(
                "material variant pass '" + pass.name +
                "' has no material_filter");
        }
        const auto &base = materials.get(material_id);
        if (!materialDrawTagFilterMatches(
                base.tags,
                *pass.materialInfo().material_filter)) {
            return false;
        }
    }
    const auto effective =
        resolveMaterialForPass(pass, material_id);
    const auto &material = materials.get(effective);
    const auto contract = pass.materialInfo().contract;
    return materialPassAcceptsMaterial(contract, pass.name, material.route,
                                       material.shader_contract, material.exact_pass) &&
           pass.materialInfo().output_schema ==
               material.output_schema;
}

GlobalMaterialId MaterialContainer::resolveMaterialForPass(
    const PassDefinition &pass, GlobalMaterialId material) const {
    if (!pass.isMaterial() ||
        !pass.materialInfo().material_variant) {
        (void)materials.get(material);
        return material;
    }
    const auto &name =
        *pass.materialInfo().material_variant;
    const auto &base = materials.get(material);
    const auto found = base.variants.find(name);
    if (found == base.variants.end()) {
        throw std::runtime_error(
            "material pass '" + pass.name +
            "' selected variant '" + name +
            "' but material " +
            std::to_string(material.value) +
            " does not provide it");
    }
    return found->second;
}

std::uint32_t MaterialContainer::materialGpuIndexForPass(
    const PassDefinition &pass, GlobalMaterialId material) const {
    const auto effective =
        resolveMaterialForPass(pass, material);
    if (effective.value < 0) {
        throw std::runtime_error(
            "material pass resolved an invalid GPU material index");
    }
    return static_cast<std::uint32_t>(effective.value);
}

static std::string makeScreenInputPassKey(const PassDefinition &pass) {
    std::ostringstream key;
    key << pass.name << ':' << static_cast<int>(pass.materialInfo().contract);
    for (const auto &input : pass.materialInfo().screen_inputs) {
        key << ':' << input.contract.name << '=' << input.target.value
            << (input.history ? "@history" : "");
    }
    for (const auto &input :
         pass.materialInfo().surface_resources) {
        key << ":surface:" << input.contract.name << '='
            << input.target.value
            << (input.history ? "@history" : "");
    }
    for (const auto &resource :
         pass.materialInfo().material_resources) {
        key << ":resource:" << resource.port.name
            << '=';
        if (resource.isBuffer()) {
            key << "buffer:" << resource.buffer
                << '#' << resource.buffer_id.value;
        } else {
            key << "image:" << resource.target.value
                << (resource.history ? "@history"
                                     : "");
        }
        key << ":access="
            << static_cast<int>(
                   resource.port.access)
            << ":view="
            << static_cast<int>(resource.port.view)
            << ":filter="
            << static_cast<int>(
                   resource.port.sampling.filter)
            << ":address="
            << static_cast<int>(
                   resource.port.sampling.address_mode);
        if (resource.port.subresource) {
            const auto &subresource =
                *resource.port.subresource;
            key << ":mip="
                << subresource.base_mip_level
                << ":mip_count=";
            if (subresource.mip_count_mode ==
                ImageSubresourceMipCountMode::
                    remaining) {
                key << "remaining";
            } else {
                key << subresource.level_count;
            }
            key << ":layer="
                << subresource.base_array_layer
                << ":layer_count="
                << subresource.layer_count;
        }
    }
    for (const auto view : pass.input_target_views) {
        key << ":view=" << static_cast<int>(view);
    }
    return key.str();
}

static std::optional<std::uint32_t>
materialPassInputAttachmentIndex(
    const GraphicsPipelineRenderingLocalReadContract
        &local_read,
    const PassDefinition &pass,
    GlobalRenderTargetId target,
    bool history) {
    if (!local_read.enabled || history) {
        return std::nullopt;
    }
    const auto position = std::find(
        pass.input_targets.begin(),
        pass.input_targets.end(), target);
    if (position == pass.input_targets.end()) {
        throw std::runtime_error(
            "material pass input target is absent from pass '" +
            pass.name + "'");
    }
    const auto input_index =
        static_cast<std::uint32_t>(
            position - pass.input_targets.begin());
    const auto color =
        std::find(
            local_read
                .color_attachment_input_indices.begin(),
            local_read
                .color_attachment_input_indices.end(),
            input_index) !=
        local_read
            .color_attachment_input_indices.end();
    if (color ||
        local_read.depth_attachment_input_index ==
            input_index) {
        return input_index;
    }
    return std::nullopt;
}

MaterialContainer::InternalMaterialInfo::ScreenInputDescriptor
MaterialContainer::buildScreenInputDescriptor(
    PipelineHandle pipeline,
    std::vector<InternalMaterialInfo::ScreenInputResource> resources,
    const RenderTargetImageViewResolver &rt_views) const {
    if (resources.empty()) return {};
    if (resources.size() >
        maxMaterialPassDescriptors) {
        throw std::runtime_error(
            "material has too many pass input descriptors");
    }

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto &reflection =
        pipeline_factory.reflection(pipeline);
    const auto reflected_count =
        std::count_if(
            reflection.bindings.begin(),
            reflection.bindings.end(),
            [](const auto &binding) {
                return binding.set ==
                       PELICAN_SET_PASS_INPUT;
            });
    if (reflected_count != resources.size()) {
        throw std::runtime_error(
            "material pass input resources do not cover shader reflection");
    }
    for (const auto &resource : resources) {
        const auto found = std::find_if(
            reflection.bindings.begin(),
            reflection.bindings.end(),
            [&](const auto &binding) {
                return binding.set ==
                           PELICAN_SET_PASS_INPUT &&
                       binding.binding ==
                           resource.binding;
            });
        if (found == reflection.bindings.end() ||
            found->count != 1 ||
            found->type !=
                resource.descriptor_type) {
            throw std::runtime_error(
                "material pass input resource '" +
                resource.name +
                "' does not match shader reflection");
        }
    }
    const auto layout = pipeline_factory.descriptorSetLayout(
        pipeline, PELICAN_SET_PASS_INPUT);

    InternalMaterialInfo::ScreenInputDescriptor result;
    result.resources = std::move(resources);
    result.binding_revision = next_screen_input_binding_revision++;
    std::uint32_t variant_count = 1;
    for (const auto &resource : result.resources) {
        if (resource.isImage() &&
            !isConcreteRenderTarget(resource.target)) {
            throw std::runtime_error(
                "material image input must resolve to a render target: " +
                resource.name);
        }
        if (resource.isInputAttachment() &&
            resource.history) {
            throw std::runtime_error(
                "material input attachment cannot read history: " +
                resource.name);
        }
        if (resource.isInputAttachment() &&
            resource.subresource) {
            throw std::runtime_error(
                "material input attachment cannot select an image "
                "subresource: " +
                resource.name);
        }
        if (resource.isInputAttachment() &&
            resource.descriptor_dimension ==
                ImageSubresourceViewDimension::cube) {
            throw std::runtime_error(
                "material input attachment cannot use a cube image "
                "view: " +
                resource.name);
        }
        if (resource.isImage() &&
            resource.descriptor_dimension ==
                ImageSubresourceViewDimension::cube &&
            rt_views.dimension(resource.target) !=
                ImageResourceDimension::cube) {
            throw std::runtime_error(
                "material cube input requires a cube render target: " +
                resource.name);
        }
        if (resource.isImage() &&
            resource.descriptor_dimension ==
                ImageSubresourceViewDimension::cube &&
            resource.subresource &&
            (resource.subresource->base_array_layer != 0 ||
             resource.subresource->layer_count != 6)) {
            throw std::runtime_error(
                "material cube input subresource must select all six "
                "faces: " +
                resource.name);
        }
        if (resource.isImage() &&
            resource.subresource &&
            !validImageSubresourceRange(
                *resource.subresource,
                rt_views.mipLevels(
                    resource.target),
                rt_views.arrayLayers(
                    resource.target))) {
            throw std::runtime_error(
                "material image input has an out-of-range "
                "subresource: " +
                resource.name);
        }
        if (resource.isBuffer() &&
            !GET_MODULE(FrameGraphResourceContainer)
                 .hasBuffer(resource.buffer)) {
            throw std::runtime_error(
                "material buffer input is absent from its GPU generation: " +
                resource.name);
        }
        if (resource.isImage() &&
            resource.view_dimension !=
                PassInputViewDimension::shared_2d &&
            resource.view_dimension !=
                PassInputViewDimension::
                    family_2d_array &&
            !(resource.isInputAttachment() &&
              resource.view_dimension ==
                  PassInputViewDimension::
                      layered_2d_array)) {
            variant_count = std::max(
                variant_count,
                rt_views.arrayLayers(resource.target));
        }
    }
    for (const auto &resource : result.resources) {
        if (resource.isImage() &&
            resource.view_dimension !=
                PassInputViewDimension::shared_2d &&
            resource.view_dimension !=
                PassInputViewDimension::
                    family_2d_array &&
            !(resource.isInputAttachment() &&
              resource.view_dimension ==
                  PassInputViewDimension::
                      layered_2d_array) &&
            rt_views.arrayLayers(resource.target) !=
                variant_count) {
            throw std::runtime_error(
                "material sequential screen inputs have inconsistent "
                "array-layer counts");
        }
    }
    result.variants.resize(variant_count);
    const auto has_material_resource =
        std::any_of(
            result.resources.begin(),
            result.resources.end(),
            [](const auto &resource) {
                return resource.material_resource;
            });
    if (has_material_resource &&
        !material_resource_desc_pool) {
        material_resource_desc_pool =
            createMaterialResourceDescriptorPool(
                device);
    }
    const auto descriptor_pool =
        has_material_resource
            ? material_resource_desc_pool.get()
            : screen_input_desc_pool.get();
    for (std::uint32_t variant = 0;
         variant < variant_count; ++variant) {
        for (uint32_t parity = 0; parity < 2; ++parity) {
            auto &descriptor_variant =
                result.variants[variant];
            vk::DescriptorSetAllocateInfo allocation;
            allocation.descriptorPool =
                descriptor_pool;
            allocation.descriptorSetCount = 1;
            allocation.pSetLayouts = &layout;
            descriptor_variant.descsets[parity] =
                std::move(device.allocateDescriptorSetsUnique(allocation).front());

            std::vector<vk::DescriptorImageInfo> image_infos;
            std::vector<vk::DescriptorBufferInfo> buffer_infos;
            std::vector<vk::WriteDescriptorSet> writes;
            image_infos.reserve(result.resources.size());
            buffer_infos.reserve(result.resources.size());
            writes.reserve(result.resources.size());
            descriptor_variant.bound_image_views[parity]
                .reserve(result.resources.size());
            for (const auto &resource :
                 result.resources) {
                if (resource.isBuffer()) {
                    buffer_infos.push_back(
                        GET_MODULE(
                            FrameGraphResourceContainer)
                            .descriptorInfo(
                                resource.buffer));
                    vk::WriteDescriptorSet write{
                        descriptor_variant
                            .descsets[parity]
                            .get(),
                        resource.binding, 0, 1,
                        vk::DescriptorType::
                            eStorageBuffer};
                    write.pBufferInfo =
                        &buffer_infos.back();
                    writes.push_back(write);
                    continue;
                }
                const auto array_view =
                    resource.descriptor_dimension ==
                    ImageSubresourceViewDimension::
                        two_d_array;
                const auto cube_view =
                    resource.descriptor_dimension ==
                    ImageSubresourceViewDimension::cube;
                vk::ImageView image_view;
                if (resource.subresource) {
                    auto subresource =
                        *resource.subresource;
                    if (array_view &&
                        resource.view_dimension ==
                            PassInputViewDimension::
                                family_2d_array &&
                        subresource
                                .base_array_layer == 0 &&
                        subresource.layer_count == 1) {
                        subresource.layer_count =
                            rt_views.arrayLayers(
                                resource.target);
                    } else if (!array_view &&
                        !cube_view &&
                        resource.view_dimension !=
                            PassInputViewDimension::
                                shared_2d) {
                        subresource
                            .base_array_layer +=
                            variant;
                    }
                    image_view =
                        rt_views
                            .getImageSubresourceViewForFrame(
                                resource.target,
                                subresource,
                                resource
                                    .descriptor_dimension,
                                resource.history,
                                parity);
                } else if (cube_view) {
                    image_view =
                        rt_views
                            .getImageSubresourceViewForFrame(
                                resource.target,
                                ImageSubresourceRange{
                                    .base_mip_level = 0,
                                    .level_count =
                                        rt_views.mipLevels(
                                            resource.target),
                                    .base_array_layer = 0,
                                    .layer_count = 6,
                                },
                                ImageSubresourceViewDimension::
                                    cube,
                                resource.history,
                                parity);
                } else if (
                    array_view &&
                    rt_views.arrayLayers(
                        resource.target) == 1) {
                    image_view =
                        rt_views
                            .getImageSubresourceViewForFrame(
                                resource.target,
                                ImageSubresourceRange{
                                    .base_mip_level = 0,
                                    .level_count =
                                        rt_views.mipLevels(
                                            resource.target),
                                    .base_array_layer = 0,
                                    .layer_count = 1,
                                },
                                ImageSubresourceViewDimension::
                                    two_d_array,
                                resource.history,
                                parity);
                } else if (array_view) {
                    image_view =
                        rt_views
                            .getLayeredImageViewForFrame(
                                resource.target,
                                resource.history,
                                parity);
                } else if (
                    resource.view_dimension ==
                    PassInputViewDimension::
                        shared_2d) {
                    image_view =
                        rt_views
                            .getImageViewForFrame(
                                resource.target,
                                resource.history,
                                parity);
                } else {
                    image_view =
                        rt_views
                            .getImageLayerViewForFrame(
                                resource.target,
                                variant,
                                resource.history,
                                parity);
                }
                const auto sampler =
                    resource.isInputAttachment()
                        ? vk::Sampler{}
                    : resource.material_resource
                        ? materialResourceSampler(
                              resource.sampling)
                    : resource.sampling.address_mode ==
                              ShaderResourcePortAddressMode::
                                  clamp_to_edge
                        ? resource.sampling.filter ==
                                  ShaderResourcePortFilter::
                                      nearest
                              ? screen_nearest_sampler.get()
                              : screen_linear_sampler.get()
                        : materialResourceSampler(
                              resource.sampling);
                image_infos.push_back(vk::DescriptorImageInfo{
                    sampler,
                    image_view,
                    resource.isInputAttachment()
                        ? vk::ImageLayout::
                              eRenderingLocalReadKHR
                        : vk::ImageLayout::
                              eShaderReadOnlyOptimal});
                descriptor_variant.bound_image_views[parity]
                    .push_back(image_view);
                vk::WriteDescriptorSet write{
                    descriptor_variant.descsets[parity].get(),
                    resource.binding, 0, 1,
                    resource.descriptor_type};
                write.pImageInfo = &image_infos.back();
                writes.push_back(write);
            }
            device.updateDescriptorSets(writes, {});
        }
    }
    return result;
}

const MaterialContainer::InternalMaterialInfo::ScreenInputDescriptor *
MaterialContainer::ensureScreenInputDescriptor(
    GlobalMaterialId material_id, const PassDefinition &pass) const {
    const auto &material = materials.get(material_id);
    if (material.pass_inputs.empty() &&
        material.resource_interface.empty()) {
        return nullptr;
    }
    if (!pass.isMaterial() ||
        !materialPassAcceptsMaterial(pass.materialInfo().contract, pass.name,
                                     material.route, material.shader_contract,
                                     material.exact_pass) ||
        pass.materialInfo().output_schema !=
            material.output_schema) {
        throw std::runtime_error(
            "material screen inputs requested for an incompatible pass: " +
            pass.name);
    }

    const auto key = makeScreenInputPassKey(pass);
    if (const auto found = material.screen_input_descriptors.find(key);
        found != material.screen_input_descriptors.end()) {
        return &found->second;
    }

    std::vector<InternalMaterialInfo::ScreenInputResource> resources;
    resources.reserve(
        material.pass_inputs.size() +
        material.resource_interface.size());
    const RenderTargetImageViewResolver rt_views{
        GET_MODULE(RenderTargetContainer)};
    std::uint32_t screen_binding = 0;
    for (const auto &required : material.pass_inputs) {
        const auto find_binding =
            [&](const auto &bindings)
            -> const MaterialPassInputBinding * {
            const auto found = std::find_if(
                bindings.begin(), bindings.end(),
                [&](const auto &candidate) {
                    return candidate.contract.name ==
                           required.contract.name;
                });
            return found == bindings.end()
                       ? nullptr
                       : &*found;
        };
        const auto *binding =
            find_binding(
                pass.materialInfo().screen_inputs);
        if (binding == nullptr) {
            binding = find_binding(
                pass.materialInfo()
                    .surface_resources);
        }
        if (binding == nullptr) {
            throw std::runtime_error(
                "material pass input '" +
                required.contract.name +
                "' is not provided by pass '" + pass.name +
                "' (fallback=" +
                std::string{
                    materialPassInputFallbackName(
                        required.contract.fallback)} +
                ")");
        }
        if (binding->contract != required.contract) {
            throw std::runtime_error(
                "material pass input '" +
                required.contract.name +
                "' type or footprint does not match pass '" + pass.name +
                "'");
        }
        const auto input_attachment_index =
            materialPassInputAttachmentIndex(
                material.pipeline_local_read,
                pass, binding->target,
                binding->history);
        const auto expected_descriptor =
            input_attachment_index
                ? vk::DescriptorType::
                      eInputAttachment
                : vk::DescriptorType::
                      eCombinedImageSampler;
        if (input_attachment_index) {
            validateMaterialInputAttachmentFormat(
                required.contract.name,
                binding->target);
        }
        if (required.descriptor_type !=
                expected_descriptor ||
            required.input_attachment_index !=
                input_attachment_index) {
            throw std::runtime_error(
                "material pass input '" +
                required.contract.name +
                "' shader sampled/local-read ABI does not match "
                "pass '" +
                pass.name + "'");
        }
        auto view_dimension =
            PassInputViewDimension::shared_2d;
        const auto target_position = std::find(
            pass.input_targets.begin(),
            pass.input_targets.end(),
            binding->target);
        if (target_position != pass.input_targets.end() &&
            pass.input_target_views.size() ==
                pass.input_targets.size()) {
            view_dimension =
                pass.input_target_views.at(
                    static_cast<std::size_t>(
                        target_position -
                        pass.input_targets.begin()));
        }
        if (required.contract.view_policy ==
                MaterialPassInputViewPolicy::shared_2d &&
            view_dimension !=
                PassInputViewDimension::shared_2d) {
            throw std::runtime_error(
                "material pass input '" +
                required.contract.name +
                "' requires shared_2d view policy in pass '" +
                pass.name + "'");
        }
        if (required.contract.view_policy ==
                MaterialPassInputViewPolicy::
                    family_array &&
            view_dimension !=
                PassInputViewDimension::
                    family_2d_array) {
            throw std::runtime_error(
                "material pass input '" +
                required.contract.name +
                "' requires family_array view policy in pass '" +
                pass.name + "'");
        }
        resources.push_back(
            InternalMaterialInfo::ScreenInputResource{
                .name = required.contract.name,
                .binding = screen_binding++,
                .descriptor_type =
                    required.descriptor_type,
                .target = binding->target,
                .history = binding->history,
                .view_dimension =
                    view_dimension,
                .descriptor_dimension =
                    view_dimension ==
                                PassInputViewDimension::
                                    family_2d_array ||
                            (required.descriptor_type ==
                                 vk::DescriptorType::
                                     eInputAttachment &&
                             view_dimension ==
                                 PassInputViewDimension::
                                     layered_2d_array)
                        ? ImageSubresourceViewDimension::
                              two_d_array
                        : ImageSubresourceViewDimension::
                              two_d,
                // Preserve the established screen-input sampler
                // behavior. The historical linear_repeat label used
                // the clamp sampler at runtime.
                .sampling =
                    ShaderResourcePortSampling{
                        required.contract.sampling ==
                                MaterialPassInputSampling::
                                    nearest_clamp_to_edge
                            ? ShaderResourcePortFilter::
                                  nearest
                            : ShaderResourcePortFilter::
                                  linear,
                        ShaderResourcePortAddressMode::
                            clamp_to_edge,
                    },
            });
    }

    for (const auto &required :
         material.resource_interface) {
        const auto binding = std::find_if(
            pass.materialInfo()
                .material_resources.begin(),
            pass.materialInfo()
                .material_resources.end(),
            [&](const auto &candidate) {
                return candidate.port.name ==
                       required.port.name;
            });
        if (binding ==
            pass.materialInfo()
                .material_resources.end()) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' is not provided by pass '" +
                pass.name + "'");
        }

        if (required.descriptor ==
            ShaderResourceDescriptorKind::
                storage_buffer) {
            if (!binding->isBuffer() ||
                !isValidFrameGraphBufferId(
                    binding->buffer_id)) {
                throw std::runtime_error(
                    "material resource port '" +
                    required.port.name +
                    "' requires a generation-pinned buffer in pass '" +
                    pass.name + "'");
            }
            resources.push_back(
                InternalMaterialInfo::
                    ScreenInputResource{
                        .name =
                            required.port.name,
                        .binding =
                            required.binding,
                        .descriptor_type =
                            vk::DescriptorType::
                                eStorageBuffer,
                        .buffer =
                            binding->buffer_id,
                        .material_resource =
                            true,
                    });
            continue;
        }
        const auto input_attachment =
            required.descriptor ==
            ShaderResourceDescriptorKind::
                input_attachment;
        if ((required.descriptor !=
                 ShaderResourceDescriptorKind::
                     combined_image_sampler &&
             !input_attachment) ||
            !binding->isImage()) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' requires an image in pass '" +
                pass.name + "'");
        }
        const auto input_attachment_index =
            materialPassInputAttachmentIndex(
                material.pipeline_local_read,
                pass, binding->target,
                binding->history);
        if (input_attachment !=
                input_attachment_index.has_value() ||
            required.input_attachment_index !=
                input_attachment_index) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' shader sampled/local-read ABI does not match "
                "pass '" +
                pass.name + "'");
        }
        if (input_attachment &&
            binding->footprint.kind !=
                LogicalReadFootprintKind::
                    same_pixel) {
            throw std::logic_error(
                "material resource port '" +
                required.port.name +
                "' uses an input attachment without a same-pixel "
                "contract");
        }
        if (input_attachment) {
            validateMaterialInputAttachmentFormat(
                required.port.name,
                binding->target);
        }

        auto view_dimension =
            PassInputViewDimension::shared_2d;
        const auto target_position = std::find(
            pass.input_targets.begin(),
            pass.input_targets.end(),
            binding->target);
        if (target_position !=
                pass.input_targets.end() &&
            pass.input_target_views.size() ==
                pass.input_targets.size()) {
            view_dimension =
                pass.input_target_views.at(
                    static_cast<std::size_t>(
                        target_position -
                        pass.input_targets.begin()));
        }
        auto descriptor_dimension =
            !input_attachment &&
                    (view_dimension ==
                         PassInputViewDimension::
                             layered_2d_array ||
                     view_dimension ==
                         PassInputViewDimension::
                             family_2d_array)
                ? ImageSubresourceViewDimension::
                      two_d_array
                : ImageSubresourceViewDimension::two_d;
        if (binding->port.view ==
                ShaderResourcePortView::
                    family_array &&
            view_dimension ==
                PassInputViewDimension::
                    shared_2d) {
            // One-view families may use the scalar physical target
            // representation. Adapt that image to a one-layer array view so
            // one material shader ABI remains valid in both the secondary
            // family and a layered main family.
            descriptor_dimension =
                ImageSubresourceViewDimension::
                    two_d_array;
        }
        if (binding->port.view ==
            ShaderResourcePortView::cube) {
            if (input_attachment) {
                throw std::runtime_error(
                    "material resource port '" +
                    required.port.name +
                    "' cube views cannot use input attachments in pass '" +
                    pass.name + "'");
            }
            if (rt_views.dimension(
                    binding->target) !=
                ImageResourceDimension::cube) {
                throw std::runtime_error(
                    "material resource port '" +
                    required.port.name +
                    "' requires a cube render target in pass '" +
                    pass.name + "'");
            }
            if (binding->port.subresource &&
                (binding->port.subresource
                         ->base_array_layer != 0 ||
                 binding->port.subresource
                         ->layer_count != 6)) {
                throw std::runtime_error(
                    "material resource port '" +
                    required.port.name +
                    "' cube subresource must select all six faces in pass '" +
                    pass.name + "'");
            }
            descriptor_dimension =
                ImageSubresourceViewDimension::cube;
        }
        const auto expected_shader_view =
            descriptor_dimension ==
                    ImageSubresourceViewDimension::cube
                ? ReflectedImageViewDimension::cube
            : !input_attachment &&
                    descriptor_dimension ==
                        ImageSubresourceViewDimension::
                            two_d_array
                ? ReflectedImageViewDimension::
                      two_d_array
                : ReflectedImageViewDimension::two_d;
        if (required.image_view_dimension !=
            expected_shader_view) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' shader image-view ABI does not match pass '" +
                pass.name + "'");
        }
        if (binding->port.view ==
                ShaderResourcePortView::shared_2d &&
            view_dimension !=
                PassInputViewDimension::shared_2d) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' requires shared_2d in pass '" +
                pass.name + "'");
        }
        if (binding->port.view ==
                ShaderResourcePortView::per_view &&
            view_dimension ==
                PassInputViewDimension::shared_2d) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' requires per_view in pass '" +
                pass.name + "'");
        }
        if (binding->port.view ==
                ShaderResourcePortView::
                    family_array &&
            view_dimension !=
                    PassInputViewDimension::
                        family_2d_array &&
            view_dimension !=
                    PassInputViewDimension::
                        shared_2d) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' requires family_array in pass '" +
                pass.name + "'");
        }
        if (binding->port.view !=
                ShaderResourcePortView::
                    family_array &&
            binding->port.view !=
                ShaderResourcePortView::cube &&
            view_dimension ==
                PassInputViewDimension::
                    family_2d_array) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' must declare family_array in pass '" +
                pass.name + "'");
        }
        if (view_dimension ==
            PassInputViewDimension::
                layered_2d_array &&
            !input_attachment) {
            throw std::runtime_error(
                "material resource port '" +
                required.port.name +
                "' currently requires sequential or shared 2D views; "
                "layered multiview needs an array accessor");
        }
        resources.push_back(
            InternalMaterialInfo::
                ScreenInputResource{
                    .name =
                        required.port.name,
                    .binding =
                        required.binding,
                    .descriptor_type =
                        input_attachment
                            ? vk::DescriptorType::
                                  eInputAttachment
                            : vk::DescriptorType::
                                  eCombinedImageSampler,
                    .target =
                        binding->target,
                    .history =
                        binding->history,
                    .view_dimension =
                        view_dimension,
                    .descriptor_dimension =
                        descriptor_dimension,
                    .sampling =
                        binding->port.sampling,
                    .subresource =
                        binding->port
                            .subresource,
                    .material_resource =
                        true,
                });
    }

    auto [inserted, unused] = material.screen_input_descriptors.emplace(
        key, buildScreenInputDescriptor(material.pipeline, std::move(resources),
                                        rt_views));
    (void)unused;
    return &inserted->second;
}

void MaterialContainer::bindResource(vk::CommandBuffer cmd_buf, PassId pass_id,
                                     const PassDefinition &pass,
                                     GlobalMaterialId material_id,
                                     GlobalMaterialId prev_material_id,
                                     RenderPassViewInvocation invocation) const {
    (void)pass_id;
    material_id =
        resolveMaterialForPass(pass, material_id);
    if (isValidMaterialId(prev_material_id)) {
        prev_material_id =
            resolveMaterialForPass(pass, prev_material_id);
    }
    const auto &material = materials.get(material_id);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline_layout = pipeline_factory.layout(material.pipeline);

    if (!isValidMaterialId(prev_material_id)) {
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(material.pipeline));
        cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout, imageDescriptorSetNumber,
                                   {material.descset.get()}, {});
    } else {
        const auto &prev_material = materials.get(prev_material_id);
        if (material.pipeline != prev_material.pipeline)
            cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_factory.pipeline(material.pipeline));
        if (material.descset.get() != prev_material.descset.get())
            cmd_buf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline_layout,
                                       imageDescriptorSetNumber, {material.descset.get()}, {});
    }

    if (const auto *screen_inputs =
            ensureScreenInputDescriptor(material_id, pass)) {
        const auto parity = GET_MODULE(RenderTargetContainer).historyFrameIndex();
        std::size_t variant = 0;
        if (screen_inputs->variants.size() > 1) {
            if (invocation.logical_view_count !=
                    screen_inputs->variants.size() ||
                invocation.view_index >=
                screen_inputs->variants.size()) {
                throw std::runtime_error(
                    "material screen-input descriptor invocation does not "
                    "match its layered inputs");
            }
            variant = invocation.view_index;
        }
        cmd_buf.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, pipeline_layout,
            PELICAN_SET_PASS_INPUT,
            {screen_inputs->variants[variant]
                 .descsets[parity]
                 .get()},
            {});
    }
}

void MaterialContainer::rebindScreenInputs(
    const RenderTargetImageViewResolver &rt_views) const {
    for (int value = 0; value < static_cast<int>(maxMaterials); ++value) {
        const auto material_id = GlobalMaterialId{value};
        if (!materials.contains(material_id)) continue;
        const auto &material = materials.get(material_id);
        auto descriptor =
            material.screen_input_descriptors.begin();
        while (descriptor !=
               material.screen_input_descriptors.end()) {
            const auto stale_buffer =
                std::any_of(
                    descriptor->second.resources.begin(),
                    descriptor->second.resources.end(),
                    [](const auto &resource) {
                        return resource.isBuffer() &&
                               !GET_MODULE(
                                    FrameGraphResourceContainer)
                                    .hasBuffer(
                                        resource.buffer);
                    });
            if (stale_buffer) {
                descriptor =
                    material.screen_input_descriptors.erase(
                        descriptor);
                continue;
            }
            descriptor->second =
                buildScreenInputDescriptor(
                    material.pipeline,
                    descriptor->second.resources,
                    rt_views);
            ++descriptor;
        }
    }
}

std::vector<vk::ImageView>
MaterialContainer::boundScreenInputImageViewsForTesting(
    GlobalMaterialId material, const PassDefinition &pass) const {
    const auto effective =
        resolveMaterialForPass(pass, material);
    const auto *descriptor =
        ensureScreenInputDescriptor(effective, pass);
    if (descriptor == nullptr) return {};
    return descriptor->variants.front().bound_image_views[
        GET_MODULE(RenderTargetContainer).historyFrameIndex()];
}

std::uint64_t MaterialContainer::screenInputBindingRevisionForTesting(
    GlobalMaterialId material, const PassDefinition &pass) const {
    const auto effective =
        resolveMaterialForPass(pass, material);
    const auto *descriptor =
        ensureScreenInputDescriptor(effective, pass);
    return descriptor == nullptr ? 0 : descriptor->binding_revision;
}

vk::PipelineLayout MaterialContainer::getPipelineLayout() const {
    if (!default_pipeline) {
        throw std::runtime_error("MaterialContainer has no material pipeline");
    }
    return GET_MODULE(PipelineFactory).layout(*default_pipeline);
}

vk::PipelineLayout MaterialContainer::pipelineLayout(
    GlobalMaterialId material_id) const {
    const auto &material = materials.get(material_id);
    return GET_MODULE(PipelineFactory).layout(material.pipeline);
}

vk::PipelineLayout MaterialContainer::pipelineLayout(
    const PassDefinition &pass,
    GlobalMaterialId material_id) const {
    material_id =
        resolveMaterialForPass(pass, material_id);
    const auto &material = materials.get(material_id);
    return GET_MODULE(PipelineFactory).layout(material.pipeline);
}

} // namespace Pelican
