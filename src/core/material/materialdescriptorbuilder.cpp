#include "materialdescriptorbuilder.hpp"
#include "materiallimits.hpp"
#include "../renderingpass/framegraphbuffercontainer.hpp"
#include "../renderingpass/rendertargetcontainer.hpp"
#include "../renderingpass/rendertargetimageviewresolver.hpp"
#include "../shader/pelican_sets.hpp"
#include "../vkcore/core.hpp"
#include <algorithm>
#include <array>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Pelican {

namespace {

vk::UniqueDescriptorPool createDescriptorPool(
    vk::Device device, bool split_custom_samplers) {
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

vk::UniqueDescriptorPool createScreenInputDescriptorPool(
    vk::Device device,
    std::uint32_t max_sets = maxMaterials * 8) {
    const std::array pool_sizes{
        vk::DescriptorPoolSize{
            vk::DescriptorType::eCombinedImageSampler,
            max_sets * maxMaterialPassInputs},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eInputAttachment,
            max_sets * maxMaterialPassInputs},
    };
    vk::DescriptorPoolCreateInfo create_info;
    // Material/pass pairs are rebound independently while sharing this pool.
    // Free lease-retired sets individually instead of retiring a pool that
    // still backs unrelated live descriptors.
    create_info.flags =
        vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = max_sets;
    create_info.setPoolSizes(pool_sizes);
    return device.createDescriptorPoolUnique(create_info);
}

vk::UniqueDescriptorPool
createMaterialResourceDescriptorPool(
    vk::Device device,
    std::uint32_t max_sets = maxMaterials * 8) {
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

vk::UniqueSampler createScreenSampler(vk::Device device,
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

vk::SamplerAddressMode materialResourceAddressMode(
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

std::size_t materialResourceSamplerIndex(
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

vk::UniqueSampler createMaterialResourceSampler(
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

// --- declared view x physical view relation ------------------------------

constexpr std::uint8_t viewBit(
    PassInputViewDimension dimension) {
    return static_cast<std::uint8_t>(
        1u << static_cast<std::uint8_t>(dimension));
}

constexpr std::uint8_t anyPhysicalView =
    viewBit(PassInputViewDimension::shared_2d) |
    viewBit(PassInputViewDimension::sequential_2d) |
    viewBit(PassInputViewDimension::layered_2d_array) |
    viewBit(PassInputViewDimension::family_2d_array);

struct ViewPolicyRule {
    std::uint8_t allowed = anyPhysicalView;
    std::string_view message;
};

// Pass inputs: MaterialPassInputViewPolicy -> allowed physical views.
constexpr std::array<ViewPolicyRule, 3>
    passInputViewPolicyRules{
        ViewPolicyRule{anyPhysicalView, {}},
        ViewPolicyRule{
            viewBit(PassInputViewDimension::shared_2d),
            "requires shared_2d view policy"},
        ViewPolicyRule{
            viewBit(
                PassInputViewDimension::family_2d_array),
            "requires family_array view policy"},
    };

// Resource ports: ShaderResourcePortView -> allowed physical views. The
// two remaining rules below are inverse ones (they constrain the
// declaration from the physical side) and keep their own wording.
constexpr std::array<ViewPolicyRule, 4>
    resourcePortViewPolicyRules{
        ViewPolicyRule{
            viewBit(PassInputViewDimension::shared_2d),
            "requires shared_2d"},
        ViewPolicyRule{
            static_cast<std::uint8_t>(
                viewBit(PassInputViewDimension::
                            sequential_2d) |
                viewBit(PassInputViewDimension::
                            layered_2d_array) |
                viewBit(PassInputViewDimension::
                            family_2d_array)),
            "requires per_view"},
        ViewPolicyRule{
            static_cast<std::uint8_t>(
                viewBit(PassInputViewDimension::
                            shared_2d) |
                viewBit(PassInputViewDimension::
                            sequential_2d) |
                viewBit(PassInputViewDimension::
                            family_2d_array)),
            "requires family_array"},
        ViewPolicyRule{anyPhysicalView, {}},
    };

void rejectViewPolicy(std::string_view kind,
                      std::string_view name,
                      std::string_view message,
                      const PassDefinition &pass) {
    throw std::runtime_error(
        std::string{kind} + " '" + std::string{name} +
        "' " + std::string{message} + " in pass '" +
        pass.name + "'");
}

} // namespace

PassInputViewDimension passInputViewDimensionForTarget(
    const PassDefinition &pass,
    GlobalRenderTargetId target) {
    const auto position = std::find(
        pass.input_targets.begin(),
        pass.input_targets.end(), target);
    if (position == pass.input_targets.end() ||
        pass.input_target_views.size() !=
            pass.input_targets.size()) {
        return PassInputViewDimension::shared_2d;
    }
    return pass.input_target_views.at(
        static_cast<std::size_t>(
            position - pass.input_targets.begin()));
}

void validateMaterialPassInputViewPolicy(
    MaterialPassInputViewPolicy policy,
    PassInputViewDimension view_dimension,
    std::string_view input_name,
    const PassDefinition &pass) {
    const auto &rule = passInputViewPolicyRules.at(
        static_cast<std::size_t>(policy));
    if ((rule.allowed & viewBit(view_dimension)) == 0) {
        rejectViewPolicy("material pass input",
                         input_name, rule.message,
                         pass);
    }
}

void validateMaterialResourcePortViewPolicy(
    ShaderResourcePortView port_view,
    PassInputViewDimension view_dimension,
    std::string_view port_name,
    const PassDefinition &pass) {
    const auto &rule = resourcePortViewPolicyRules.at(
        static_cast<std::size_t>(port_view));
    if ((rule.allowed & viewBit(view_dimension)) == 0) {
        rejectViewPolicy("material resource port",
                         port_name, rule.message,
                         pass);
    }
    // A family-array physical target is only meaningful to a port that
    // declared the family (or a cube, which selects all six faces).
    if (view_dimension ==
            PassInputViewDimension::family_2d_array &&
        port_view !=
            ShaderResourcePortView::family_array &&
        port_view != ShaderResourcePortView::cube) {
        rejectViewPolicy("material resource port",
                         port_name,
                         "must declare family_array",
                         pass);
    }
}

std::string makeScreenInputPassKey(
    const PassDefinition &pass) {
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

std::optional<std::uint32_t>
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

MaterialDescriptorBuilder::MaterialDescriptorBuilder(
    vk::Device device, bool split_custom_samplers)
    : device{device},
      screen_nearest_sampler{createScreenSampler(
          device, vk::Filter::eNearest)},
      screen_linear_sampler{createScreenSampler(
          device, vk::Filter::eLinear)},
      desc_pool{createDescriptorPool(
          device, split_custom_samplers)},
      screen_input_desc_pool{
          createScreenInputDescriptorPool(device)} {}

vk::Sampler
MaterialDescriptorBuilder::materialResourceSampler(
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

vk::Sampler MaterialDescriptorBuilder::samplerFor(
    const MaterialScreenInputResource &resource) const {
    if (resource.isInputAttachment()) {
        return vk::Sampler{};
    }
    if (resource.material_resource) {
        return materialResourceSampler(
            resource.sampling);
    }
    // Screen inputs historically resolved clamp_to_edge through their own
    // two-sampler cache; anything else falls back to the resource cache.
    if (resource.sampling.address_mode ==
        ShaderResourcePortAddressMode::clamp_to_edge) {
        return resource.sampling.filter ==
                       ShaderResourcePortFilter::nearest
                   ? screen_nearest_sampler.get()
                   : screen_linear_sampler.get();
    }
    return materialResourceSampler(resource.sampling);
}

MaterialScreenInputDescriptorSet
MaterialDescriptorBuilder::build(
    PipelineHandle pipeline,
    std::vector<MaterialScreenInputResource> resources,
    const RenderTargetImageViewResolver &rt_views,
    std::uint64_t &next_binding_revision) const {
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

    MaterialScreenInputDescriptorSet result;
    result.resources = std::move(resources);
    result.binding_revision = next_binding_revision++;

    // How many layers this input actually selects. Variant counting and
    // the exact-match precondition below both read the subresource, so a
    // narrowed input cannot ask for more variants than it owns layers.
    const auto selected_layers =
        [&](const MaterialScreenInputResource &resource) {
            return resource.subresource
                       ? resource.subresource->layer_count
                       : rt_views.arrayLayers(
                             resource.target);
        };
    // Which inputs give each logical view its own descriptor. shared_2d
    // and the producer family are bound once for every variant; an input
    // attachment reading a layered target sees the whole array.
    const auto layer_binding =
        [](const MaterialScreenInputResource &resource) {
            if (resource.view_dimension ==
                    PassInputViewDimension::shared_2d) {
                return PassInputLayerBinding::shared;
            }
            if (resource.view_dimension ==
                    PassInputViewDimension::
                        family_2d_array ||
                (resource.isInputAttachment() &&
                 resource.view_dimension ==
                     PassInputViewDimension::
                         layered_2d_array)) {
                return PassInputLayerBinding::whole_array;
            }
            return PassInputLayerBinding::per_variant;
        };

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
            layer_binding(resource) ==
                PassInputLayerBinding::per_variant) {
            variant_count = std::max(
                variant_count,
                selected_layers(resource));
        }
    }
    for (const auto &resource : result.resources) {
        if (resource.isImage() &&
            layer_binding(resource) ==
                PassInputLayerBinding::per_variant &&
            selected_layers(resource) !=
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

            PassInputDescriptorWrites writes{
                descriptor_variant, parity,
                result.resources.size(),
                result.resources.size()};
            for (const auto &resource :
                 result.resources) {
                if (resource.isBuffer()) {
                    writes.appendBuffer(
                        resource.binding,
                        GET_MODULE(
                            FrameGraphResourceContainer)
                            .descriptorInfo(
                                resource.buffer));
                    continue;
                }
                ResolvedPassInput input{
                    .target = resource.target,
                    .history = resource.history,
                    .view = resource.view_dimension,
                    .descriptor_dimension =
                        resource.descriptor_dimension,
                    .subresource =
                        resource.subresource,
                    .layer_binding =
                        layer_binding(resource),
                    .variant = variant,
                    .parity = parity,
                };
                // A family-array descriptor authored as the degenerate
                // "layer 0, one layer" range means the whole producer
                // family. Expanding it here keeps the shared ladder free
                // of material-only vocabulary.
                if (input.subresource &&
                    resource.descriptor_dimension ==
                        ImageSubresourceViewDimension::
                            two_d_array &&
                    resource.view_dimension ==
                        PassInputViewDimension::
                            family_2d_array &&
                    input.subresource->base_array_layer ==
                        0 &&
                    input.subresource->layer_count == 1) {
                    input.subresource->layer_count =
                        rt_views.arrayLayers(
                            resource.target);
                }
                writes.appendImage(
                    resource.binding,
                    resource.descriptor_type,
                    samplerFor(resource),
                    resolvePassInputImageView(
                        input, rt_views));
            }
            writes.submit(device);
        }
    }
    return result;
}

} // namespace Pelican
