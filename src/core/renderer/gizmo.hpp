#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "../vkcore/buf.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.hpp>

namespace Pelican {

class ECSCore;
class FrameResources;
class ProjectBasicConfig;
class SceneLoader;

enum class GizmoMode {
    translate,
    rotate,
    scale,
};

enum class GizmoAxis {
    x,
    y,
    z,
};

enum class GizmoHandle {
    translate_x,
    translate_y,
    translate_z,
    rotate_x,
    rotate_y,
    rotate_z,
    scale_x,
    scale_y,
    scale_z,
};

struct GizmoSelection {
    std::string scene_id;
    std::size_t declaration_index = 0;

    bool operator==(const GizmoSelection &) const = default;
};

struct GizmoDisplayRequest {
    GizmoSelection selection;
    GizmoMode mode = GizmoMode::translate;

    bool operator==(const GizmoDisplayRequest &) const = default;
};

struct GizmoTargetTransform {
    glm::vec3 position{0.0f};
};

struct GizmoProjectedVertex {
    glm::vec3 ndc{0.0f};
    glm::vec2 pixel{0.0f};
};

struct GizmoSegment {
    GizmoHandle handle = GizmoHandle::translate_x;
    GizmoProjectedVertex from;
    GizmoProjectedVertex to;
    glm::vec4 color{1.0f};
};

struct GizmoGeometry {
    vk::Extent2D extent{};
    float content_scale = 1.0f;
    float grab_radius_pixels = 10.0f;
    std::vector<GizmoSegment> segments;
};

// A physical 1 px raster line is deliberately easier to acquire than it is
// to see. The public hit query uses this logical-pixel radius independently
// of PipelineFactory's fixed lineWidth=1.0f.
inline constexpr float gizmoGrabRadiusLogicalPixels = 10.0f;
inline constexpr float gizmoAxisLengthLogicalPixels = 72.0f;

std::string_view gizmoModeName(GizmoMode mode) noexcept;
std::optional<GizmoMode> gizmoModeFromName(std::string_view name) noexcept;
std::string_view gizmoAxisName(GizmoAxis axis) noexcept;
GizmoAxis gizmoHandleAxis(GizmoHandle handle) noexcept;
std::string_view gizmoHandleName(GizmoHandle handle) noexcept;

float gizmoContentScale(vk::Extent2D framebuffer_extent,
                        vk::Extent2D logical_extent) noexcept;
float gizmoGrabRadiusPixels(float content_scale) noexcept;

GizmoGeometry buildGizmoGeometry(GizmoMode mode, glm::vec3 world_position,
                                 const glm::mat4 &view_projection,
                                 vk::Extent2D extent, float content_scale);
std::optional<GizmoHandle> hitTestGizmo(const GizmoGeometry &geometry,
                                        glm::vec2 pixel) noexcept;

// Resolve the sole public object identity through the session-stable
// authoring id already used by SceneLoader. No second public identity leaves
// this boundary.
std::optional<GizmoTargetTransform> resolveGizmoTargetTransform(
    const GizmoSelection &selection, const ProjectBasicConfig &project_config,
    const SceneLoader &scene_loader, ECSCore &ecs_core);

struct GizmoVertex {
    glm::vec4 position{0.0f};
    glm::vec4 color{1.0f};
};

DECLARE_MODULE(Gizmo) {
    struct PipelineRecord {
        PipelineHandle pipeline;
        vk::UniqueDescriptorSet descriptor_set;
    };

    vk::Device device;
    vk::UniqueDescriptorPool descriptor_pool;
    BufferWrapper vertex_buffer;
    vk::DeviceSize vertex_buffer_bytes = 0;
    std::unordered_map<PassId, PipelineRecord, PassId::Hash> pipelines;
    std::vector<PassId> registration_order;
    int next_pass_id = 0;

    mutable std::mutex display_mutex;
    std::optional<GizmoDisplayRequest> display_request;

    void ensureDevice();
    void ensureDescriptorPool();
    void ensureVertexCapacity(std::size_t vertex_count);
    void ensureDescriptorSet(PassId pass_id, PipelineRecord &record);
    void updateDescriptorSet(const PipelineRecord &record,
                             vk::DeviceSize bytes);

  public:
    struct RegistrationCheckpoint {
        std::size_t registration_count = 0;
        int next_pass_id = 0;
    };

    Gizmo();
    ~Gizmo();

    void setDisplayRequest(std::optional<GizmoDisplayRequest> request);
    std::optional<GizmoDisplayRequest> displayRequest() const;

    PassId registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                        ShaderBundleId frag_shader,
                        std::vector<std::string> shader_defines = {},
                        vk::SampleCountFlagBits samples =
                            vk::SampleCountFlagBits::e1);
    void render(vk::CommandBuffer cmd_buf, PassId pass_id,
                const FrameResources &frame_resources,
                const std::optional<glm::vec3> &world_position,
                const glm::mat4 &view_projection, vk::Extent2D extent,
                float content_scale);

    RegistrationCheckpoint checkpointRegistrations() const noexcept;
    void rollbackRegistrations(RegistrationCheckpoint checkpoint);
    std::vector<PassId>
    registrationsSince(RegistrationCheckpoint checkpoint) const;
    std::size_t registrationCount() const noexcept {
        return registration_order.size();
    }
    void retireRegistrations(const std::vector<PassId> &ids) noexcept;
};

} // namespace Pelican
