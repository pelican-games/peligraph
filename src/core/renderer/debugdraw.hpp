#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "lineoverlaypassregistry.hpp"
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class FrameResources;

struct DebugDrawVertex {
    glm::vec4 position;
    glm::vec4 color;
};

// Uploaded to the GPU as raw bytes: the size is the sum of the members,
// so the struct has no padding for stale bytes to hide in.
static_assert(sizeof(DebugDrawVertex) == 32);

DECLARE_MODULE(DebugDraw) {
    // The device, descriptor pool, vertex buffer and pass table are the same
    // plumbing DebugText and Gizmo need; only the vertices below are ours.
    LineOverlayPassRegistry passes{LineOverlayRegistryTraits{
        .label = "DebugDraw",
        .vertex_stride = sizeof(DebugDrawVertex),
        .initial_vertex_count = 64,
        .contract = AuxiliaryDescriptorContract::vertex_buffer,
        .topology = vk::PrimitiveTopology::eLineList,
        .sampled_image = false,
        .tracks_enabled = true,
    }};
    std::vector<DebugDrawVertex> vertices;

  public:
    using RegistrationCheckpoint = LineOverlayPassRegistry::Checkpoint;

    DebugDraw();
    ~DebugDraw();

    PassId registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                        ShaderBundleId frag_shader,
                        std::vector<std::string> shader_defines = {},
                        vk::SampleCountFlagBits samples =
                            vk::SampleCountFlagBits::e1,
                        bool ray_query = false,
                        std::string acceleration_structure = "scene");
    void line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 color);
    void line(glm::vec3 from_ndc, glm::vec3 to_ndc, glm::vec4 from_color, glm::vec4 to_color);
    void clear();
    void render(vk::CommandBuffer cmd_buf, PassId pass_id, const FrameResources &frame_resources);

    bool isEnabledForTesting() const { return passes.enabled(); }
    size_t queuedVertexCountForTesting() const { return vertices.size(); }

    RegistrationCheckpoint checkpointRegistrations() const noexcept {
        return passes.checkpointRegistrations();
    }
    void rollbackRegistrations(RegistrationCheckpoint checkpoint) {
        passes.rollbackRegistrations(checkpoint);
    }
    std::vector<PassId>
    registrationsSince(RegistrationCheckpoint checkpoint) const {
        return passes.registrationsSince(checkpoint);
    }
    std::size_t registrationCount() const noexcept {
        return passes.registrationCount();
    }
    void retireRegistrations(
        const std::vector<PassId> &ids) noexcept {
        passes.retireRegistrations(ids);
    }
};

} // namespace Pelican
