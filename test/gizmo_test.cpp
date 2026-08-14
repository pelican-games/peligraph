#include "../src/core/renderer/gizmo.hpp"
#include "../src/core/renderer/modaltransform.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {
namespace {

glm::mat4 testViewProjection() {
    return glm::perspectiveRH_ZO(0.7853981633974483f, 1.0f, 0.1f,
                                 100.0f) *
           glm::lookAtRH(glm::vec3{0.0f, 0.0f, -2.0f},
                         glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
}

glm::mat4 cameraViewProjection(glm::vec3 eye, glm::vec3 target,
                               glm::vec3 up) {
    return glm::perspectiveRH_ZO(0.7853981633974483f, 1.0f, 0.1f,
                                 100.0f) *
           glm::lookAtRH(eye, target, up);
}

bool hasVisibleHandle(const GizmoGeometry &geometry, GizmoHandle handle) {
    return std::any_of(
        geometry.segments.begin(), geometry.segments.end(),
        [handle](const auto &segment) {
            return segment.handle == handle &&
                   glm::length(segment.to.pixel - segment.from.pixel) > 1.0f;
        });
}

std::optional<glm::vec2> isolatedRotationZPoint(
    const GizmoGeometry &geometry) {
    const glm::vec2 center{static_cast<float>(geometry.extent.width) * 0.5f,
                           static_cast<float>(geometry.extent.height) * 0.5f};
    for (const auto &segment : geometry.segments) {
        if (segment.handle != GizmoHandle::rotate_z) continue;
        const auto midpoint = (segment.from.pixel + segment.to.pixel) * 0.5f;
        if (midpoint.x > center.x + 30.0f &&
            midpoint.y > center.y + 30.0f) {
            return midpoint;
        }
    }
    return std::nullopt;
}

const GizmoSegment &longestSegment(const GizmoGeometry &geometry,
                                   GizmoHandle handle) {
    const auto found = std::max_element(
        geometry.segments.begin(), geometry.segments.end(),
        [handle](const auto &left, const auto &right) {
            const auto length = [handle](const auto &segment) {
                return segment.handle == handle
                           ? glm::length(segment.to.pixel -
                                         segment.from.pixel)
                           : -1.0f;
            };
            return length(left) < length(right);
        });
    REQUIRE(found != geometry.segments.end());
    REQUIRE(found->handle == handle);
    return *found;
}

const GizmoDragProjection &dragFor(const GizmoGeometry &geometry,
                                   GizmoHandle handle) {
    const auto found = std::find_if(
        geometry.segments.begin(), geometry.segments.end(),
        [handle](const auto &segment) {
            return segment.handle == handle && segment.drag.has_value();
        });
    REQUIRE(found != geometry.segments.end());
    return *found->drag;
}

} // namespace

TEST_CASE("gizmo projects all three modes and identifies their handles",
          "[gizmo][wp274]") {
    const auto view_projection = testViewProjection();
    constexpr vk::Extent2D extent{192, 192};

    const auto translate = buildGizmoGeometry(
        GizmoMode::translate, {0.0f, 0.0f, 0.0f}, view_projection,
        extent, 1.0f);
    const auto rotate = buildGizmoGeometry(
        GizmoMode::rotate, {0.0f, 0.0f, 0.0f}, view_projection,
        extent, 1.0f);
    const auto scale = buildGizmoGeometry(
        GizmoMode::scale, {0.0f, 0.0f, 0.0f}, view_projection,
        extent, 1.0f);

    for (const auto handle : {
             GizmoHandle::translate_x,
             GizmoHandle::translate_y,
             GizmoHandle::translate_z,
         }) {
        REQUIRE(hasVisibleHandle(translate, handle));
    }
    for (const auto handle : {
             GizmoHandle::rotate_x,
             GizmoHandle::rotate_y,
             GizmoHandle::rotate_z,
         }) {
        REQUIRE(hasVisibleHandle(rotate, handle));
    }
    for (const auto handle : {
             GizmoHandle::scale_x,
             GizmoHandle::scale_y,
             GizmoHandle::scale_z,
         }) {
        REQUIRE(hasVisibleHandle(scale, handle));
    }

    const auto x_segment = std::find_if(
        translate.segments.begin(), translate.segments.end(),
        [](const auto &segment) {
            return segment.handle == GizmoHandle::translate_x;
        });
    REQUIRE(x_segment != translate.segments.end());
    const auto x_midpoint =
        (x_segment->from.pixel + x_segment->to.pixel) * 0.5f;
    REQUIRE(hitTestGizmo(translate, x_midpoint) ==
            GizmoHandle::translate_x);

    const auto rotation_point = isolatedRotationZPoint(rotate);
    REQUIRE(rotation_point.has_value());
    REQUIRE(hitTestGizmo(rotate, *rotation_point) ==
            GizmoHandle::rotate_z);
    REQUIRE_FALSE(hitTestGizmo(rotate, {0.0f, 0.0f}).has_value());
}

TEST_CASE("gizmo grab radius is wider than the raster line and follows DPI",
          "[gizmo][hit-test][dpi][wp274]") {
    REQUIRE(gizmoGrabRadiusLogicalPixels > 1.0f);
    REQUIRE(gizmoContentScale({200, 100}, {100, 50}) ==
            Catch::Approx(2.0f));
    REQUIRE(gizmoGrabRadiusPixels(1.0f) == Catch::Approx(10.0f));
    REQUIRE(gizmoGrabRadiusPixels(2.0f) == Catch::Approx(20.0f));

    const auto geometry = buildGizmoGeometry(
        GizmoMode::translate, {0.0f, 0.0f, 0.0f},
        testViewProjection(), {192, 192}, 1.0f);
    REQUIRE(geometry.grab_radius_pixels == Catch::Approx(10.0f));
    const auto segment = std::find_if(
        geometry.segments.begin(), geometry.segments.end(),
        [](const auto &candidate) {
            return candidate.handle == GizmoHandle::translate_x;
        });
    REQUIRE(segment != geometry.segments.end());
    const auto midpoint = (segment->from.pixel + segment->to.pixel) * 0.5f;
    auto direction = segment->to.pixel - segment->from.pixel;
    REQUIRE(glm::length(direction) > 0.0f);
    direction = glm::normalize(direction);
    const glm::vec2 perpendicular{-direction.y, direction.x};

    REQUIRE(hitTestGizmo(
                geometry,
                midpoint + perpendicular *
                               (geometry.grab_radius_pixels - 0.5f)) ==
            GizmoHandle::translate_x);
    REQUIRE_FALSE(hitTestGizmo(
                      geometry,
                      midpoint + perpendicular *
                                     (geometry.grab_radius_pixels + 1.5f))
                      .has_value());
}

TEST_CASE("gizmo drag projection follows three camera poses and drawn axes",
          "[gizmo][drag][projection][headless][wp276]") {
    struct Pose {
        glm::vec3 eye;
        glm::vec3 up;
        GizmoHandle handle;
    };
    constexpr std::array poses{
        Pose{{0.0f, 0.0f, -4.0f}, {0.0f, 1.0f, 0.0f},
             GizmoHandle::translate_x},
        Pose{{0.0f, 0.0f, -4.0f}, {0.0f, -1.0f, 0.0f},
             GizmoHandle::translate_y},
        Pose{{3.0f, 2.0f, -4.0f}, {0.0f, 1.0f, 0.0f},
             GizmoHandle::translate_z},
    };
    constexpr vk::Extent2D extent{640, 640};

    for (std::size_t index = 0; index < poses.size(); ++index) {
        CAPTURE(index);
        const auto &pose = poses[index];
        const auto geometry = buildGizmoGeometry(
            GizmoMode::translate, {0.0f, 0.0f, 0.0f},
            cameraViewProjection(pose.eye, {0.0f, 0.0f, 0.0f}, pose.up),
            extent, 1.0f);
        const auto &segment = longestSegment(geometry, pose.handle);
        const auto drawn_direction =
            glm::normalize(segment.to.pixel - segment.from.pixel);
        const auto midpoint =
            (segment.from.pixel + segment.to.pixel) * 0.5f;
        const auto hit = hitTestGizmoDrag(geometry, midpoint);
        REQUIRE(hit.has_value());
        REQUIRE(hit->handle == pose.handle);
        REQUIRE(glm::dot(hit->drag.direction, drawn_direction) ==
                Catch::Approx(1.0f).margin(1.0e-4f));
        REQUIRE(glm::length(hit->drag.direction) ==
                Catch::Approx(1.0f).margin(1.0e-4f));

        const glm::vec2 drag = drawn_direction * 24.0f;
        const float scalar = glm::dot(drag, hit->drag.direction) *
                             hit->drag.value_per_logical_pixel;
        REQUIRE(std::signbit(scalar) ==
                std::signbit(glm::dot(drag, drawn_direction)));
        REQUIRE(scalar > 0.0f);

        if (index == 0) {
            REQUIRE(drawn_direction.x < 0.0f);
        } else if (index == 1) {
            REQUIRE(drawn_direction.y < 0.0f);
        }
    }
}

TEST_CASE("gizmo translation magnitude grows with camera distance",
          "[gizmo][drag][distance][headless][wp276]") {
    constexpr vk::Extent2D extent{640, 640};
    const auto view_projection = cameraViewProjection(
        {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f});
    const auto near_geometry = buildGizmoGeometry(
        GizmoMode::translate, {0.0f, 0.0f, 2.0f}, view_projection,
        extent, 1.0f);
    const auto far_geometry = buildGizmoGeometry(
        GizmoMode::translate, {0.0f, 0.0f, 8.0f}, view_projection,
        extent, 1.0f);

    const float near_change =
        dragFor(near_geometry, GizmoHandle::translate_x)
            .value_per_logical_pixel;
    const float far_change =
        dragFor(far_geometry, GizmoHandle::translate_x)
            .value_per_logical_pixel;
    REQUIRE(std::isfinite(near_change));
    REQUIRE(std::isfinite(far_change));
    REQUIRE(far_change > near_change);
}

TEST_CASE("gizmo rotation uses the positive ring tangent",
          "[gizmo][drag][rotation][headless][wp276]") {
    const auto geometry = buildGizmoGeometry(
        GizmoMode::rotate, {0.0f, 0.0f, 0.0f}, testViewProjection(),
        {640, 640}, 1.0f);
    const auto segment = std::find_if(
        geometry.segments.begin(), geometry.segments.end(),
        [](const auto &candidate) {
            return candidate.handle == GizmoHandle::rotate_z &&
                   candidate.drag.has_value();
        });
    REQUIRE(segment != geometry.segments.end());
    const auto tangent =
        glm::normalize(segment->to.pixel - segment->from.pixel);
    REQUIRE(glm::dot(segment->drag->direction, tangent) ==
            Catch::Approx(1.0f).margin(1.0e-4f));
    REQUIRE(segment->drag->value_per_logical_pixel ==
            Catch::Approx(gizmoRotationRadiansPerLogicalPixel));
}

TEST_CASE("gizmo rejects a camera-facing degenerate axis for dragging",
          "[gizmo][drag][degenerate][headless][wp276]") {
    const auto geometry = buildGizmoGeometry(
        GizmoMode::translate, {0.0f, 0.0f, 0.0f},
        cameraViewProjection({-4.0f, 0.0f, 0.0f},
                             {0.0f, 0.0f, 0.0f},
                             {0.0f, 1.0f, 0.0f}),
        {640, 640}, 1.0f);
    const auto degenerate = std::find_if(
        geometry.segments.begin(), geometry.segments.end(),
        [](const auto &segment) {
            return segment.handle == GizmoHandle::translate_x;
        });
    REQUIRE(degenerate != geometry.segments.end());
    REQUIRE_FALSE(degenerate->drag.has_value());
    const auto midpoint =
        (degenerate->from.pixel + degenerate->to.pixel) * 0.5f;
    REQUIRE(hitTestGizmo(geometry, midpoint) == GizmoHandle::translate_x);
    REQUIRE_FALSE(hitTestGizmoDrag(geometry, midpoint).has_value());

    for (const auto &segment : geometry.segments) {
        if (!segment.drag) continue;
        REQUIRE(std::isfinite(segment.drag->direction.x));
        REQUIRE(std::isfinite(segment.drag->direction.y));
        REQUIRE(std::isfinite(segment.drag->value_per_logical_pixel));
    }
}

TEST_CASE("modal axis movement reuses the exact rendered handle projection",
          "[gizmo][modal][axis][negative-contrast][headless][wp286]") {
    constexpr vk::Extent2D extent{640, 640};
    const auto geometry = buildGizmoGeometry(
        GizmoMode::translate, {0.0f, 0.0f, 0.0f},
        cameraViewProjection({0.0f, 0.0f, -4.0f}, glm::vec3{0.0f},
                             {0.0f, 1.0f, 0.0f}),
        extent, 1.0f);
    const auto &segment = longestSegment(
        geometry, GizmoHandle::translate_x);
    const auto midpoint =
        (segment.from.pixel + segment.to.pixel) * 0.5f;
    const auto hit = hitTestGizmoDrag(geometry, midpoint);
    const auto named_axis = gizmoDragProjectionForAxis(
        geometry, GizmoMode::translate, GizmoAxis::x);
    REQUIRE(hit.has_value());
    REQUIRE(named_axis.has_value());
    REQUIRE(hit->handle == GizmoHandle::translate_x);
    REQUIRE(named_axis->direction.x ==
            Catch::Approx(hit->drag.direction.x));
    REQUIRE(named_axis->direction.y ==
            Catch::Approx(hit->drag.direction.y));
    REQUIRE(named_axis->value_per_logical_pixel ==
            Catch::Approx(hit->drag.value_per_logical_pixel));

    const GizmoSelection selection = GizmoDeclarationSelection{
        .scene_id = "default_scene", .declaration_index = 1};
    ModalTransformController modal;
    modal.update({.enabled = true,
                  .mode_pressed = GizmoMode::translate,
                  .selection = selection});
    modal.update({.enabled = true,
                  .axis_pressed = GizmoAxis::x,
                  .selection = selection,
                  .axis_projection = named_axis});
    constexpr float logical_pixels = 37.0f;
    const glm::vec2 pointer_delta =
        named_axis->direction * logical_pixels;
    modal.update({.enabled = true,
                  .pointer_delta_logical = pointer_delta,
                  .selection = selection});
    modal.update({.enabled = true,
                  .confirm_pressed = true,
                  .selection = selection});

    const float handle_change =
        glm::dot(pointer_delta, hit->drag.direction) *
        hit->drag.value_per_logical_pixel;
    const auto transformed = modal.snapshot();
    REQUIRE(transformed.phase == ModalTransformPhase::confirmed);
    REQUIRE(transformed.delta.translation.x ==
            Catch::Approx(handle_change).margin(1.0e-6f));
    REQUIRE(transformed.delta.translation.y == Catch::Approx(0.0f));
    REQUIRE(transformed.delta.translation.z == Catch::Approx(0.0f));

    // Rule 10 negative contrast: the identical action/axis/pointer sequence
    // with the editor action overlay disabled must remain idle and differ.
    ModalTransformController disabled;
    disabled.update({.enabled = false,
                     .mode_pressed = GizmoMode::translate,
                     .selection = selection});
    disabled.update({.enabled = false,
                     .axis_pressed = GizmoAxis::x,
                     .selection = selection,
                     .axis_projection = named_axis});
    disabled.update({.enabled = false,
                     .pointer_delta_logical = pointer_delta,
                     .selection = selection});
    disabled.update({.enabled = false,
                     .confirm_pressed = true,
                     .selection = selection});
    REQUIRE(disabled.snapshot().phase == ModalTransformPhase::idle);
    REQUIRE(disabled.snapshot().delta.translation.x == 0.0f);
    REQUIRE(disabled.snapshot().delta.translation.x !=
            transformed.delta.translation.x);
}

TEST_CASE("unconstrained modal translation follows the current camera plane",
          "[gizmo][modal][view-plane][negative-contrast][headless][wp286]") {
    constexpr vk::Extent2D extent{640, 640};
    const auto first = buildGizmoViewPlaneDragProjection(
        glm::vec3{0.0f},
        cameraViewProjection({0.0f, 0.0f, -4.0f}, glm::vec3{0.0f},
                             {0.0f, 1.0f, 0.0f}),
        {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}, extent, 1.0f);
    const auto orbited = buildGizmoViewPlaneDragProjection(
        glm::vec3{0.0f},
        cameraViewProjection({4.0f, 0.0f, 0.0f}, glm::vec3{0.0f},
                             {0.0f, 1.0f, 0.0f}),
        {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, extent, 1.0f);
    REQUIRE(first.has_value());
    REQUIRE(orbited.has_value());
    const GizmoSelection selection = GizmoDeclarationSelection{
        .scene_id = "default_scene", .declaration_index = 1};
    const glm::vec2 pointer{19.0f, 7.0f};

    const auto run = [&](const GizmoViewPlaneDragProjection &second_basis) {
        ModalTransformController controller;
        controller.update({.enabled = true,
                           .mode_pressed = GizmoMode::translate,
                           .selection = selection,
                           .view_plane_projection = *first});
        controller.update({.enabled = true,
                           .pointer_delta_logical = pointer,
                           .selection = selection,
                           .view_plane_projection = *first});
        controller.update({.enabled = true,
                           .pointer_delta_logical = pointer,
                           .selection = selection,
                           .view_plane_projection = second_basis});
        return controller.snapshot().delta.translation;
    };
    const glm::vec3 camera_following = run(*orbited);
    const glm::vec3 frozen_camera = run(*first);
    const glm::vec3 expected =
        first->world_per_logical_pixel_x * pointer.x +
        first->world_per_logical_pixel_y * pointer.y +
        orbited->world_per_logical_pixel_x * pointer.x +
        orbited->world_per_logical_pixel_y * pointer.y;
    REQUIRE(camera_following.x == Catch::Approx(expected.x));
    REQUIRE(camera_following.y == Catch::Approx(expected.y));
    REQUIRE(camera_following.z == Catch::Approx(expected.z));
    // Negative contrast is the same operation with camera following removed.
    REQUIRE(glm::length(camera_following - frozen_camera) > 1.0e-4f);
}

TEST_CASE("modal transform cancellation triggers share one terminal state and acknowledgement",
          "[gizmo][modal][cancel][negative-contrast][headless][wp286]") {
    const GizmoSelection selection = GizmoDeclarationSelection{
        .scene_id = "default_scene", .declaration_index = 1};
    const GizmoSelection other_selection = GizmoDeclarationSelection{
        .scene_id = "default_scene", .declaration_index = 2};
    const auto enter = [&] {
        ModalTransformController controller;
        controller.update({.enabled = true,
                           .mode_pressed = GizmoMode::translate,
                           .selection = selection});
        REQUIRE(controller.snapshot().phase ==
                ModalTransformPhase::active);
        return controller;
    };
    const auto terminalFor = [&](std::string_view trigger) {
        auto controller = enter();
        if (trigger == "escape") {
            controller.update({.enabled = true,
                               .cancel_pressed = true,
                               .selection = selection});
        } else if (trigger == "mode") {
            controller.update({.enabled = true,
                               .mode_pressed = GizmoMode::rotate,
                               .selection = selection});
        } else if (trigger == "selection") {
            controller.update({.enabled = true,
                               .selection = other_selection});
        } else if (trigger == "actions_disabled") {
            controller.update({.enabled = false,
                               .selection = selection});
        } else if (trigger == "player_stop") {
            controller.requestCancel("player_stopped");
        } else if (trigger == "control") {
            controller.update({.enabled = true,
                               .selection = selection});
        }
        return controller;
    };

    const auto control = terminalFor("control").snapshot();
    REQUIRE(control.phase == ModalTransformPhase::active);
    for (const auto trigger : {"escape", "mode", "selection",
                               "actions_disabled", "player_stop"}) {
        CAPTURE(trigger);
        auto controller = terminalFor(trigger);
        const auto terminal = controller.snapshot();
        REQUIRE(terminal.phase == ModalTransformPhase::cancelled);
        REQUIRE(terminal.phase != control.phase);
        REQUIRE_FALSE(terminal.reason.empty());
        REQUIRE(controller.acknowledge(terminal.operation_id,
                                       terminal.revision));
        REQUIRE(controller.snapshot().phase == ModalTransformPhase::idle);
    }
}

} // namespace Pelican
