#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(ShadowDepthPassContainer) {
    std::unordered_map<int, PipelineHandle> pipelines;

  public:
    PassId registerShadowDepthPass(vk::Format depth_format, ShaderBundleId vert_shader,
                                   std::vector<std::string> shader_defines = {});
    void bind(vk::CommandBuffer cmd_buf, PassId pass_id) const;
    vk::PipelineLayout pipelineLayout(PassId pass_id) const;
};

} // namespace Pelican
