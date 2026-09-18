#pragma once

#include "../renderingpass/passinputdescriptorvariant.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class RenderTargetImageViewResolver;

// One resolved material pass-input / resource-port descriptor. Hoisted out
// of MaterialContainer::InternalMaterialInfo so the descriptor builder can
// live in its own translation unit; the field list is unchanged.
struct MaterialScreenInputResource {
    std::string name;
    std::uint32_t binding = 0;
    vk::DescriptorType descriptor_type =
        vk::DescriptorType::eCombinedImageSampler;
    GlobalRenderTargetId target = noRenderTargetId();
    FrameGraphBufferId buffer =
        noFrameGraphBufferId();
    bool history = false;
    PassInputViewDimension view_dimension =
        PassInputViewDimension::shared_2d;
    ImageSubresourceViewDimension
        descriptor_dimension =
            ImageSubresourceViewDimension::two_d;
    ShaderResourcePortSampling sampling;
    std::optional<ImageSubresourceRange> subresource;
    bool material_resource = false;

    bool isImage() const {
        return descriptor_type ==
                   vk::DescriptorType::
                       eCombinedImageSampler ||
               descriptor_type ==
                   vk::DescriptorType::eInputAttachment;
    }
    bool isInputAttachment() const {
        return descriptor_type ==
               vk::DescriptorType::eInputAttachment;
    }
    bool isBuffer() const {
        return descriptor_type ==
               vk::DescriptorType::eStorageBuffer;
    }
};

// The descriptor sets one (material, pass) pair binds, one per logical
// view and history parity.
struct MaterialScreenInputDescriptorSet {
    std::vector<PassInputDescriptorVariant> variants;
    std::vector<MaterialScreenInputResource> resources;
    std::uint64_t binding_revision = 0;
};

// The sampled/local-read ABI a material expects of one declared pass
// input. Hoisted for the same reason as the resource record above.
struct MaterialPassInputState {
    MaterialPassInputContract contract;
    vk::DescriptorType descriptor_type =
        vk::DescriptorType::eCombinedImageSampler;
    std::optional<std::uint32_t>
        input_attachment_index;

    bool operator==(const MaterialPassInputState &) const =
        default;
};

// Which physical view a pass presents for one of its input targets. Both
// the pass-input loop and the resource-port loop of
// ensureScreenInputDescriptor used to spell this out identically.
PassInputViewDimension passInputViewDimensionForTarget(
    const PassDefinition &pass,
    GlobalRenderTargetId target);

// The declared view x physical view relation, as a table instead of a
// hand-expanded ladder of negations. Throws with the caller's vocabulary
// ("material pass input" / "material resource port") when the pair is not
// in the relation.
void validateMaterialPassInputViewPolicy(
    MaterialPassInputViewPolicy policy,
    PassInputViewDimension view_dimension,
    std::string_view input_name,
    const PassDefinition &pass);
void validateMaterialResourcePortViewPolicy(
    ShaderResourcePortView port_view,
    PassInputViewDimension view_dimension,
    std::string_view port_name,
    const PassDefinition &pass);

// Cache key for one (material, pass) descriptor set: every pass field the
// built descriptors depend on.
std::string makeScreenInputPassKey(
    const PassDefinition &pass);

// The positional input index of a target that the pass reads back through
// a local-read attachment, or nullopt when it is sampled normally.
std::optional<std::uint32_t>
materialPassInputAttachmentIndex(
    const GraphicsPipelineRenderingLocalReadContract
        &local_read,
    const PassDefinition &pass,
    GlobalRenderTargetId target, bool history);

// Owns the three descriptor pools and the screen/resource sampler caches,
// and turns a resolved resource list into descriptor sets. It holds no
// material table: everything it needs arrives as arguments.
class MaterialDescriptorBuilder {
  public:
    MaterialDescriptorBuilder(
        vk::Device device, bool split_custom_samplers);

    // The per-material descriptor set (textures + material SSBO) is
    // allocated by the container from this pool.
    vk::DescriptorPool materialPool() const {
        return desc_pool.get();
    }

    vk::Sampler materialResourceSampler(
        ShaderResourcePortSampling sampling) const;

    MaterialScreenInputDescriptorSet build(
        PipelineHandle pipeline,
        std::vector<MaterialScreenInputResource>
            resources,
        const RenderTargetImageViewResolver &rt_views,
        std::uint64_t &next_binding_revision) const;

  private:
    // Screen inputs, material resources and the historical clamp cache all
    // pick a sampler differently; the rule stays here with the pools that
    // back the sets it writes.
    vk::Sampler samplerFor(
        const MaterialScreenInputResource &resource)
        const;

    vk::Device device;
    vk::UniqueSampler screen_nearest_sampler;
    vk::UniqueSampler screen_linear_sampler;
    mutable std::array<vk::UniqueSampler, 6>
        material_resource_samplers;
    vk::UniqueDescriptorPool desc_pool;
    vk::UniqueDescriptorPool screen_input_desc_pool;
    mutable vk::UniqueDescriptorPool
        material_resource_desc_pool;
};

} // namespace Pelican
