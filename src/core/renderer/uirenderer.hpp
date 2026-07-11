#pragma once

#include "../container.hpp"
#include "../shader/pipelinefactory.hpp"
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class UIContainer;
class FrameResources;

struct UiDrawRequest {
    vk::ImageView target_view;
    vk::Extent2D target_extent;
    vk::Format target_format;
    vk::AttachmentLoadOp load_op;
    vk::AttachmentStoreOp store_op;
    vk::ClearColorValue clear_color;
};

struct UiRendererDependencies {
    const UIContainer &ui_container;
    const FrameResources &frame_resources;
};

DECLARE_MODULE(UiRenderer) {
    vk::Device device;
    ShaderBundleId vert_shader;
    ShaderBundleId frag_shader;
    std::unordered_map<int, PipelineHandle> pipelines;

    PipelineHandle getPipeline(vk::Format color_format);

  public:
    UiRenderer();
    ~UiRenderer();

    void render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request,
                const UiRendererDependencies &dependencies);
};

} // namespace Pelican
