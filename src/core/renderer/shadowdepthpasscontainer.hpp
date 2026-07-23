#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(ShadowDepthPassContainer) {
  public:
    struct PipelineVariants { PipelineHandle regular; PipelineHandle skinned; };
  private:
    std::unordered_map<int, PipelineVariants> pipelines;

  public:
    PassId registerShadowDepthPass(vk::Format depth_format, ShaderBundleId vert_shader,
                                   std::vector<std::string> shader_defines = {},
                                   vk::SampleCountFlagBits samples =
                                       vk::SampleCountFlagBits::e1);
    void bind(vk::CommandBuffer cmd_buf, PassId pass_id, bool skinned = false) const;
    vk::PipelineLayout pipelineLayout(PassId pass_id, bool skinned = false) const;
};

} // namespace Pelican
