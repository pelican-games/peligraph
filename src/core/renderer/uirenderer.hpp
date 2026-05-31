#pragma once

#include "../container.hpp"
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct UiDrawRequest {
    vk::ImageView target_view;
    vk::Extent2D target_extent;
    vk::Format target_format;
    vk::AttachmentLoadOp load_op;
    vk::AttachmentStoreOp store_op;
    vk::ClearColorValue clear_color;
};

// UI レンダリング実行モジュール
DECLARE_MODULE(UiRenderer) {
    vk::Device device;
    vk::UniquePipelineLayout pipeline_layout;
    vk::ShaderModule vert_shader;
    vk::ShaderModule frag_shader;
    std::unordered_map<int, vk::UniquePipeline> pipelines;

    vk::Pipeline getPipeline(vk::Format color_format);

  public:
    UiRenderer();
    ~UiRenderer();

    void render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request);
};

} // namespace Pelican
