#include "planarreflectionview.hpp"

// Standard planar-reflection camera policy lives with the removable render
// algorithm package; only RenderViewFamily data crosses back into the engine.

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {

RenderViewClipPlane normalizedPlane(
    const RenderViewClipPlane &plane) {
    const auto length =
        glm::length(plane.normal);
    if (!std::isfinite(length) ||
        length <=
            std::numeric_limits<float>::epsilon() ||
        !std::isfinite(plane.offset)) {
        throw std::invalid_argument(
            "planar reflection requires a finite non-zero clip plane");
    }
    return {
        plane.normal / length,
        plane.offset / length,
    };
}

glm::mat4 reflectionMatrix(
    const RenderViewClipPlane &plane) {
    const auto x = plane.normal.x;
    const auto y = plane.normal.y;
    const auto z = plane.normal.z;
    const auto d = plane.offset;
    glm::mat4 result{1.0f};
    result[0][0] = 1.0f - 2.0f * x * x;
    result[1][0] = -2.0f * x * y;
    result[2][0] = -2.0f * x * z;
    result[3][0] = -2.0f * d * x;
    result[0][1] = -2.0f * y * x;
    result[1][1] = 1.0f - 2.0f * y * y;
    result[2][1] = -2.0f * y * z;
    result[3][1] = -2.0f * d * y;
    result[0][2] = -2.0f * z * x;
    result[1][2] = -2.0f * z * y;
    result[2][2] = 1.0f - 2.0f * z * z;
    result[3][2] = -2.0f * d * z;
    return result;
}

bool finiteMatrix(const glm::mat4 &matrix) {
    for (glm::length_t column = 0;
         column < 4; ++column) {
        for (glm::length_t row = 0;
             row < 4; ++row) {
            if (!std::isfinite(
                    matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool finiteVector(const glm::vec4 &value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z) &&
           std::isfinite(value.w);
}

} // namespace

std::optional<glm::mat4>
tryBuildObliqueNearPlaneProjectionZO(
    const glm::mat4 &projection,
    const glm::mat4 &view,
    const RenderViewClipPlane &clip_plane) {
    if (!finiteMatrix(projection) ||
        !finiteMatrix(view)) {
        throw std::invalid_argument(
            "oblique near-plane projection requires finite matrices");
    }
    const auto plane =
        normalizedPlane(clip_plane);
    const auto inverse_view =
        glm::inverse(view);
    const auto inverse_projection =
        glm::inverse(projection);
    if (!finiteMatrix(inverse_view) ||
        !finiteMatrix(inverse_projection)) {
        throw std::invalid_argument(
            "oblique near-plane projection requires invertible matrices");
    }

    const auto view_plane =
        glm::transpose(inverse_view) *
        glm::vec4{
            plane.normal,
            plane.offset};
    if (!finiteVector(view_plane)) {
        throw std::invalid_argument(
            "oblique near-plane projection produced an invalid view plane");
    }

    // The retained half-space must begin in front of the camera. When the
    // camera is on/inside it, the semantic clip plane is behind the near
    // boundary and fragment/CPU clipping remains the correct fallback.
    constexpr float applicability_epsilon =
        1.0e-5f;
    if (view_plane.w >=
        -applicability_epsilon) {
        return std::nullopt;
    }

    // Vulkan's forward-Z clip volume uses 0 <= z <= w. The new near row is
    // k * plane, and k is chosen so the far-face corner deepest inside the
    // retained half-space still maps to z == w. Enumerating the near face too
    // rejects orientations whose retained distance decreases along the view;
    // those planes cannot safely replace a forward-Z near boundary.
    float near_plane_dot =
        -std::numeric_limits<float>::infinity();
    float far_plane_dot =
        -std::numeric_limits<float>::infinity();
    for (const auto z : {0.0f, 1.0f}) {
        for (const auto x : {-1.0f, 1.0f}) {
            for (const auto y : {-1.0f, 1.0f}) {
                const auto view_corner =
                    inverse_projection *
                    glm::vec4{
                        x, y, z, 1.0f};
                if (!finiteVector(view_corner)) {
                    throw std::invalid_argument(
                        "oblique near-plane projection produced an invalid "
                        "clip-volume corner");
                }
                const auto plane_dot =
                    glm::dot(
                        view_plane,
                        view_corner);
                if (!std::isfinite(plane_dot)) {
                    throw std::invalid_argument(
                        "oblique near-plane projection produced an invalid "
                        "clip-volume distance");
                }
                auto &face_max =
                    z == 0.0f
                        ? near_plane_dot
                        : far_plane_dot;
                face_max =
                    std::max(
                        face_max,
                        plane_dot);
            }
        }
    }
    if (far_plane_dot <=
            applicability_epsilon ||
        far_plane_dot <=
            near_plane_dot +
                applicability_epsilon) {
        return std::nullopt;
    }

    auto result = projection;
    const auto near_row =
        view_plane / far_plane_dot;
    for (glm::length_t column = 0;
         column < 4; ++column) {
        result[column][2] =
            near_row[column];
    }
    if (!finiteMatrix(result)) {
        throw std::invalid_argument(
            "oblique near-plane projection produced an invalid matrix");
    }
    return result;
}

RenderViewFamily buildPlanarReflectionViewFamily(
    const RenderViewFamily &main_family,
    const PlanarReflectionViewSettings
        &settings) {
    if (main_family.family_id !=
            mainRenderViewFamilyId ||
        main_family.views.empty()) {
        throw std::invalid_argument(
            "planar reflection requires a non-empty '$main' family");
    }
    const auto plane =
        normalizedPlane(
            settings.clip_plane);
    const auto reflection =
        reflectionMatrix(plane);
    glm::mat4 clip_x_flip{1.0f};
    clip_x_flip[0][0] = -1.0f;

    RenderViewFamily result{
        .family_id =
            std::string{
                planarReflectionRenderViewFamilyId},
    };
    result.views.reserve(
        main_family.views.size());
    for (const auto &source :
         main_family.views) {
        if (source.view_id.empty() ||
            !finiteMatrix(source.view) ||
            !finiteMatrix(source.projection) ||
            !std::isfinite(
                source.camera_position.x) ||
            !std::isfinite(
                source.camera_position.y) ||
            !std::isfinite(
                source.camera_position.z)) {
            throw std::invalid_argument(
                "planar reflection source view is invalid");
        }
        const auto reflected_position =
            reflection *
            glm::vec4{
                source.camera_position,
                1.0f};
        const auto reflected_view =
            source.view *
            reflection;
        auto reflected_projection =
            settings
                    .preserve_raster_winding
                ? clip_x_flip *
                      source.projection
                : source.projection;
        if (settings.oblique_near_plane) {
            if (const auto oblique =
                    tryBuildObliqueNearPlaneProjectionZO(
                        reflected_projection,
                        reflected_view,
                        plane)) {
                reflected_projection =
                    *oblique;
            }
        }
        result.views.push_back(
            RenderViewParameters{
                .view =
                    reflected_view,
                .projection =
                    reflected_projection,
                .camera_position =
                    glm::vec3{
                        reflected_position},
                .first_person_view =
                    source.first_person_view,
                .view_id =
                    "$mirror/" +
                    source.view_id,
                .clip_plane = plane,
            });
    }
    return result;
}

} // namespace Pelican
