#pragma once

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Pelican {

// CC-1a (docs/design_coordinate_convention.md section 2). The world is
// right-handed with +Y up, as in glTF. A camera's view space is +X right,
// +Y up and -Z forward, and its up comes from its rotation. Clip space, NDC,
// pixels and uv stay Vulkan's: +y is down, depth is 0..1 and
// uv = ndc * 0.5 + 0.5. The one place the two meet is the camera projection
// below: it flips y, so the image is the world as seen, not its mirror.
// Projection jitter is added after these (in NDC), so TAA is unchanged.
//
// Header-only: pelican_openxr is a separate library and uses it too.

namespace clipspace_detail {

// Clip y = -clip y: the view's +Y reaches the top of the framebuffer.
inline glm::mat4 flipClipY(glm::mat4 matrix) {
    for (glm::length_t column = 0; column < 4; ++column) {
        matrix[column][1] = -matrix[column][1];
    }
    return matrix;
}

inline float signOf(float value) { return value < 0.0f ? -1.0f : 1.0f; }

} // namespace clipspace_detail

// Right-handed, depth 0..1, clip y flipped ([1][1] is negative).
inline glm::mat4 vulkanPerspective(float yfov, float aspect, float znear,
                                   float zfar) {
    return clipspace_detail::flipClipY(
        glm::perspectiveRH_ZO(yfov, aspect, znear, zfar));
}

inline glm::mat4 vulkanOrtho(float left, float right, float bottom, float top,
                             float znear, float zfar) {
    return clipspace_detail::flipClipY(
        glm::orthoRH_ZO(left, right, bottom, top, znear, zfar));
}

// An off-axis perspective by the tangents of its four half-angles (left and
// down are negative for a frustum around the view axis), as OpenXR gives
// them. Right-handed, depth 0..1, clip y flipped.
inline glm::mat4 vulkanAsymmetric(float tangent_left, float tangent_right,
                                  float tangent_down, float tangent_up,
                                  float znear, float zfar) {
    const float tangent_width = tangent_right - tangent_left;
    const float tangent_height = tangent_up - tangent_down;
    glm::mat4 result{0.0f};
    result[0][0] = 2.0f / tangent_width;
    result[1][1] = 2.0f / tangent_height;
    result[2][0] = (tangent_right + tangent_left) / tangent_width;
    result[2][1] = (tangent_up + tangent_down) / tangent_height;
    result[2][2] = -zfar / (zfar - znear);
    result[2][3] = -1.0f;
    result[3][2] = -(zfar * znear) / (zfar - znear);
    return clipspace_detail::flipClipY(result);
}

struct CameraBasis {
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    glm::vec3 right{1.0f, 0.0f, 0.0f};
};

// forward = R * (0, 0, -1), up = R * (0, 1, 0),
// right = cross(forward, up) = R * (1, 0, 0).
inline CameraBasis cameraBasis(const glm::quat &rotation) {
    const auto matrix = glm::mat3_cast(glm::normalize(rotation));
    return CameraBasis{
        .forward = glm::normalize(matrix * glm::vec3{0.0f, 0.0f, -1.0f}),
        .up = glm::normalize(matrix * glm::vec3{0.0f, 1.0f, 0.0f}),
        .right = glm::normalize(matrix * glm::vec3{1.0f, 0.0f, 0.0f}),
    };
}

// The rotation whose basis looks along `forward` with `up` (orthogonalized
// against forward). The inverse of cameraBasis for a non-parallel pair.
inline glm::quat cameraRotation(glm::vec3 forward, glm::vec3 up) {
    const auto back = -glm::normalize(forward);
    const auto right = glm::normalize(glm::cross(up, back));
    const auto true_up = glm::cross(back, right);
    return glm::normalize(glm::quat_cast(glm::mat3{right, true_up, back}));
}

// World to view space (+X right, +Y up, -Z forward) of a camera at
// `position` looking along `forward` with `up`.
inline glm::mat4 viewFromBasis(glm::vec3 position, glm::vec3 forward,
                               glm::vec3 up) {
    return glm::lookAtRH(position, position + forward, up);
}

// The inverse of the camera's transform (translation and rotation).
inline glm::mat4 viewFromWorld(glm::vec3 position, const glm::quat &rotation) {
    const auto basis = cameraBasis(rotation);
    return viewFromBasis(position, basis.forward, basis.up);
}

// True when the view draws the world's mirror image on the framebuffer:
// screen right, screen up and toward-the-viewer form a left-handed frame in
// the world. Mesh passes that cull by winding take counter-clockwise as the
// front on a view that is not mirrored and clockwise on one that is (a cube
// face in Vulkan's cube-face convention, or a projection without the y flip).
inline bool viewIsMirrored(const glm::mat4 &projection, const glm::mat4 &view) {
    // Screen right is NDC +x and screen up NDC -y (Vulkan); the viewer looks
    // along the view-space z on which w (perspective) or depth
    // (orthographic) grows. The frame is right-handed in view space when
    // sign(x) * sign(y) * sign(forward z) is positive, and a reflecting view
    // turns it over.
    using clipspace_detail::signOf;
    const auto x_sign = signOf(projection[0][0]);
    const auto y_sign = signOf(projection[1][1]);
    const auto forward_sign = projection[2][3] != 0.0f
                                  ? signOf(projection[2][3])
                                  : signOf(projection[2][2]);
    const auto view_sign = signOf(glm::determinant(glm::mat3{view}));
    return x_sign * y_sign * forward_sign * view_sign < 0.0f;
}

} // namespace Pelican
