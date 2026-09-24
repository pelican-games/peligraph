#pragma once

#include "../container.hpp"
#include "../renderingpass/renderingpass.hpp"
#include "../shader/pipelinefactory.hpp"
#include "lineoverlaypassregistry.hpp"
#include <details/ecs/entity.hpp>

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
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

struct GizmoDeclarationSelection {
    std::string scene_id;
    std::size_t declaration_index = 0;

    bool operator==(const GizmoDeclarationSelection &) const = default;
};

struct GizmoRuntimeSelection {
    GameObjectId object_id = invalidGameObjectId;

    bool operator==(const GizmoRuntimeSelection &) const = default;
};

using GizmoSelection =
    std::variant<GizmoDeclarationSelection, GizmoRuntimeSelection>;

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

// Public, camera-resolved drag contract. direction is a unit vector in the
// same top-left-origin physical-pixel space used by query_gizmo_handle. The
// magnitude is expressed per logical pixel so every client can apply DPI
// scaling without reproducing the engine camera or projection calculation.
struct GizmoDragProjection {
    glm::vec2 direction{0.0f};
    float value_per_logical_pixel = 0.0f;
};

// Camera-resolved basis for unconstrained translation. Each vector is the
// world-space displacement caused by one logical pointer pixel on the matching
// screen axis. The basis is deliberately produced next to GizmoGeometry so no
// editor client needs to reproduce projection math.
struct GizmoViewPlaneDragProjection {
    glm::vec3 world_per_logical_pixel_x{0.0f};
    glm::vec3 world_per_logical_pixel_y{0.0f};
};

struct GizmoSegment {
    GizmoHandle handle = GizmoHandle::translate_x;
    GizmoProjectedVertex from;
    GizmoProjectedVertex to;
    glm::vec4 color{1.0f};
    std::optional<GizmoDragProjection> drag;
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
inline constexpr float gizmoRotationRadiansPerLogicalPixel = 0.01f;
inline constexpr float gizmoScaleExponentPerLogicalPixel = 0.01f;

struct GizmoHit {
    GizmoHandle handle = GizmoHandle::translate_x;
    GizmoDragProjection drag;
};

std::string_view gizmoModeName(GizmoMode mode) noexcept;
std::optional<GizmoMode> gizmoModeFromName(std::string_view name) noexcept;
std::string_view gizmoAxisName(GizmoAxis axis) noexcept;
std::optional<GizmoAxis> gizmoAxisFromName(std::string_view name) noexcept;
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
std::optional<GizmoHit> hitTestGizmoDrag(const GizmoGeometry &geometry,
                                        glm::vec2 pixel) noexcept;
// Returns the exact projection already attached to the rendered handle for
// the requested axis, without requiring a hit-test coordinate.
std::optional<GizmoDragProjection>
gizmoDragProjectionForAxis(const GizmoGeometry &geometry,
                           GizmoMode mode, GizmoAxis axis) noexcept;
std::optional<GizmoViewPlaneDragProjection>
buildGizmoViewPlaneDragProjection(glm::vec3 world_position,
                                  const glm::mat4 &view_projection,
                                  glm::vec3 camera_direction,
                                  glm::vec3 camera_up,
                                  vk::Extent2D extent,
                                  float content_scale) noexcept;

// Declaration selections retain the authoring-document/SceneLoader route.
// Runtime selections are process-local GameObjectIds and resolve directly in
// ECS; they must never be persisted or reused by a later execution.
std::optional<GizmoTargetTransform> resolveGizmoTargetTransform(
    const GizmoSelection &selection, const ProjectBasicConfig &project_config,
    const SceneLoader &scene_loader, ECSCore &ecs_core);

struct GizmoVertex {
    glm::vec4 position{0.0f};
    glm::vec4 color{1.0f};
};

// Uploaded to the GPU as raw bytes: the size is the sum of the members,
// so the struct has no padding for stale bytes to hide in.
static_assert(sizeof(GizmoVertex) == 32);

DECLARE_MODULE(Gizmo) {
    // The device, descriptor pool, vertex buffer and pass table are the same
    // plumbing DebugDraw and DebugText need; only the handle geometry below
    // is ours.
    LineOverlayPassRegistry passes{LineOverlayRegistryTraits{
        .label = "Gizmo",
        .vertex_stride = sizeof(GizmoVertex),
        .initial_vertex_count = 256,
        .contract = AuxiliaryDescriptorContract::vertex_buffer,
        .topology = vk::PrimitiveTopology::eLineList,
        .sampled_image = false,
        .tracks_enabled = false,
    }};

    mutable std::mutex display_mutex;
    // This process-memory-only request intentionally has no serialization
    // path: a runtime GameObjectId is not stable across executions.
    std::optional<GizmoDisplayRequest> display_request;

  public:
    using RegistrationCheckpoint = LineOverlayPassRegistry::Checkpoint;

    Gizmo();
    ~Gizmo();

    void setDisplayRequest(std::optional<GizmoDisplayRequest> request);
    std::optional<GizmoDisplayRequest> displayRequest() const;

    PassId registerPass(vk::Format color_format, ShaderBundleId vert_shader,
                        ShaderBundleId frag_shader,
                        std::vector<std::string> shader_defines = {},
                        vk::SampleCountFlagBits samples =
                            vk::SampleCountFlagBits::e1,
                        bool ray_query = false,
                        std::string acceleration_structure = "scene");
    void render(vk::CommandBuffer cmd_buf, PassId pass_id,
                const FrameResources &frame_resources,
                const std::optional<glm::vec3> &world_position,
                const glm::mat4 &view_projection, vk::Extent2D extent,
                float content_scale);

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
    void retireRegistrations(const std::vector<PassId> &ids) noexcept;
};

} // namespace Pelican
