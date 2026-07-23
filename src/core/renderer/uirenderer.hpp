#pragma once

#include "../container.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"

#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class UIContainer;
class FrameResources;
namespace ui { class UiModule; }

struct UiDrawRequest {
    vk::ImageView target_view;
    vk::Extent2D target_extent;
    vk::Format target_format;
    vk::AttachmentLoadOp load_op;
    vk::AttachmentStoreOp store_op;
    vk::ClearColorValue clear_color;
    double ui_scale = 1.0;
    vk::ImageView resolve_view;
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
    vk::ResolveModeFlagBits resolve_mode =
        vk::ResolveModeFlagBits::eNone;
};

struct UiRendererDependencies {
    const UIContainer &ui_container;
    const ui::UiModule &ui_module;
    const FrameResources &frame_resources;
};

DECLARE_MODULE(UiRenderer) {
    vk::Device device;
    ShaderBundleId vert_shader;
    ShaderBundleId frag_shader;
    std::unordered_map<std::uint64_t, PipelineHandle> pipelines;
    BufferWrapper vertex_buffer;
    BufferWrapper index_buffer;
    vk::DeviceSize vertex_capacity = 0;
    vk::DeviceSize index_capacity = 0;

    PipelineHandle getPipeline(vk::Format color_format,
                               vk::SampleCountFlagBits samples);
    void ensureBuffers(vk::DeviceSize vertex_bytes, vk::DeviceSize index_bytes);

  public:
    UiRenderer();
    ~UiRenderer();

    void render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request,
                const UiRendererDependencies &dependencies);
    bool hasGpuBuffersForTesting() const noexcept { return bool(vertex_buffer.buffer) || bool(index_buffer.buffer); }
};

} // namespace Pelican
