#pragma once

#include "../container.hpp"
#include <vulkan/vulkan.hpp>

namespace Pelican {

struct UiDrawRequest {
    vk::ImageView target_view;
    vk::Extent2D target_extent;
};

// UI レンダリング実行モジュール
DECLARE_MODULE(UiRenderer) {
    vk::Device device;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniquePipeline pipeline;

  public:
    UiRenderer();
    ~UiRenderer();

    void render(vk::CommandBuffer cmd_buf, const UiDrawRequest &request) const;
};

} // namespace Pelican
