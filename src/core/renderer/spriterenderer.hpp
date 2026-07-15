#pragma once

#include "../container.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"

#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class AtlasAssetResource;
class FrameResources;
class SpriteScene;

struct SpriteDrawRequest {
    vk::ImageView color_view;
    vk::ImageView depth_view;
    vk::Extent2D extent;
    vk::Format color_format;
    vk::Format depth_format;
};

struct SpriteRendererDependencies {
    SpriteScene &scene;
    const AtlasAssetResource &atlas;
    const FrameResources &frame_resources;
};

DECLARE_MODULE(SpriteRenderer) {
    vk::Device device;
    ShaderBundleId vert_shader;
    ShaderBundleId frag_shader;
    std::unordered_map<std::uint64_t, PipelineHandle> pipelines;
    BufferWrapper vertex_buffer;
    BufferWrapper index_buffer;
    vk::DeviceSize vertex_capacity = 0;
    vk::DeviceSize index_capacity = 0;

    PipelineHandle getPipeline(vk::Format color_format, vk::Format depth_format);
    void ensureBuffers(vk::DeviceSize vertex_bytes, vk::DeviceSize index_bytes);

  public:
    SpriteRenderer();
    ~SpriteRenderer();
    void render(vk::CommandBuffer cmd_buf, const SpriteDrawRequest &request,
                const SpriteRendererDependencies &dependencies);
    bool hasGpuBuffersForTesting() const noexcept { return bool(vertex_buffer.buffer) || bool(index_buffer.buffer); }
};

} // namespace Pelican
