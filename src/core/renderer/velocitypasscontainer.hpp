#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <unordered_map>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(VelocityPassContainer) {
  public:
    struct PipelineVariants { PipelineHandle regular; PipelineHandle skinned; };

  private:
    std::unordered_map<int, PipelineVariants> pipelines;

  public:
    PassId registerVelocityPass(vk::Format color_format, vk::Format depth_format,
                                ShaderBundleId regular_vert, ShaderBundleId skinned_vert,
                                ShaderBundleId frag,
                                std::vector<std::string> shader_defines = {});
    void bind(vk::CommandBuffer cmd_buf, PassId pass_id, bool skinned = false) const;
    vk::PipelineLayout pipelineLayout(PassId pass_id, bool skinned = false) const;
};

} // namespace Pelican
