#include "gizmo.hpp"

#include "frameresources.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/scene.hpp"
#include "../shader/pelican_sets.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace Pelican {

namespace {

constexpr float pi = 3.14159265358979323846f;
constexpr float minimum_clip_w = 1.0e-5f;
constexpr float minimum_projected_derivative = 1.0e-4f;
constexpr float innerGapLogicalPixels = 10.0f;
constexpr float arrowLengthLogicalPixels = 10.0f;
constexpr float arrowHalfWidthLogicalPixels = 5.0f;
constexpr float scaleCapHalfSizeLogicalPixels = 4.5f;
constexpr float minimumLinearHandleLogicalPixels = 8.0f;
constexpr std::size_t rotationSegmentCount = 64;

constexpr std::array<glm::vec3, 3> axes{
    glm::vec3{1.0f, 0.0f, 0.0f},
    glm::vec3{0.0f, 1.0f, 0.0f},
    glm::vec3{0.0f, 0.0f, 1.0f},
};

constexpr std::array<glm::vec4, 3> axisColors{
    glm::vec4{0.95f, 0.18f, 0.16f, 1.0f},
    glm::vec4{0.20f, 0.85f, 0.28f, 1.0f},
    glm::vec4{0.18f, 0.42f, 1.0f, 1.0f},
};

bool finite(glm::vec2 value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool finite(glm::vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(glm::vec4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

std::optional<GizmoProjectedVertex> projectPoint(
    glm::vec3 world, const glm::mat4 &view_projection, vk::Extent2D extent) {
    const auto clip = view_projection * glm::vec4{world, 1.0f};
    if (!finite(clip) || clip.w <= minimum_clip_w || extent.width == 0 ||
        extent.height == 0) {
        return std::nullopt;
    }
    const glm::vec3 ndc = glm::vec3{clip} / clip.w;
    if (!finite(ndc)) return std::nullopt;
    return GizmoProjectedVertex{
        .ndc = ndc,
        .pixel = {
            (ndc.x + 1.0f) * 0.5f * static_cast<float>(extent.width),
            (ndc.y + 1.0f) * 0.5f * static_cast<float>(extent.height),
        },
    };
}

GizmoProjectedVertex vertexAtPixel(glm::vec2 pixel, float ndc_z,
                                   vk::Extent2D extent) {
    return GizmoProjectedVertex{
        .ndc = {
            pixel.x * 2.0f / static_cast<float>(extent.width) - 1.0f,
            pixel.y * 2.0f / static_cast<float>(extent.height) - 1.0f,
            ndc_z,
        },
        .pixel = pixel,
    };
}

GizmoHandle handleFor(GizmoMode mode, std::size_t axis) {
    static constexpr GizmoHandle handles[3][3]{
        {GizmoHandle::translate_x, GizmoHandle::translate_y,
         GizmoHandle::translate_z},
        {GizmoHandle::rotate_x, GizmoHandle::rotate_y,
         GizmoHandle::rotate_z},
        {GizmoHandle::scale_x, GizmoHandle::scale_y,
         GizmoHandle::scale_z},
    };
    return handles[static_cast<std::size_t>(mode)][axis];
}

void appendSegment(
    GizmoGeometry &geometry, GizmoHandle handle,
    GizmoProjectedVertex from, GizmoProjectedVertex to, glm::vec4 color,
    std::optional<GizmoDragProjection> drag = std::nullopt) {
    if (!finite(from.pixel) || !finite(to.pixel) || !finite(from.ndc) ||
        !finite(to.ndc)) {
        return;
    }
    if (drag &&
        (!finite(drag->direction) ||
         !std::isfinite(drag->value_per_logical_pixel) ||
         drag->value_per_logical_pixel <= 0.0f ||
         glm::length(drag->direction) <= minimum_projected_derivative)) {
        drag.reset();
    }
    geometry.segments.push_back(GizmoSegment{
        .handle = handle,
        .from = from,
        .to = to,
        .color = color,
        .drag = drag,
    });
}

void appendLinearFallbackMarker(GizmoGeometry &geometry, GizmoMode mode,
                                GizmoHandle handle,
                                const GizmoProjectedVertex &pivot,
                                glm::vec4 color) {
    const auto half = scaleCapHalfSizeLogicalPixels * geometry.content_scale;
    std::array<glm::vec2, 4> points;
    if (mode == GizmoMode::translate) {
        points = {
            pivot.pixel + glm::vec2{0.0f, -half},
            pivot.pixel + glm::vec2{half, 0.0f},
            pivot.pixel + glm::vec2{0.0f, half},
            pivot.pixel + glm::vec2{-half, 0.0f},
        };
    } else {
        points = {
            pivot.pixel + glm::vec2{-half, -half},
            pivot.pixel + glm::vec2{half, -half},
            pivot.pixel + glm::vec2{half, half},
            pivot.pixel + glm::vec2{-half, half},
        };
    }
    for (std::size_t index = 0; index < points.size(); ++index) {
        appendSegment(
            geometry, handle,
            vertexAtPixel(points[index], pivot.ndc.z, geometry.extent),
            vertexAtPixel(points[(index + 1) % points.size()], pivot.ndc.z,
                          geometry.extent),
            color);
    }
}

std::optional<glm::vec2> projectedPixelDerivative(
    glm::vec3 world, glm::vec3 axis, const glm::mat4 &view_projection,
    vk::Extent2D extent) {
    const auto clip = view_projection * glm::vec4{world, 1.0f};
    if (!finite(clip) || clip.w <= minimum_clip_w) return std::nullopt;

    const auto delta = view_projection * glm::vec4{axis, 0.0f};
    const auto denominator = clip.w * clip.w;
    const glm::vec2 derivative_ndc{
        (delta.x * clip.w - clip.x * delta.w) / denominator,
        (delta.y * clip.w - clip.y * delta.w) / denominator,
    };
    const glm::vec2 derivative_pixels{
        derivative_ndc.x * 0.5f * static_cast<float>(extent.width),
        derivative_ndc.y * 0.5f * static_cast<float>(extent.height),
    };
    if (!finite(derivative_pixels)) return std::nullopt;
    return derivative_pixels;
}

float projectedPixelsPerWorldUnit(glm::vec3 world,
                                  const glm::mat4 &view_projection,
                                  vk::Extent2D extent) {
    float largest = 0.0f;
    for (const auto axis : axes) {
        if (const auto derivative = projectedPixelDerivative(
                world, axis, view_projection, extent)) {
            largest = std::max(largest, glm::length(*derivative));
        }
    }
    return largest;
}

void appendLinearHandles(GizmoGeometry &geometry, GizmoMode mode,
                         glm::vec3 world_position,
                         const glm::mat4 &view_projection,
                         float world_units_per_pixel) {
    const auto scale = geometry.content_scale;
    const auto inner_world =
        innerGapLogicalPixels * scale * world_units_per_pixel;
    const auto outer_world =
        gizmoAxisLengthLogicalPixels * scale * world_units_per_pixel;
    const auto pivot =
        projectPoint(world_position, view_projection, geometry.extent);
    if (!pivot) return;

    for (std::size_t axis_index = 0; axis_index < axes.size(); ++axis_index) {
        const auto handle = handleFor(mode, axis_index);
        const auto color = axisColors[axis_index];
        const auto from = projectPoint(world_position + axes[axis_index] * inner_world,
                                       view_projection, geometry.extent);
        const auto to = projectPoint(world_position + axes[axis_index] * outer_world,
                                     view_projection, geometry.extent);
        if (!from || !to) continue;

        auto direction = to->pixel - from->pixel;
        const auto length = glm::length(direction);
        const auto derivative = projectedPixelDerivative(
            world_position, axes[axis_index], view_projection,
            geometry.extent);
        if (length < minimumLinearHandleLogicalPixels * scale ||
            !derivative) {
            appendLinearFallbackMarker(geometry, mode, handle, *pivot,
                                       color);
            continue;
        }
        direction /= length;
        const auto projected_pixels_per_world =
            glm::dot(*derivative, direction);
        if (!std::isfinite(projected_pixels_per_world) ||
            projected_pixels_per_world <= minimum_projected_derivative) {
            appendLinearFallbackMarker(geometry, mode, handle, *pivot,
                                       color);
            continue;
        }
        const auto value_per_logical_pixel =
            mode == GizmoMode::translate
                ? geometry.content_scale / projected_pixels_per_world
                : gizmoScaleExponentPerLogicalPixel;
        const GizmoDragProjection drag{
            .direction = direction,
            .value_per_logical_pixel = value_per_logical_pixel,
        };
        appendSegment(geometry, handle, *from, *to, color, drag);
        const glm::vec2 perpendicular{-direction.y, direction.x};

        if (mode == GizmoMode::translate) {
            const auto base =
                to->pixel - direction * arrowLengthLogicalPixels * scale;
            const auto half_width = arrowHalfWidthLogicalPixels * scale;
            appendSegment(
                geometry, handle, *to,
                vertexAtPixel(base + perpendicular * half_width, to->ndc.z,
                              geometry.extent),
                color, drag);
            appendSegment(
                geometry, handle, *to,
                vertexAtPixel(base - perpendicular * half_width, to->ndc.z,
                              geometry.extent),
                color, drag);
        } else {
            const auto half = scaleCapHalfSizeLogicalPixels * scale;
            const auto a = to->pixel + direction * half + perpendicular * half;
            const auto b = to->pixel + direction * half - perpendicular * half;
            const auto c = to->pixel - direction * half - perpendicular * half;
            const auto d = to->pixel - direction * half + perpendicular * half;
            appendSegment(geometry, handle,
                           vertexAtPixel(a, to->ndc.z, geometry.extent),
                           vertexAtPixel(b, to->ndc.z, geometry.extent), color,
                           drag);
            appendSegment(geometry, handle,
                           vertexAtPixel(b, to->ndc.z, geometry.extent),
                           vertexAtPixel(c, to->ndc.z, geometry.extent), color,
                           drag);
            appendSegment(geometry, handle,
                           vertexAtPixel(c, to->ndc.z, geometry.extent),
                           vertexAtPixel(d, to->ndc.z, geometry.extent), color,
                           drag);
            appendSegment(geometry, handle,
                           vertexAtPixel(d, to->ndc.z, geometry.extent),
                           vertexAtPixel(a, to->ndc.z, geometry.extent), color,
                           drag);
        }
    }
}

void appendRotationHandles(GizmoGeometry &geometry, glm::vec3 world_position,
                           const glm::mat4 &view_projection,
                           float world_units_per_pixel) {
    const auto radius = gizmoAxisLengthLogicalPixels * geometry.content_scale *
                        world_units_per_pixel;
    static constexpr std::array<std::array<glm::vec3, 2>, 3> bases{
        std::array{axes[1], axes[2]},
        std::array{axes[2], axes[0]},
        std::array{axes[0], axes[1]},
    };

    for (std::size_t axis_index = 0; axis_index < axes.size(); ++axis_index) {
        const auto handle = handleFor(GizmoMode::rotate, axis_index);
        const auto color = axisColors[axis_index];
        for (std::size_t segment = 0; segment < rotationSegmentCount;
             ++segment) {
            const auto angle_a = 2.0f * pi * static_cast<float>(segment) /
                                 static_cast<float>(rotationSegmentCount);
            const auto angle_b =
                2.0f * pi * static_cast<float>(segment + 1) /
                static_cast<float>(rotationSegmentCount);
            const auto point = [&](float angle) {
                return world_position +
                       radius * (bases[axis_index][0] * std::cos(angle) +
                                 bases[axis_index][1] * std::sin(angle));
            };
            const auto from =
                projectPoint(point(angle_a), view_projection, geometry.extent);
            const auto to =
                projectPoint(point(angle_b), view_projection, geometry.extent);
            if (from && to) {
                const auto tangent = to->pixel - from->pixel;
                const auto tangent_length = glm::length(tangent);
                std::optional<GizmoDragProjection> drag;
                if (tangent_length > minimum_projected_derivative) {
                    drag = GizmoDragProjection{
                        .direction = tangent / tangent_length,
                        .value_per_logical_pixel =
                            gizmoRotationRadiansPerLogicalPixel,
                    };
                }
                appendSegment(geometry, handle, *from, *to, color, drag);
            }
        }
    }
}

float squaredDistanceToSegment(glm::vec2 point, glm::vec2 from,
                               glm::vec2 to) {
    const auto delta = to - from;
    const auto length_squared = glm::dot(delta, delta);
    if (length_squared <= std::numeric_limits<float>::epsilon()) {
        const auto distance = point - from;
        return glm::dot(distance, distance);
    }
    const auto t = std::clamp(glm::dot(point - from, delta) / length_squared,
                              0.0f, 1.0f);
    const auto distance = point - (from + delta * t);
    return glm::dot(distance, distance);
}

const GizmoSegment *nearestGizmoSegment(const GizmoGeometry &geometry,
                                        glm::vec2 pixel) noexcept {
    if (!finite(pixel) || geometry.segments.empty()) return nullptr;
    const auto radius_squared = geometry.grab_radius_pixels *
                                geometry.grab_radius_pixels;
    auto best_distance = std::numeric_limits<float>::max();
    const GizmoSegment *best = nullptr;
    for (const auto &segment : geometry.segments) {
        const auto distance = squaredDistanceToSegment(
            pixel, segment.from.pixel, segment.to.pixel);
        if (distance <= radius_squared && distance < best_distance) {
            best_distance = distance;
            best = &segment;
        }
    }
    return best;
}

vk::UniqueDescriptorPool createDescriptorPool(vk::Device device) {
    vk::DescriptorPoolSize pool_size;
    pool_size.type = vk::DescriptorType::eStorageBuffer;
    pool_size.descriptorCount = 32;

    vk::DescriptorPoolCreateInfo create_info;
    create_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    create_info.maxSets = 32;
    create_info.poolSizeCount = 1;
    create_info.pPoolSizes = &pool_size;
    return device.createDescriptorPoolUnique(create_info);
}

vk::DeviceSize nextCapacity(vk::DeviceSize required) {
    vk::DeviceSize capacity = sizeof(GizmoVertex) * 256;
    while (capacity < required) capacity *= 2;
    return capacity;
}

} // namespace

std::string_view gizmoModeName(GizmoMode mode) noexcept {
    switch (mode) {
    case GizmoMode::translate:
        return "translate";
    case GizmoMode::rotate:
        return "rotate";
    case GizmoMode::scale:
        return "scale";
    }
    return "translate";
}

std::optional<GizmoMode> gizmoModeFromName(std::string_view name) noexcept {
    if (name == "translate") return GizmoMode::translate;
    if (name == "rotate") return GizmoMode::rotate;
    if (name == "scale") return GizmoMode::scale;
    return std::nullopt;
}

std::string_view gizmoAxisName(GizmoAxis axis) noexcept {
    switch (axis) {
    case GizmoAxis::x:
        return "x";
    case GizmoAxis::y:
        return "y";
    case GizmoAxis::z:
        return "z";
    }
    return "x";
}

std::optional<GizmoAxis> gizmoAxisFromName(std::string_view name) noexcept {
    if (name == "x") return GizmoAxis::x;
    if (name == "y") return GizmoAxis::y;
    if (name == "z") return GizmoAxis::z;
    return std::nullopt;
}

GizmoAxis gizmoHandleAxis(GizmoHandle handle) noexcept {
    switch (handle) {
    case GizmoHandle::translate_x:
    case GizmoHandle::rotate_x:
    case GizmoHandle::scale_x:
        return GizmoAxis::x;
    case GizmoHandle::translate_y:
    case GizmoHandle::rotate_y:
    case GizmoHandle::scale_y:
        return GizmoAxis::y;
    case GizmoHandle::translate_z:
    case GizmoHandle::rotate_z:
    case GizmoHandle::scale_z:
        return GizmoAxis::z;
    }
    return GizmoAxis::x;
}

std::string_view gizmoHandleName(GizmoHandle handle) noexcept {
    switch (handle) {
    case GizmoHandle::translate_x:
        return "translate_x";
    case GizmoHandle::translate_y:
        return "translate_y";
    case GizmoHandle::translate_z:
        return "translate_z";
    case GizmoHandle::rotate_x:
        return "rotate_x";
    case GizmoHandle::rotate_y:
        return "rotate_y";
    case GizmoHandle::rotate_z:
        return "rotate_z";
    case GizmoHandle::scale_x:
        return "scale_x";
    case GizmoHandle::scale_y:
        return "scale_y";
    case GizmoHandle::scale_z:
        return "scale_z";
    }
    return "translate_x";
}

float gizmoContentScale(vk::Extent2D framebuffer_extent,
                        vk::Extent2D logical_extent) noexcept {
    if (framebuffer_extent.width == 0 || framebuffer_extent.height == 0 ||
        logical_extent.width == 0 || logical_extent.height == 0) {
        return 1.0f;
    }
    const auto x = static_cast<float>(framebuffer_extent.width) /
                   static_cast<float>(logical_extent.width);
    const auto y = static_cast<float>(framebuffer_extent.height) /
                   static_cast<float>(logical_extent.height);
    const auto scale = (x + y) * 0.5f;
    if (!std::isfinite(scale) || scale <= 0.0f) return 1.0f;
    return std::clamp(scale, 0.5f, 4.0f);
}

float gizmoGrabRadiusPixels(float content_scale) noexcept {
    if (!std::isfinite(content_scale) || content_scale <= 0.0f) {
        content_scale = 1.0f;
    }
    return gizmoGrabRadiusLogicalPixels *
           std::clamp(content_scale, 0.5f, 4.0f);
}

GizmoGeometry buildGizmoGeometry(GizmoMode mode, glm::vec3 world_position,
                                 const glm::mat4 &view_projection,
                                 vk::Extent2D extent, float content_scale) {
    GizmoGeometry geometry{
        .extent = extent,
        .content_scale =
            std::isfinite(content_scale) && content_scale > 0.0f
                ? std::clamp(content_scale, 0.5f, 4.0f)
                : 1.0f,
    };
    geometry.grab_radius_pixels =
        gizmoGrabRadiusPixels(geometry.content_scale);
    if (extent.width == 0 || extent.height == 0) return geometry;

    const auto pivot = projectPoint(world_position, view_projection, extent);
    if (!pivot || pivot->ndc.z < 0.0f || pivot->ndc.z > 1.0f) {
        return geometry;
    }
    const auto pixels_per_world = projectedPixelsPerWorldUnit(
        world_position, view_projection, extent);
    if (pixels_per_world <= minimum_projected_derivative) return geometry;
    const auto world_units_per_pixel = 1.0f / pixels_per_world;

    if (mode == GizmoMode::rotate) {
        appendRotationHandles(geometry, world_position, view_projection,
                              world_units_per_pixel);
    } else {
        appendLinearHandles(geometry, mode, world_position, view_projection,
                            world_units_per_pixel);
    }
    return geometry;
}

std::optional<GizmoHandle> hitTestGizmo(const GizmoGeometry &geometry,
                                        glm::vec2 pixel) noexcept {
    const auto *segment = nearestGizmoSegment(geometry, pixel);
    return segment ? std::optional{segment->handle} : std::nullopt;
}

std::optional<GizmoHit> hitTestGizmoDrag(const GizmoGeometry &geometry,
                                        glm::vec2 pixel) noexcept {
    const auto *segment = nearestGizmoSegment(geometry, pixel);
    if (segment == nullptr || !segment->drag) return std::nullopt;
    return GizmoHit{
        .handle = segment->handle,
        .drag = *segment->drag,
    };
}

std::optional<GizmoDragProjection>
gizmoDragProjectionForAxis(const GizmoGeometry &geometry,
                           GizmoMode mode, GizmoAxis axis) noexcept {
    const auto handle =
        handleFor(mode, static_cast<std::size_t>(axis));
    const auto segment = std::find_if(
        geometry.segments.begin(), geometry.segments.end(),
        [handle](const GizmoSegment &candidate) {
            return candidate.handle == handle && candidate.drag.has_value();
        });
    if (segment == geometry.segments.end()) return std::nullopt;
    return segment->drag;
}

std::optional<GizmoViewPlaneDragProjection>
buildGizmoViewPlaneDragProjection(glm::vec3 world_position,
                                  const glm::mat4 &view_projection,
                                  glm::vec3 camera_direction,
                                  glm::vec3 camera_up,
                                  vk::Extent2D extent,
                                  float content_scale) noexcept {
    if (extent.width == 0 || extent.height == 0 ||
        !finite(world_position) || !finite(camera_direction) ||
        !finite(camera_up)) {
        return std::nullopt;
    }

    const auto direction_length = glm::length(camera_direction);
    if (!std::isfinite(direction_length) ||
        direction_length <= minimum_projected_derivative) {
        return std::nullopt;
    }
    const auto direction = camera_direction / direction_length;
    auto right = glm::cross(camera_up, direction);
    const auto right_length = glm::length(right);
    if (!std::isfinite(right_length) ||
        right_length <= minimum_projected_derivative) {
        return std::nullopt;
    }
    right /= right_length;
    auto up = glm::cross(direction, right);
    const auto up_length = glm::length(up);
    if (!std::isfinite(up_length) ||
        up_length <= minimum_projected_derivative) {
        return std::nullopt;
    }
    up /= up_length;

    const auto right_pixels = projectedPixelDerivative(
        world_position, right, view_projection, extent);
    const auto up_pixels = projectedPixelDerivative(
        world_position, up, view_projection, extent);
    if (!right_pixels || !up_pixels) return std::nullopt;

    const auto determinant = right_pixels->x * up_pixels->y -
                             up_pixels->x * right_pixels->y;
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= minimum_projected_derivative) {
        return std::nullopt;
    }
    const auto scale =
        std::isfinite(content_scale) && content_scale > 0.0f
            ? std::clamp(content_scale, 0.5f, 4.0f)
            : 1.0f;
    const auto right_for_x = up_pixels->y * scale / determinant;
    const auto up_for_x = -right_pixels->y * scale / determinant;
    const auto right_for_y = -up_pixels->x * scale / determinant;
    const auto up_for_y = right_pixels->x * scale / determinant;
    const GizmoViewPlaneDragProjection result{
        .world_per_logical_pixel_x =
            right * right_for_x + up * up_for_x,
        .world_per_logical_pixel_y =
            right * right_for_y + up * up_for_y,
    };
    if (!finite(result.world_per_logical_pixel_x) ||
        !finite(result.world_per_logical_pixel_y)) {
        return std::nullopt;
    }
    return result;
}

std::optional<GizmoTargetTransform> resolveGizmoTargetTransform(
    const GizmoSelection &selection, const ProjectBasicConfig &project_config,
    const SceneLoader &scene_loader, ECSCore &ecs_core) {
    if (const auto *runtime =
            std::get_if<GizmoRuntimeSelection>(&selection)) {
        const auto *transform = ecs_core.getTemplatePublicModule()
                                    .tryComponent<TransformComponent>(
                                        runtime->object_id);
        if (transform == nullptr) return std::nullopt;
        return GizmoTargetTransform{.position = transform->pos};
    }

    const auto &declaration =
        std::get<GizmoDeclarationSelection>(selection);
    if (declaration.scene_id != scene_loader.currentScene()) {
        return std::nullopt;
    }

    std::optional<AuthoringObjectId> authoring_object_id;
    for (const auto &scene : project_config.resolvedScene().scenes()) {
        if (scene.scene_id != declaration.scene_id) continue;
        const auto object = std::find_if(
            scene.objects.begin(), scene.objects.end(), [&](const auto &candidate) {
                return candidate.authoring_object_index ==
                       declaration.declaration_index;
            });
        if (object != scene.objects.end()) {
            authoring_object_id = object->authoring_object_id;
        }
        break;
    }
    if (!authoring_object_id) return std::nullopt;

    for (const auto &binding : scene_loader.runtimeObjectBindings()) {
        if (binding.authoring_object_id != *authoring_object_id) continue;
        const auto *transform = ecs_core.getTemplatePublicModule()
                                    .tryComponent<TransformComponent>(
                                        binding.object_id);
        if (transform == nullptr) return std::nullopt;
        return GizmoTargetTransform{.position = transform->pos};
    }
    return std::nullopt;
}

Gizmo::Gizmo() = default;
Gizmo::~Gizmo() = default;

void Gizmo::setDisplayRequest(
    std::optional<GizmoDisplayRequest> request) {
    const std::scoped_lock lock{display_mutex};
    display_request = std::move(request);
}

std::optional<GizmoDisplayRequest> Gizmo::displayRequest() const {
    const std::scoped_lock lock{display_mutex};
    return display_request;
}

void Gizmo::ensureDevice() {
    if (!device) device = GET_MODULE(VulkanManageCore).getDevice();
}

void Gizmo::ensureDescriptorPool() {
    ensureDevice();
    if (!descriptor_pool) descriptor_pool = createDescriptorPool(device);
}

void Gizmo::ensureVertexCapacity(std::size_t vertex_count) {
    const auto required =
        static_cast<vk::DeviceSize>(vertex_count * sizeof(GizmoVertex));
    if (required <= vertex_buffer_bytes) return;
    auto &vulkan = GET_MODULE(VulkanManageCore);
    vertex_buffer_bytes = nextCapacity(required);
    vertex_buffer = vulkan.allocBuf(
        vertex_buffer_bytes, vk::BufferUsageFlagBits::eStorageBuffer,
        vma::MemoryUsage::eAutoPreferHost,
        vma::AllocationCreateFlagBits::eHostAccessSequentialWrite);
}

void Gizmo::ensureDescriptorSet(PassId pass_id, PipelineRecord &record) {
    if (record.descriptor_set) return;
    ensureDescriptorPool();
    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    const auto layout = pipeline_factory.descriptorSetLayout(
        record.pipeline, PELICAN_SET_FREE);
    vk::DescriptorSetAllocateInfo allocate;
    allocate.descriptorPool = descriptor_pool.get();
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    auto sets = device.allocateDescriptorSetsUnique(allocate);
    if (sets.empty()) {
        throw std::runtime_error("Gizmo descriptor set allocation failed");
    }
    record.descriptor_set = std::move(sets.front());
    (void)pass_id;
}

void Gizmo::updateDescriptorSet(const PipelineRecord &record,
                                vk::DeviceSize bytes) {
    vk::DescriptorBufferInfo buffer;
    buffer.buffer = vertex_buffer.buffer.get();
    buffer.offset = 0;
    buffer.range = bytes;
    vk::WriteDescriptorSet write;
    write.dstSet = record.descriptor_set.get();
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = vk::DescriptorType::eStorageBuffer;
    write.pBufferInfo = &buffer;
    device.updateDescriptorSets(write, {});
}

PassId Gizmo::registerPass(vk::Format color_format,
                           ShaderBundleId vert_shader,
                           ShaderBundleId frag_shader,
                           std::vector<std::string> shader_defines,
                           vk::SampleCountFlagBits samples) {
    ensureDevice();
    if (next_pass_id == std::numeric_limits<int>::max()) {
        throw std::runtime_error("Gizmo pass handle table is exhausted");
    }

    GraphicsPipelineDesc desc;
    desc.vert = vert_shader;
    desc.frag = frag_shader;
    desc.color_formats = {color_format};
    desc.shader_defines = std::move(shader_defines);
    desc.blend = true;
    desc.src_color_blend_factor = vk::BlendFactor::eSrcAlpha;
    desc.dst_color_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.src_alpha_blend_factor = vk::BlendFactor::eOne;
    desc.dst_alpha_blend_factor = vk::BlendFactor::eOneMinusSrcAlpha;
    desc.topology = vk::PrimitiveTopology::eLineList;
    desc.rasterization_samples = samples;

    const auto pass_id = PassId{next_pass_id++};
    if (!pipelines
             .emplace(pass_id,
                      PipelineRecord{
                          GET_MODULE(PipelineFactory).create(desc), {}})
             .second) {
        throw std::runtime_error(
            "Gizmo pipeline table changed during registration");
    }
    registration_order.push_back(pass_id);
    return pass_id;
}

void Gizmo::render(vk::CommandBuffer cmd_buf, PassId pass_id,
                   const FrameResources &frame_resources,
                   const std::optional<glm::vec3> &world_position,
                   const glm::mat4 &view_projection, vk::Extent2D extent,
                   float content_scale) {
    const auto request = displayRequest();
    if (!request || !world_position) return;
    const auto geometry = buildGizmoGeometry(
        request->mode, *world_position, view_projection, extent, content_scale);
    if (geometry.segments.empty()) return;

    const auto found = pipelines.find(pass_id);
    if (found == pipelines.end()) {
        throw std::runtime_error("Gizmo pass pipeline not found");
    }
    std::vector<GizmoVertex> vertices;
    vertices.reserve(geometry.segments.size() * 2);
    for (const auto &segment : geometry.segments) {
        vertices.push_back(
            GizmoVertex{glm::vec4{segment.from.ndc, 1.0f}, segment.color});
        vertices.push_back(
            GizmoVertex{glm::vec4{segment.to.ndc, 1.0f}, segment.color});
    }

    const auto bytes =
        static_cast<vk::DeviceSize>(vertices.size() * sizeof(GizmoVertex));
    ensureVertexCapacity(vertices.size());
    ensureDescriptorSet(pass_id, found->second);
    GET_MODULE(VulkanManageCore)
        .writeBuf(vertex_buffer, vertices.data(), 0, bytes);
    updateDescriptorSet(found->second, bytes);

    auto &pipeline_factory = GET_MODULE(PipelineFactory);
    cmd_buf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                         pipeline_factory.pipeline(found->second.pipeline));
    frame_resources.bindGraphics(
        cmd_buf, pipeline_factory.layout(found->second.pipeline));
    cmd_buf.bindDescriptorSets(
        vk::PipelineBindPoint::eGraphics,
        pipeline_factory.layout(found->second.pipeline), PELICAN_SET_FREE,
        found->second.descriptor_set.get(), {});
    cmd_buf.draw(static_cast<std::uint32_t>(vertices.size()), 1, 0, 0);
}

Gizmo::RegistrationCheckpoint Gizmo::checkpointRegistrations() const noexcept {
    return RegistrationCheckpoint{registration_order.size(), next_pass_id};
}

void Gizmo::rollbackRegistrations(RegistrationCheckpoint checkpoint) {
    if (checkpoint.registration_count > registration_order.size()) {
        throw std::runtime_error("Gizmo registration checkpoint is invalid");
    }
    while (registration_order.size() > checkpoint.registration_count) {
        pipelines.erase(registration_order.back());
        registration_order.pop_back();
    }
    next_pass_id = checkpoint.next_pass_id;
}

std::vector<PassId>
Gizmo::registrationsSince(RegistrationCheckpoint checkpoint) const {
    if (checkpoint.registration_count > registration_order.size()) {
        throw std::runtime_error("Gizmo registration checkpoint is invalid");
    }
    return {registration_order.begin() +
                static_cast<std::ptrdiff_t>(checkpoint.registration_count),
            registration_order.end()};
}

void Gizmo::retireRegistrations(const std::vector<PassId> &ids) noexcept {
    for (const auto id : ids) {
        const auto found = pipelines.find(id);
        if (found != pipelines.end()) {
            auto retired = std::move(found->second);
            pipelines.erase(found);
            try {
                auto *queue = FastModuleContainer::tryGet<DeletionQueue>();
                if (queue != nullptr && queue->acceptingResources()) {
                    queue->defer(std::move(retired));
                }
            } catch (...) {
            }
        }
        std::erase(registration_order, id);
    }
    if (registration_order.empty()) {
        try {
            setDisplayRequest(std::nullopt);
        } catch (...) {
        }
    }
}

} // namespace Pelican
