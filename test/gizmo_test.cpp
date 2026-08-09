#include "../src/core/renderer/gizmo.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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

} // namespace Pelican
