#pragma once

#include "../container.hpp"
#include "gizmo.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Pelican {

inline constexpr std::string_view modalTransformActionSet = "gizmo_modal";
inline constexpr std::string_view modalTranslateAction = "gizmo.translate";
inline constexpr std::string_view modalRotateAction = "gizmo.rotate";
inline constexpr std::string_view modalScaleAction = "gizmo.scale";
inline constexpr std::string_view modalAxisXAction = "gizmo.axis_x";
inline constexpr std::string_view modalAxisYAction = "gizmo.axis_y";
inline constexpr std::string_view modalAxisZAction = "gizmo.axis_z";
inline constexpr std::string_view modalConfirmAction = "gizmo.confirm";
inline constexpr std::string_view modalCancelAction = "gizmo.cancel";

enum class ModalTransformPhase {
    idle,
    active,
    confirmed,
    cancelled,
};

std::string_view modalTransformPhaseName(ModalTransformPhase phase) noexcept;

struct ModalTransformDelta {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale_exponent{0.0f};
};

struct ModalTransformSnapshot {
    bool enabled = false;
    std::uint64_t revision = 0;
    ModalTransformPhase phase = ModalTransformPhase::idle;
    std::uint64_t operation_id = 0;
    std::optional<GizmoSelection> selection;
    std::optional<GizmoMode> mode;
    std::optional<GizmoAxis> axis;
    ModalTransformDelta delta;
    std::string reason;
};

struct ModalTransformFrameInput {
    bool enabled = false;
    std::optional<GizmoMode> mode_pressed;
    std::optional<GizmoAxis> axis_pressed;
    bool confirm_pressed = false;
    bool cancel_pressed = false;
    glm::vec2 pointer_delta_logical{0.0f};
    std::optional<GizmoSelection> selection;
    std::optional<GizmoDragProjection> axis_projection;
    std::optional<GizmoViewPlaneDragProjection> view_plane_projection;
    glm::vec3 view_rotation_axis{0.0f, 0.0f, 1.0f};
};

// Deterministic engine-side state machine. It consumes named actions and a
// camera-resolved basis supplied by ModalTransformSystem; it has no Qt or
// authoring dependency.
class ModalTransformController {
    ModalTransformSnapshot snapshot_;
    std::uint64_t next_operation_id_ = 1;
    std::optional<GizmoDragProjection> axis_projection_;

    void bumpRevision();
    void resetDelta() noexcept;
    void cancel(std::string reason);

  public:
    void update(const ModalTransformFrameInput &input);
    void requestCancel(std::string reason);
    bool acknowledge(std::uint64_t operation_id,
                     std::uint64_t revision);
    const ModalTransformSnapshot &snapshot() const noexcept {
        return snapshot_;
    }
};

DECLARE_MODULE(ModalTransformState) {
    mutable std::mutex mutex_;
    ModalTransformController controller_;

  public:
    void update(const ModalTransformFrameInput &input);
    void requestCancel(std::string reason);
    bool acknowledge(std::uint64_t operation_id,
                     std::uint64_t revision);
    ModalTransformSnapshot snapshot() const;
};

} // namespace Pelican
