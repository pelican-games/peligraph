#include "materialcontainer.hpp"
#include "../loader/basicconfig.hpp"
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
#include "materialdrawlooptable.hpp"
#include "materialgenerationplanner.hpp"
#include "materiallimits.hpp"
#include "standardmaterialresource.hpp"
#include "materialvaluesreloadhandler.hpp"
#include "texturereloadhandler.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace Pelican {



static_assert(
    materialCustomTextureFirstBinding ==
    PELICAN_MATERIAL_CUSTOM_TEXTURE_FIRST_BINDING);

// Every compiled pass the renderer would run right now: the published
// snapshot when there is one, the registered table otherwise. Registration
// and variant prewarming both walk it, and both used to spell the
// snapshot-precedence rule out for themselves.
template <typename F>
static void forEachCompiledPass(
    const RenderingPassContainer &registered_passes,
    F &&visit) {
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
            visit(program->rendering_pass);
        }
        return;
    }
    for (const auto rendering_pass_id :
         registered_passes.getRegisteredPassIds()) {
        visit(registered_passes
                  .getCompiledRenderingPass(
                      rendering_pass_id));
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


static std::string makePipelineKey(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract
        &rendering, bool allow_ray_query) {
    std::ostringstream key;
    key << "ray_query=" << allow_ray_query << ':';
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
    const auto &surface_resources =
        shader_library.get(
            info.frag_shader)
            .surface_resource_interface;

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
        const auto generated = std::find_if(
            surface_resources.begin(),
            surface_resources.end(),
            [&](const auto &binding) {
                return binding.port.name ==
                       port.name;
            });
        if (input_attachment &&
            (generated ==
                 surface_resources.end() ||
             !generated
                  ->input_attachment_extent)) {
            throw std::runtime_error(
                "material resource port '" +
                port.name +
                "' input attachment has no generated resolved extent");
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
                .input_attachment_extent =
                    input_attachment
                        ? generated
                              ->input_attachment_extent
                        : std::nullopt,
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


static GraphicsPipelineDesc makeMaterialPipelineDesc(
    const MaterialInfo &info,
    const MaterialPipelineRenderingContract
        &rendering,
    std::vector<ShaderResourceInterfaceBinding>
        resource_interface, bool allow_ray_query) {
    GraphicsPipelineDesc desc;
    desc.allow_ray_query = allow_ray_query;
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
    // CC-1a: counter-clockwise is the front (glTF), and a mirrored view (a
    // cube face) flips it at record time (materialrender.cpp).
    desc.front_face = vk::FrontFace::eCounterClockwise;
    desc.dynamic_front_face = true;
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






MaterialContainer::MaterialContainer()
    : device{GET_MODULE(VulkanManageCore).getDevice()},
      split_custom_samplers{surfaceSpvLinkExperimentalEnabled()},
      texture_store{device},
      descriptor_builder{device, split_custom_samplers},
      material_buffer{GET_MODULE(VulkanManageCore).allocBuf(
          sizeof(MaterialGpuData) * maxMaterials, vk::BufferUsageFlagBits::eStorageBuffer,
          vma::MemoryUsage::eAuto, vma::AllocationCreateFlagBits::eHostAccessSequentialWrite)} {}
MaterialContainer::~MaterialContainer() {
    deferred_callbacks.closeAndWait();
}

GlobalTextureId MaterialContainer::registerTexture(
    vk::Extent3D extent, const void *data) {
    return texture_store.registerTexture(extent, data);
}

GlobalTextureId MaterialContainer::registerTexture(
    vk::Extent3D extent, const void *data,
    vk::Format format, vk::DeviceSize bytes_num) {
    return texture_store.registerTexture(
        extent, data, format, bytes_num);
}

GlobalTextureId MaterialContainer::registerTexture(
    const LoadedImage &loaded, std::string_view name) {
    return texture_store.registerTexture(loaded, name);
}

GlobalTextureId MaterialContainer::registerTextureFile(
    const std::filesystem::path &path) {
    return texture_store.registerTextureFile(path);
}

MaterialTextureResource
MaterialContainer::createTextureResource(
    const LoadedImage &loaded,
    std::string_view name) const {
    return texture_store.createTextureResource(loaded,
                                               name);
}

bool MaterialContainer::textureShapeMatches(
    GlobalTextureId texture,
    const LoadedImage &image) const {
    return texture_store.textureShapeMatches(texture,
                                             image);
}

void MaterialContainer::uploadTextureInPlace(
    GlobalTextureId texture,
    const LoadedImage &image) const {
    texture_store.uploadTextureInPlace(texture, image);
}

std::pair<vk::ImageView, vk::ImageView>
MaterialContainer::textureViewsForTesting(
    GlobalTextureId texture) const {
    return texture_store.textureViewsForTesting(texture);
}

std::vector<uint8_t>
MaterialContainer::texturePixelsForTesting(
    GlobalTextureId texture,
    uint32_t mip_level) const {
    return texture_store.texturePixelsForTesting(
        texture, mip_level);
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

MaterialContainer::RayDeformationMaterial MaterialContainer::rayDeformationMaterial(
    GlobalMaterialId material) const {
    const auto &entry = materials.get(material);
    const auto fallback = entry.texture_bindings.front().texture;
    const std::array ids{
        entry.vat ? entry.vat->position_texture : fallback,
        entry.vat ? entry.vat->normal_texture : fallback};
    RayDeformationMaterial result{.vat = entry.vat};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        result.images[i] = vk::DescriptorImageInfo{
            texture_store.nearestSampler(), texture_store.textures.get(ids[i]).linear_view.get(),
            vk::ImageLayout::eShaderReadOnlyOptimal};
    }
    return result;
}

GlobalMaterialId MaterialContainer::registerMaterial(MaterialInfo info) {
    if (materials.size() >= maxMaterials) {
        throw std::runtime_error("Material capacity exceeded");
    }
    if (next_ray_material_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "ray material generation identity exhausted");
    }
    const auto ray_generation = next_ray_material_generation++;
    std::shared_ptr<const void> ray_lease =
        std::make_shared<const std::uint64_t>(ray_generation);
    if (!info.base_color_texture_identity.empty() &&
        !info.base_color_texture_identity.starts_with("document:") &&
        !info.base_color_texture_identity.starts_with("asset:")) {
        if (!info.source_document.empty()) {
            info.base_color_texture_identity =
                "document:" + info.source_document + "#texture:" +
                info.base_color_texture_identity;
        } else {
            info.base_color_texture_identity =
                "asset:" + info.source_asset + "#texture:" +
                info.base_color_texture_identity;
        }
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
    const auto pass_bindings = GET_MODULE(RenderingPassContainer).materialPassRenderingBindings(
        info.route, info.shader_contract, info.exact_pass);
    const bool allow_ray_query = !pass_bindings.empty() && std::all_of(
        pass_bindings.begin(), pass_bindings.end(), [](const auto &binding) { return binding.ray_query; });
    const auto pipeline_key =
        makePipelineKey(info, rendering, allow_ray_query);
    auto pipeline_it = pipelines.find(pipeline_key);
    if (pipeline_it == pipelines.end() || GET_MODULE(PipelineFactory).graphicsDesc(pipeline_it->second).allow_ray_query != allow_ray_query) {
        const auto pipeline_handle =
            GET_MODULE(PipelineFactory)
                .create(makeMaterialPipelineDesc(
                    info, rendering,
                    resource_interface, allow_ray_query));
        pipeline_it = pipelines.insert_or_assign(pipeline_key, pipeline_handle).first;
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
    desc_alloc_info.descriptorPool = descriptor_builder.materialPool();
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
        const auto &tex = texture_store.textures.get(texture);
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

    setImageInfo(baseColorBinding, info.base_color_texture, texture_store.linearSampler(), true);
    setImageInfo(metallicRoughnessBinding, info.metallic_roughness_texture, texture_store.linearSampler(), false);
    setImageInfo(normalBinding, info.normal_texture, texture_store.linearSampler(), false);
    setImageInfo(emissiveBinding, info.emissive_texture, texture_store.linearSampler(), true);
    setImageInfo(occlusionBinding, info.occlusion_texture, texture_store.linearSampler(), false);
    if (info.vat) {
        setImageInfo(vatPositionBinding, info.vat->position_texture, texture_store.nearestSampler(), false);
        setImageInfo(vatNormalBinding, info.vat->normal_texture, texture_store.nearestSampler(), false);
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
                texture_store.textures.get(texture);
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
                texture_store.materialTextureSampler(resolved_sampler);
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
    addImageWrite(occlusionBinding, vk::DescriptorType::eCombinedImageSampler);
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
        .ray_generation = ray_generation,
        .ray_lease = std::move(ray_lease),
        .name = std::move(info.name),
        .source_asset = std::move(info.source_asset),
        .source_material_index = info.source_material_index,
        .source_document = std::move(info.source_document),
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
        .default_routes = info.default_routes,
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
        .occlusion_texture = info.occlusion_texture,
        .base_color_texture_identity =
            std::move(info.base_color_texture_identity),
        .normalized_alpha_ir =
            std::move(info.normalized_alpha_ir),
        .alpha_mode = info.alpha_mode,
        .alpha_cutoff = info.alpha_cutoff,
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
        forEachCompiledPass(registered_passes,
                            bind_screen_inputs);
        GET_MODULE(VulkanManageCore)
            .writeBuf(material_buffer, &gpu_data, sizeof(MaterialGpuData) * material_id.value,
                      sizeof(gpu_data));
        for (const auto &binding : materials.get(material_id).texture_bindings) {
            texture_materials[binding.texture].insert(material_id);
        }
        if (texture_reload_handler) texture_reload_handler->materialRegistered(material_id);
        // LP-2: a material registered while generations are alive is
        // judged by their draw-loop rules now, not at the next publication.
        if (!registering_variant_records) {
            MaterialDrawLoopTable::materialRegisteredInLiveTables(
                material_id);
        }
    } catch (...) {
        for (auto it = texture_materials.begin(); it != texture_materials.end();) {
            it->second.erase(material_id);
            if (it->second.empty()) it = texture_materials.erase(it);
            else ++it;
        }
        (void)materials.extract(material_id);
        MaterialDrawLoopTable::materialReleasedFromLiveTables(material_id);
        throw;
    }
    return material_id;
}

void MaterialContainer::registerMaterialVariants(
    GlobalMaterialId base,
    std::vector<NamedMaterialVariantRegistration> registrations) {
    const auto &registered_base = materials.get(base);
    const auto base_route = registered_base.route;
    MaterialInfo inherited_appearance;
    inheritFromBase(inherited_appearance,
                    registered_base);
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
        {
            registering_variant_records = true;
            struct ResetFlag {
                bool &flag;
                ~ResetFlag() { flag = false; }
            } reset_flag{registering_variant_records};
            for (auto &registration : registrations) {
                inheritFromBase(registration.material,
                                inherited_appearance);
                const auto resource =
                    registerMaterial(std::move(registration.material));
                created.emplace_back(registration.name, resource);
                next.at(registration.name) = resource;
            }
        }

        auto &base_info = materials.get(base);
        base_info.variants.swap(next);
        attached = true;
        // LP-2: the base now has these variants; its draw-loop steps may
        // name them.
        MaterialDrawLoopTable::materialRegisteredInLiveTables(base);

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
        forEachCompiledPass(
            GET_MODULE(RenderingPassContainer), prewarm);
    } catch (...) {
        if (attached && materials.contains(base)) {
            materials.get(base).variants.swap(next);
            // LP-2: tables that already took the new variants judge the
            // base again as it was.
            try {
                MaterialDrawLoopTable::materialRegisteredInLiveTables(base);
            } catch (...) {
            }
        }
        std::vector<GlobalMaterialId> resources;
        resources.reserve(created.size());
        for (const auto &[name, resource] : created) {
            (void)name;
            resources.push_back(resource);
        }
        ReleaseErrorSink::record("material variant rollback release",
                                 releaseModelResources(std::move(resources), {}, false));
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

ReleaseReport MaterialContainer::releaseModelResources(
    std::vector<GlobalMaterialId> material_ids,
    std::vector<GlobalTextureId> texture_ids, bool deferred) noexcept {
    ReleaseReport report;
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
            try {
                if (!materials.contains(material)) continue;
                const auto bindings = materials.get(material).texture_bindings;
                for (const auto &binding : bindings) {
                    const auto found = texture_materials.find(binding.texture);
                    if (found == texture_materials.end()) continue;
                    found->second.erase(material);
                    if (found->second.empty()) texture_materials.erase(found);
                }
                auto value = materials.extract(material, !deferred);
                MaterialDrawLoopTable::materialReleasedFromLiveTables(
                    material);
                if (value) {
                    if (deferred) retired.material_ids.push_back(material);
                    retired.materials.push_back(std::move(*value));
                }
                if (fail_material_release_for_testing) {
                    throw std::runtime_error("injected material release failure");
                }
            } catch (...) {
                report.add({ReleaseStage::material,
                            ReleaseErrorCode::operation_failed,
                            static_cast<std::uint32_t>(material.value)});
            }
        }

        for (const auto texture : texture_ids) {
            try {
                const auto reverse = texture_materials.find(texture);
                if (reverse != texture_materials.end() && !reverse->second.empty()) {
                    report.add({ReleaseStage::texture,
                                ReleaseErrorCode::resource_still_referenced,
                                static_cast<std::uint32_t>(texture.value)});
                    continue;
                }
                texture_materials.erase(texture);
                auto value = texture_store.textures.extract(texture);
                if (value) retired.textures.push_back(std::move(*value));
                if (fail_texture_release_for_testing) {
                    throw std::runtime_error("injected texture release failure");
                }
            } catch (...) {
                report.add({ReleaseStage::texture,
                            ReleaseErrorCode::operation_failed,
                            static_cast<std::uint32_t>(texture.value)});
            }
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
    } catch (...) {
        report.add({ReleaseStage::material,
                    ReleaseErrorCode::operation_failed, 0});
    }
    return report;
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


void MaterialContainer::validateTextureReload(GlobalTextureId texture,
                                              const LoadedImage &image) const {
    const auto &vkcore = GET_MODULE(VulkanManageCore);
    validateLoadedTextureShape(
        image, "reload candidate",
        vkcore.getPhysDevice().getProperties().limits);
    const auto candidate_dimension =
        surfaceTextureDimensionOf(image.dimension);
    const bool has_srgb_view = image.format == ImagePixelFormat::Rgba8Unorm ||
                               image.format == ImagePixelFormat::Rgba8Srgb ||
                               image.format == ImagePixelFormat::Bc7Unorm ||
                               image.format == ImagePixelFormat::Bc7Srgb;
    const auto candidate_format =
        vkFormatForLoadedImage(image.format);
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
        allocation.descriptorPool = descriptor_builder.materialPool();
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
                                       : texture_store.textures.get(binding.texture);
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
    auto &slot = texture_store.textures.get(texture);
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

std::optional<GlobalTextureId>
MaterialContainer::materialTextureForTesting(
    GlobalMaterialId material, std::uint32_t binding) const {
    const auto &registered = materials.get(material);
    const auto found = std::find_if(
        registered.texture_bindings.begin(),
        registered.texture_bindings.end(),
        [&](const auto &texture) {
            return texture.binding == binding;
        });
    return found == registered.texture_bindings.end()
               ? std::nullopt
               : std::optional{found->texture};
}

size_t MaterialContainer::materialCapacityForTesting() const { return maxMaterials; }

RayMaterialTableSource MaterialContainer::rayMaterialTableSource(
    GlobalMaterialId material, std::string canonical_key,
    std::uint64_t expected_generation,
    const std::shared_ptr<const void> &expected_lease) const {
    const auto &registered = materials.get(material);
    if (registered.ray_generation != expected_generation ||
        registered.ray_lease != expected_lease || !expected_lease) {
        throw std::runtime_error(
            "pelican.ray_material_snapshot_stale@1: resolved material handle generation/lease is no longer live");
    }
    const auto expected_document_key =
        "document:" + registered.source_document + "#" + registered.name;
    const auto expected_asset_prefix =
        "asset:" + registered.source_asset + "#";
    const auto expected_asset_key =
        registered.source_material_index
            ? expected_asset_prefix +
                  std::to_string(*registered.source_material_index)
            : std::string{};
    if ((!registered.source_document.empty() &&
         canonical_key != expected_document_key) ||
        (registered.source_document.empty() &&
         ((!registered.source_asset.empty() &&
           ((registered.source_material_index &&
             canonical_key != expected_asset_key) ||
            (!registered.source_material_index &&
             !canonical_key.starts_with(expected_asset_prefix)))) ||
          (registered.source_asset.empty() &&
           !canonical_key.starts_with("asset:"))))) {
        throw std::runtime_error(
            "pelican.ray_material_key_mismatch@1: canonical row key does not identify the resolved material handle");
    }
    RayMaterialTableSource result{
        .material_handle = material.value,
        .material_generation = registered.ray_generation,
        .material_lease = registered.ray_lease,
        .canonical_key = std::move(canonical_key),
        .base_color_factor = {
            registered.base_color_factor.x,
            registered.base_color_factor.y,
            registered.base_color_factor.z,
            registered.base_color_factor.w,
        },
        .alpha_cutoff = registered.alpha_cutoff,
        .alpha_mode = static_cast<std::uint32_t>(registered.alpha_mode),
        .base_color_texture_identity =
            registered.base_color_texture_identity,
    };
    if (registered.normalized_alpha_ir) {
        const auto &alpha = *registered.normalized_alpha_ir;
        result.has_normalized_alpha_ir = true;
        result.alpha_source_kind = alpha.sourceKind();
        result.alpha_material_identity =
            alpha.materialIdentity();
        result.alpha_texture_identity =
            alpha.textureIdentity();
        result.normalized_alpha_mode =
            static_cast<std::uint32_t>(alpha.alphaMode());
        result.normalized_alpha_cutoff = alpha.cutoff();
        result.normalized_base_factor_a =
            alpha.baseFactorA();
        result.normalized_uv_offset_scale =
            alpha.uvOffsetScale();
        result.normalized_uv_rotation =
            alpha.uvRotation();
        result.normalized_uv_transform_state =
            static_cast<std::uint32_t>(
                alpha.uvTransformState());
        result.normalized_alpha_digest_sha256 =
            alpha.digestSha256();
    }
    if (!result.base_color_texture_identity.empty()) {
        const auto &texture = texture_store.textures.get(
            registered.base_color_texture);
        if (!texture.srgb_view) {
            throw std::runtime_error(
                "pelican.ray_material_texture_invalid@1: authored base color texture has no sRGB view");
        }
        result.base_color_srgb_view = texture.srgb_view.get();
        result.default_sampler = texture_store.linearSampler();
    }
    return result;
}

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
        const auto view_dimension =
            passInputViewDimensionForTarget(
                pass, binding->target);
        validateMaterialPassInputViewPolicy(
            required.contract.view_policy,
            view_dimension,
            required.contract.name, pass);
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

        const auto view_dimension =
            passInputViewDimensionForTarget(
                pass, binding->target);
        auto descriptor_view_dimension =
            view_dimension;
        if (!input_attachment &&
            binding->port.view ==
                ShaderResourcePortView::
                    family_array &&
            (view_dimension ==
                 PassInputViewDimension::
                     shared_2d ||
             view_dimension ==
                 PassInputViewDimension::
                     sequential_2d)) {
            // Material pipelines are shared across compatible pass variants.
            // Shared and sequential physical targets still have array-backed
            // storage, so expose the complete family as one array descriptor
            // and keep the shader ABI stable across those variants.
            descriptor_view_dimension =
                PassInputViewDimension::
                    family_2d_array;
        }
        auto descriptor_dimension =
            !input_attachment &&
                    (descriptor_view_dimension ==
                         PassInputViewDimension::
                             layered_2d_array ||
                     descriptor_view_dimension ==
                         PassInputViewDimension::
                             family_2d_array)
                ? ImageSubresourceViewDimension::
                      two_d_array
                : ImageSubresourceViewDimension::two_d;
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
                pass.name + "' (shader=" +
                std::string{
                    reflectedImageViewDimensionName(
                        required.image_view_dimension)} +
                ", pass=" +
                std::string{
                    reflectedImageViewDimensionName(
                        expected_shader_view)} +
                ", declared=" +
                std::string{
                    shaderResourcePortViewName(
                        binding->port.view)} +
                ")");
        }
        validateMaterialResourcePortViewPolicy(
            binding->port.view, view_dimension,
            required.port.name, pass);
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
                        descriptor_view_dimension,
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
        key, descriptor_builder.build(
                 material.pipeline, std::move(resources),
                 rt_views,
                 next_screen_input_binding_revision));
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

MaterialContainer::MaterialDrawBinding
MaterialContainer::bindMaterialDraw(
    vk::CommandBuffer cmd_buf, const PassDefinition &pass,
    GlobalMaterialId material_id,
    std::optional<PipelineHandle> pipeline_override,
    MaterialDrawBindState &bound,
    RenderPassViewInvocation invocation) const {
    const auto &material = materials.get(material_id);
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto pipeline =
        pipeline_override.value_or(material.pipeline);
    MaterialDrawBinding binding{
        .layout = pipeline_factory.layout(pipeline),
        .pipeline = pipeline,
        .descriptor_set = material.descset.get(),
    };
    if (!bound.pipeline || *bound.pipeline != pipeline) {
        cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                             pipeline_factory.pipeline(pipeline));
        binding.pipeline_bound = true;
    }
    if (!bound.pipeline ||
        bound.descriptor_set != binding.descriptor_set) {
        cmd_buf.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics, binding.layout,
            imageDescriptorSetNumber, {binding.descriptor_set}, {});
        binding.descriptor_set_bound = true;
    }
    bound.pipeline = pipeline;
    bound.descriptor_set = binding.descriptor_set;

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
            vk::PipelineBindPoint::eGraphics, binding.layout,
            PELICAN_SET_PASS_INPUT,
            {screen_inputs->variants[variant]
                 .descsets[parity]
                 .get()},
            {});
    }
    return binding;
}

std::vector<GlobalMaterialId>
MaterialContainer::drawLoopMaterials() const {
    std::unordered_set<GlobalMaterialId, GlobalMaterialId::Hash>
        variant_records;
    materials.forEach([&](GlobalMaterialId,
                          const InternalMaterialInfo &material) {
        for (const auto &[name, record] : material.variants) {
            (void)name;
            variant_records.insert(record);
        }
    });
    std::vector<GlobalMaterialId> result;
    materials.forEach([&](GlobalMaterialId id,
                          const InternalMaterialInfo &) {
        if (!variant_records.contains(id)) result.push_back(id);
    });
    std::sort(result.begin(), result.end(),
              [](GlobalMaterialId left, GlobalMaterialId right) {
                  return left.value < right.value;
              });
    return result;
}

MaterialDrawSubject MaterialContainer::drawLoopSubject(
    GlobalMaterialId material_id) const {
    const auto &material = materials.get(material_id);
    MaterialDrawSubject subject{
        .key = materialDrawSubjectKey(
            material.name, material.source_document,
            material.source_asset,
            material.source_material_index),
        .name = material.name,
        .tags = material.tags,
        .route = material.route,
        .contract = material.shader_contract,
        .skinned = material.skinned,
        .exact_pass = material.exact_pass,
    };
    for (const auto &[name, record] : material.variants) {
        (void)record;
        subject.variants.push_back(name);
    }
    return subject;
}

PipelineHandle MaterialContainer::materialPipeline(
    GlobalMaterialId material) const {
    return materials.get(material).pipeline;
}

GraphicsPipelineDesc materialDrawOverridePipelineDesc(
    GraphicsPipelineDesc desc, const MaterialInfo &material,
    const MaterialDrawRenderStateOverride &render_state,
    const MaterialPipelineRenderingContract &rendering) {
    MaterialInfo state;
    state.render_state = applyMaterialDrawRenderStateOverride(
        material.render_state, render_state);
    state.output_schema = material.output_schema;
    state.vat = material.vat;
    desc.depth_test = state.render_state.depth_test;
    desc.depth_write = state.render_state.depth_write;
    desc.depth_compare =
        toVkCompare(state.render_state.depth_compare);
    desc.cull_mode = toVkCull(state.render_state.cull);
    if (!render_state.blend) return desc;

    const GraphicsPipelineDesc defaults{};
    desc.blend = false;
    desc.src_color_blend_factor = defaults.src_color_blend_factor;
    desc.dst_color_blend_factor = defaults.dst_color_blend_factor;
    desc.src_alpha_blend_factor = defaults.src_alpha_blend_factor;
    desc.dst_alpha_blend_factor = defaults.dst_alpha_blend_factor;
    if (state.render_state.blend == SurfaceBlendMode::blend) {
        desc.blend = true;
        desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
        desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
        desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
        desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    } else if (state.render_state.blend == SurfaceBlendMode::additive) {
        desc.blend = true;
        desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
        desc.dst_color_blend_factor = vk::BlendFactor::eOne;
        desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
        desc.dst_alpha_blend_factor = vk::BlendFactor::eOne;
    }
    if (!desc.color_attachment_states.empty()) {
        desc.color_attachment_states =
            resolveMaterialColorAttachmentStates(state, rendering);
    }
    return desc;
}

GraphicsPipelineDesc MaterialContainer::drawOverridePipelineDesc(
    GlobalMaterialId base,
    const MaterialDrawRenderStateOverride &render_state) const {
    const auto &material = materials.get(base);
    MaterialInfo state;
    state.render_state = material.render_state;
    state.output_schema = material.output_schema;
    state.vat = material.vat;
    return materialDrawOverridePipelineDesc(
        GET_MODULE(PipelineFactory).graphicsDesc(material.pipeline), state,
        render_state,
        MaterialPipelineRenderingContract{
            .color_formats = material.pipeline_color_formats,
            .depth_format = material.pipeline_depth_format,
            .rasterization_samples =
                material.pipeline_rasterization_samples,
            .local_read = material.pipeline_local_read,
            .output_states = material.pipeline_output_states,
        });
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
                deferRetiredDescriptorResources(
                    GET_MODULE(DeletionQueue),
                    std::move(descriptor->second));
                descriptor =
                    material.screen_input_descriptors.erase(
                        descriptor);
                continue;
            }
            auto replacement =
                descriptor_builder.build(
                    material.pipeline,
                    descriptor->second.resources,
                    rt_views,
                    next_screen_input_binding_revision);
            deferRetiredDescriptorResources(
                GET_MODULE(DeletionQueue),
                std::move(descriptor->second));
            descriptor->second =
                std::move(replacement);
            ++descriptor;
        }
    }
}

std::vector<vk::ImageView>
MaterialContainer::boundScreenInputImageViewsForTesting(
    GlobalMaterialId material, const PassDefinition &pass,
    std::uint32_t view_index) const {
    const auto effective =
        resolveMaterialForPass(pass, material);
    const auto *descriptor =
        ensureScreenInputDescriptor(effective, pass);
    if (descriptor == nullptr) return {};
    return descriptor->variants.at(view_index)
        .bound_image_views[
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
