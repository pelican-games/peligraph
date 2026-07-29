#include "planarreflectionview.hpp"

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

} // namespace

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
        result.views.push_back(
            RenderViewParameters{
                .view =
                    source.view *
                    reflection,
                .projection =
                    settings
                            .preserve_raster_winding
                        ? clip_x_flip *
                              source.projection
                        : source.projection,
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
