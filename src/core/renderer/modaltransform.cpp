#include "modaltransform.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

constexpr float pointer_rotation_radians_per_logical_pixel = 0.01f;
constexpr float pointer_scale_exponent_per_logical_pixel = 0.01f;
constexpr float diagonal_normalizer = 0.70710678118654752440f;
constexpr float minimum_axis_length = 1.0e-6f;

glm::vec3 axisVector(GizmoAxis axis) noexcept {
    switch (axis) {
    case GizmoAxis::x: return {1.0f, 0.0f, 0.0f};
    case GizmoAxis::y: return {0.0f, 1.0f, 0.0f};
    case GizmoAxis::z: return {0.0f, 0.0f, 1.0f};
    }
    return {1.0f, 0.0f, 0.0f};
}

bool finite(glm::vec2 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(glm::vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(glm::quat value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

} // namespace

std::string_view modalTransformPhaseName(ModalTransformPhase phase) noexcept {
    switch (phase) {
    case ModalTransformPhase::idle: return "idle";
    case ModalTransformPhase::active: return "active";
    case ModalTransformPhase::confirmed: return "confirmed";
    case ModalTransformPhase::cancelled: return "cancelled";
    }
    return "idle";
}

void ModalTransformController::bumpRevision() {
    if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("modal transform revision exhausted");
    }
    ++snapshot_.revision;
}

void ModalTransformController::resetDelta() noexcept {
    snapshot_.delta = {};
}

void ModalTransformController::cancel(std::string reason) {
    if (snapshot_.phase != ModalTransformPhase::active) return;
    snapshot_.phase = ModalTransformPhase::cancelled;
    snapshot_.reason = std::move(reason);
    bumpRevision();
}

void ModalTransformController::update(
    const ModalTransformFrameInput &input) {
    if (snapshot_.enabled != input.enabled) {
        snapshot_.enabled = input.enabled;
        bumpRevision();
    }
    if (!input.enabled) {
        cancel("actions_disabled");
        return;
    }
    if (snapshot_.phase == ModalTransformPhase::confirmed ||
        snapshot_.phase == ModalTransformPhase::cancelled) {
        return;
    }

    if (snapshot_.phase == ModalTransformPhase::active) {
        if (!input.selection || !snapshot_.selection ||
            *input.selection != *snapshot_.selection) {
            cancel("selection_changed");
            return;
        }
        if (input.mode_pressed) {
            cancel(*input.mode_pressed == *snapshot_.mode
                       ? "mode_reselected"
                       : "mode_changed");
            return;
        }
    } else {
        if (!input.mode_pressed || !input.selection) return;
        if (next_operation_id_ == 0 ||
            next_operation_id_ > 9007199254740991ULL) {
            throw std::overflow_error(
                "modal transform operation identifier exhausted");
        }
        snapshot_.phase = ModalTransformPhase::active;
        snapshot_.operation_id = next_operation_id_++;
        snapshot_.selection = input.selection;
        snapshot_.mode = input.mode_pressed;
        snapshot_.axis.reset();
        snapshot_.reason.clear();
        axis_projection_.reset();
        resetDelta();
        bumpRevision();

        if (input.axis_pressed) {
            if (!input.axis_projection) {
                cancel("axis_degenerate");
                return;
            }
            snapshot_.axis = input.axis_pressed;
            axis_projection_ = input.axis_projection;
            bumpRevision();
        }
        // Pointer motion that happened before the entry action must not move
        // the object on the activation frame.
        return;
    }

    if (input.cancel_pressed) {
        cancel("cancel_action");
        return;
    }
    if (input.axis_pressed) {
        if (!input.axis_projection) {
            cancel("axis_degenerate");
            return;
        }
        snapshot_.axis = input.axis_pressed;
        axis_projection_ = input.axis_projection;
        resetDelta();
        bumpRevision();
        return;
    }

    if (!finite(input.pointer_delta_logical)) {
        cancel("non_finite_pointer_delta");
        return;
    }
    const auto pointer = input.pointer_delta_logical;
    bool changed = false;
    if (pointer.x != 0.0f || pointer.y != 0.0f) {
        if (snapshot_.axis) {
            if (!axis_projection_) {
                cancel("axis_degenerate");
                return;
            }
            const auto scalar =
                glm::dot(pointer, axis_projection_->direction) *
                axis_projection_->value_per_logical_pixel;
            const auto world_axis = axisVector(*snapshot_.axis);
            switch (*snapshot_.mode) {
            case GizmoMode::translate:
                snapshot_.delta.translation += world_axis * scalar;
                break;
            case GizmoMode::rotate:
                snapshot_.delta.rotation =
                    glm::angleAxis(scalar, world_axis) *
                    snapshot_.delta.rotation;
                break;
            case GizmoMode::scale:
                snapshot_.delta.scale_exponent += world_axis * scalar;
                break;
            }
            changed = scalar != 0.0f;
        } else {
            switch (*snapshot_.mode) {
            case GizmoMode::translate:
                if (!input.view_plane_projection) {
                    cancel("view_plane_degenerate");
                    return;
                }
                snapshot_.delta.translation +=
                    input.view_plane_projection->world_per_logical_pixel_x *
                        pointer.x +
                    input.view_plane_projection->world_per_logical_pixel_y *
                        pointer.y;
                changed = true;
                break;
            case GizmoMode::rotate: {
                const auto length = glm::length(input.view_rotation_axis);
                if (!std::isfinite(length) || length <= minimum_axis_length) {
                    cancel("view_axis_degenerate");
                    return;
                }
                const auto scalar =
                    (pointer.x - pointer.y) * diagonal_normalizer *
                    pointer_rotation_radians_per_logical_pixel;
                snapshot_.delta.rotation =
                    glm::angleAxis(scalar,
                                   input.view_rotation_axis / length) *
                    snapshot_.delta.rotation;
                changed = scalar != 0.0f;
                break;
            }
            case GizmoMode::scale: {
                const auto scalar =
                    (pointer.x - pointer.y) * diagonal_normalizer *
                    pointer_scale_exponent_per_logical_pixel;
                snapshot_.delta.scale_exponent += glm::vec3{scalar};
                changed = scalar != 0.0f;
                break;
            }
            }
        }
    }

    if (!finite(snapshot_.delta.translation) ||
        !finite(snapshot_.delta.rotation) ||
        !finite(snapshot_.delta.scale_exponent)) {
        cancel("non_finite_transform_delta");
        return;
    }
    if (changed) bumpRevision();

    if (input.confirm_pressed) {
        snapshot_.phase = ModalTransformPhase::confirmed;
        snapshot_.reason = "confirm_action";
        bumpRevision();
    }
}

void ModalTransformController::requestCancel(std::string reason) {
    if (reason.empty()) reason = "external_cancel";
    cancel(std::move(reason));
}

bool ModalTransformController::acknowledge(std::uint64_t operation_id,
                                           std::uint64_t revision) {
    if ((snapshot_.phase != ModalTransformPhase::confirmed &&
         snapshot_.phase != ModalTransformPhase::cancelled) ||
        operation_id != snapshot_.operation_id ||
        revision < snapshot_.revision) {
        return false;
    }
    snapshot_.phase = ModalTransformPhase::idle;
    snapshot_.operation_id = 0;
    snapshot_.selection.reset();
    snapshot_.mode.reset();
    snapshot_.axis.reset();
    snapshot_.reason.clear();
    axis_projection_.reset();
    resetDelta();
    bumpRevision();
    return true;
}

void ModalTransformState::update(const ModalTransformFrameInput &input) {
    const std::scoped_lock lock{mutex_};
    controller_.update(input);
}

void ModalTransformState::requestCancel(std::string reason) {
    const std::scoped_lock lock{mutex_};
    controller_.requestCancel(std::move(reason));
}

bool ModalTransformState::acknowledge(std::uint64_t operation_id,
                                      std::uint64_t revision) {
    const std::scoped_lock lock{mutex_};
    return controller_.acknowledge(operation_id, revision);
}

ModalTransformSnapshot ModalTransformState::snapshot() const {
    const std::scoped_lock lock{mutex_};
    return controller_.snapshot();
}

} // namespace Pelican
