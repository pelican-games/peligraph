#include "cubecaptureview.hpp"

// Standard cube-camera policy is intentionally confined to the removable
// render-algorithm package.

#include <array>
#include <cmath>
#include <stdexcept>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {
namespace {

struct CubeFace {
    std::string_view id;
    glm::vec3 direction;
    glm::vec3 up;
};

constexpr std::array<CubeFace, 6>
    cube_faces{{
        {
            cubeCapturePositiveXViewId,
            {1.0f, 0.0f, 0.0f},
            {0.0f, -1.0f, 0.0f},
        },
        {
            cubeCaptureNegativeXViewId,
            {-1.0f, 0.0f, 0.0f},
            {0.0f, -1.0f, 0.0f},
        },
        {
            cubeCapturePositiveYViewId,
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f},
        },
        {
            cubeCaptureNegativeYViewId,
            {0.0f, -1.0f, 0.0f},
            {0.0f, 0.0f, -1.0f},
        },
        {
            cubeCapturePositiveZViewId,
            {0.0f, 0.0f, 1.0f},
            {0.0f, -1.0f, 0.0f},
        },
        {
            cubeCaptureNegativeZViewId,
            {0.0f, 0.0f, -1.0f},
            {0.0f, -1.0f, 0.0f},
        },
    }};

bool finitePosition(
    const glm::vec3 &position) {
    return std::isfinite(position.x) &&
           std::isfinite(position.y) &&
           std::isfinite(position.z);
}

} // namespace

RenderViewFamily buildCubeCaptureViewFamily(
    const CubeCaptureViewSettings
        &settings) {
    if (!finitePosition(settings.position) ||
        !std::isfinite(settings.near_distance) ||
        !std::isfinite(settings.far_distance) ||
        settings.near_distance <= 0.0f ||
        settings.far_distance <=
            settings.near_distance) {
        throw std::invalid_argument(
            "cube capture requires a finite position and "
            "0 < near_distance < far_distance");
    }

    const auto projection =
        glm::perspectiveRH_ZO(
            glm::radians(90.0f), 1.0f,
            settings.near_distance,
            settings.far_distance);
    RenderViewFamily result{
        .family_id =
            std::string{
                cubeCaptureRenderViewFamilyId},
    };
    result.views.reserve(cube_faces.size());
    for (const auto &face : cube_faces) {
        result.views.push_back(
            RenderViewParameters{
                .view =
                    glm::lookAtRH(
                        settings.position,
                        settings.position +
                            face.direction,
                        face.up),
                .projection = projection,
                .camera_position =
                    settings.position,
                .first_person_view = false,
                .view_id =
                    std::string{face.id},
            });
    }
    return result;
}

} // namespace Pelican
