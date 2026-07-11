#pragma once

#include "shaderlibrary.hpp"
#include "shaderreflection.hpp"
#include "../container.hpp"
#include "../resourcecontainer.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct GraphicsPipelineDesc {
    ShaderBundleId vert;
    std::optional<ShaderBundleId> frag;
    std::vector<vk::Format> color_formats;
    std::optional<vk::Format> depth_format;
    std::vector<std::string> shader_defines;
    bool use_engine_vertex_layout = false;
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
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
};

struct ComputePipelineDesc {
    ShaderBundleId shader;
    std::vector<std::string> shader_defines;
};

PELICAN_DEFINE_HANDLE(PipelineHandle, int);

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
    PipelineFactory();
    ~PipelineFactory();

    PipelineHandle create(const GraphicsPipelineDesc &desc);
    PipelineHandle createCompute(const ComputePipelineDesc &desc);

    vk::Pipeline pipeline(PipelineHandle handle) const;
    vk::PipelineLayout layout(PipelineHandle handle) const;
    vk::DescriptorSetLayout descriptorSetLayout(PipelineHandle handle, uint32_t set) const;
    vk::DescriptorSetLayout frameDescriptorSetLayout();
    const ShaderReflection &reflection(PipelineHandle handle) const;

    void rebuildDirty();
};

} // namespace Pelican
