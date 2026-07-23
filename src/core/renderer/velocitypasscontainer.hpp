#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

DECLARE_MODULE(VelocityPassContainer) {
  public:
    struct PipelineVariants { PipelineHandle regular; PipelineHandle skinned; };

  private:
    std::unordered_map<int, PipelineVariants> pipelines;
    std::vector<PassId> registration_order;

  public:
    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
    };

    PassId registerVelocityPass(vk::Format color_format, vk::Format depth_format,
                                ShaderBundleId regular_vert, ShaderBundleId skinned_vert,
                                ShaderBundleId frag,
                                std::vector<std::string> shader_defines = {},
                                vk::SampleCountFlagBits samples =
                                    vk::SampleCountFlagBits::e1);
    void bind(vk::CommandBuffer cmd_buf, PassId pass_id, bool skinned = false) const;
    vk::PipelineLayout pipelineLayout(PassId pass_id, bool skinned = false) const;

    RegistrationCheckpoint checkpointRegistrations() const noexcept;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<PassId>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
};

} // namespace Pelican
