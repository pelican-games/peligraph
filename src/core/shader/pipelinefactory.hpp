#pragma once

#include "graphicsviewcontract.hpp"
#include "shaderlibrary.hpp"
#include "shaderreflection.hpp"
#include "shaderresourceinterface.hpp"
#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

inline constexpr std::uint32_t
    unusedGraphicsAttachmentMapping =
        std::numeric_limits<std::uint32_t>::max();

struct GraphicsPipelineRenderingLocalReadContract {
    bool enabled = false;
    // Both arrays are indexed by the color attachment slot declared in
    // GraphicsPipelineDesc::color_formats. Values are shader locations/input
    // indices or unusedGraphicsAttachmentMapping.
    std::vector<std::uint32_t>
        color_attachment_locations;
    std::vector<std::uint32_t>
        color_attachment_input_indices;
    std::uint32_t depth_attachment_input_index =
        unusedGraphicsAttachmentMapping;
    std::uint32_t stencil_attachment_input_index =
        unusedGraphicsAttachmentMapping;

    bool operator==(
        const GraphicsPipelineRenderingLocalReadContract &) const =
        default;
};

struct GraphicsPipelineColorAttachmentState {
    bool blend_enabled = false;
    vk::BlendFactor source_color =
        vk::BlendFactor::eOne;
    vk::BlendFactor destination_color =
        vk::BlendFactor::eZero;
    vk::BlendOp color_operation =
        vk::BlendOp::eAdd;
    vk::BlendFactor source_alpha =
        vk::BlendFactor::eOne;
    vk::BlendFactor destination_alpha =
        vk::BlendFactor::eZero;
    vk::BlendOp alpha_operation =
        vk::BlendOp::eAdd;
    vk::ColorComponentFlags write_mask =
        vk::ColorComponentFlagBits::eR |
        vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB |
        vk::ColorComponentFlagBits::eA;

    bool operator==(
        const GraphicsPipelineColorAttachmentState &) const =
        default;
};

struct GraphicsPipelineDesc {
    ShaderBundleId vert;
    std::optional<ShaderBundleId> frag;
    std::vector<vk::Format> color_formats;
    std::optional<vk::Format> depth_format;
    std::vector<std::string> shader_defines;
    bool use_engine_vertex_layout = false;
    bool use_skinned_vertex_layout = false;
    std::vector<vk::VertexInputBindingDescription> vertex_bindings;
    std::vector<vk::VertexInputAttributeDescription> vertex_attributes;
    bool depth_test = false;
    bool depth_write = false;
    vk::CompareOp depth_compare = vk::CompareOp::eLess;
    vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eNone;
    vk::FrontFace front_face = vk::FrontFace::eCounterClockwise;
    bool blend = false;
    vk::BlendFactor src_color_blend_factor = vk::BlendFactor::eOne;
    vk::BlendFactor dst_color_blend_factor = vk::BlendFactor::eZero;
    vk::BlendOp color_blend_op = vk::BlendOp::eAdd;
    vk::BlendFactor src_alpha_blend_factor = vk::BlendFactor::eOne;
    vk::BlendFactor dst_alpha_blend_factor = vk::BlendFactor::eZero;
    vk::BlendOp alpha_blend_op = vk::BlendOp::eAdd;
    // Empty preserves the legacy pass-wide fields above. Otherwise this must
    // align one-to-one with color_formats.
    std::vector<GraphicsPipelineColorAttachmentState>
        color_attachment_states;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    vk::SampleCountFlagBits rasterization_samples =
        vk::SampleCountFlagBits::e1;
    GraphicsPipelineViewContract view;
    GraphicsPipelineRenderingLocalReadContract
        local_read;
    // Empty is the raw-layout escape hatch. Generated resource ports carry
    // their reflected ABI here so every hot-reload rebuild revalidates it.
    std::vector<ShaderResourceInterfaceBinding>
        resource_interface;

    bool operator==(
        const GraphicsPipelineDesc &) const = default;
};

struct ComputePipelineDesc {
    ShaderBundleId shader;
    std::vector<std::string> shader_defines;
    std::vector<ShaderResourceInterfaceBinding>
        resource_interface;
};

PELICAN_DEFINE_HANDLE(PipelineHandle, int);

struct GraphicsPipelineReloadOverride {
    PipelineHandle handle;
    GraphicsPipelineDesc desc;
};

struct PipelineRebuildResult {
    size_t dirty_shaders = 0;
    size_t attempted_pipelines = 0;
    size_t rebuilt_pipelines = 0;
    size_t failed_pipelines = 0;
    bool committed = false;
    std::string last_error;
};

// Set 0 is engine-owned. The six-binding form is the stable non-ray ABI;
// only pipelines that reflect the TLAS binding use the extended form.
std::vector<vk::DescriptorSetLayoutBinding>
frameDescriptorSetLayoutBindings(bool ray_query);

DECLARE_MODULE(PipelineFactory) {
    struct DescriptorSetLayoutKey {
        std::vector<vk::DescriptorSetLayoutBinding> bindings;

        bool operator==(const DescriptorSetLayoutKey &other) const;
    };

    struct DescriptorSetLayoutKeyHash {
        size_t operator()(const DescriptorSetLayoutKey &key) const;
    };

    struct PipelineRecord {
        std::variant<GraphicsPipelineDesc, ComputePipelineDesc> desc;
        ShaderReflection reflection;
        std::vector<vk::DescriptorSetLayout> descriptor_set_layouts;
        vk::UniquePipelineLayout layout;
        vk::UniquePipeline pipeline;
        bool ray_query_frame_set = false;
    };

    vk::Device device;
    ShaderLibrary &shader_library;
    std::filesystem::path pipeline_cache_path;
    vk::UniquePipelineCache pipeline_cache;
    std::unordered_map<DescriptorSetLayoutKey, vk::UniqueDescriptorSetLayout, DescriptorSetLayoutKeyHash>
        descriptor_set_layout_cache;
    ResourceContainer<PipelineHandle, PipelineRecord> pipelines;
    std::vector<PipelineHandle> pipeline_handles;

    std::vector<DescriptorSetLayoutKey> descriptorSetLayoutKeysFor(const ShaderReflection &reflection) const;
    std::vector<vk::DescriptorSetLayout> descriptorSetLayoutsFor(const ShaderReflection &reflection);
    vk::UniquePipelineLayout createPipelineLayout(const ShaderReflection &reflection,
                                                  std::span<const vk::DescriptorSetLayout> layouts) const;
    vk::UniquePipeline createGraphicsPipeline(const GraphicsPipelineDesc &desc,
                                              vk::PipelineLayout layout) const;
    vk::UniquePipeline createComputePipeline(const ComputePipelineDesc &desc,
                                             vk::PipelineLayout layout) const;
    PipelineRecord buildGraphicsPipeline(const GraphicsPipelineDesc &desc);
    PipelineRecord buildComputePipeline(const ComputePipelineDesc &desc);
    void replacePipeline(PipelineHandle handle, PipelineRecord replacement);
    void savePipelineCache() noexcept;

  public:
    struct RegistrationCheckpoint {
        std::size_t pipeline_count = 0;
        std::vector<DescriptorSetLayoutKey>
            descriptor_set_layout_keys;
    };

    PipelineFactory();
    ~PipelineFactory();

    PipelineHandle create(const GraphicsPipelineDesc &desc);
    PipelineHandle createCompute(const ComputePipelineDesc &desc);

    vk::Pipeline pipeline(PipelineHandle handle) const;
    vk::PipelineLayout layout(PipelineHandle handle) const;
    vk::DescriptorSetLayout descriptorSetLayout(PipelineHandle handle, uint32_t set) const;
    vk::DescriptorSetLayout frameDescriptorSetLayout(
        bool ray_query = false);
    bool pipelineLayoutUsesRayQueryFrameSet(
        vk::PipelineLayout layout) const noexcept;
    const ShaderReflection &reflection(PipelineHandle handle) const;
    GraphicsPipelineDesc graphicsDesc(
        PipelineHandle handle) const;

    // Builds every dependent pipeline while prepared shader bundles are only
    // temporarily visible. Publication happens once, after all candidates and
    // the optional cross-domain callback have succeeded.
    PipelineRebuildResult rebuildPrepared(
        PreparedShaderReload prepared,
        const std::function<void()> &before_publish = {},
        std::span<const GraphicsPipelineReloadOverride>
            graphics_overrides = {});
    PipelineRebuildResult rebuildDirty();

    RegistrationCheckpoint checkpointRegistrations() const;
    void rollbackRegistrations(
        const RegistrationCheckpoint &checkpoint);
    std::vector<PipelineHandle>
    registrationsSince(
        const RegistrationCheckpoint &checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return pipeline_handles.size();
    }
    void retireRegistrations(
        const std::vector<PipelineHandle> &handles) noexcept;
};

} // namespace Pelican
