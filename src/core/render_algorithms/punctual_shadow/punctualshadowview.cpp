#include "punctualshadowview.hpp"
#include "../cube_capture/cubecaptureview.hpp"

#include <cmath>
#include <stdexcept>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {

RenderViewFamily buildPointShadowViewFamily(std::span<const PointLight> lights,
                                            float near_distance, float far_distance) {
    RenderViewFamily result{.family_id = std::string{pointShadowViewFamilyId}};
    result.views.reserve(lights.size() * 6);
    for (std::size_t index = 0; index < lights.size(); ++index) {
        auto cube = buildCubeCaptureViewFamily({lights[index].position, near_distance, far_distance});
        for (auto &view : cube.views) {
            view.view_id = result.family_id + "/" + std::to_string(index) + "/" + view.view_id;
            // The cube builder already set `face`; the light is outermost.
            view.axes[std::string{renderViewAxisLight}] = static_cast<std::uint32_t>(index);
            result.views.push_back(std::move(view));
        }
    }
    return result;
}

RenderViewFamily buildSpotShadowViewFamily(std::span<const SpotLight> lights,
                                           float near_distance, float far_distance) {
    if (!std::isfinite(near_distance) || !std::isfinite(far_distance) ||
        near_distance <= 0.0f || far_distance <= near_distance)
        throw std::invalid_argument("spot shadow requires 0 < near_distance < far_distance");
    RenderViewFamily result{.family_id = std::string{spotShadowViewFamilyId}};
    result.views.reserve(lights.size());
    for (std::size_t index = 0; index < lights.size(); ++index) {
        const auto &light = lights[index];
        const auto length = glm::length(light.direction);
        if (!std::isfinite(light.position.x) || !std::isfinite(light.position.y) ||
            !std::isfinite(light.position.z) || !std::isfinite(length) || length <= 0.0f ||
            !std::isfinite(light.outerConeAngle) || light.outerConeAngle <= 0.0f ||
            light.outerConeAngle >= 90.0f)
            throw std::invalid_argument("spot shadow requires a finite position, nonzero direction and 0 < outer cone < 90 degrees");
        const auto direction = light.direction / length;
        const auto up = std::abs(direction.y) < 0.99f
            ? glm::vec3{0, 1, 0} : glm::vec3{0, 0, 1};
        result.views.push_back(RenderViewParameters{
            .view = glm::lookAtRH(light.position, light.position + direction, up),
            .projection = glm::perspectiveRH_ZO(glm::radians(light.outerConeAngle * 2.0f),
                                               1.0f, near_distance, far_distance),
            .camera_position = light.position,
            .first_person_view = false,
            .view_id = result.family_id + "/" + std::to_string(index),
            .axes = {{std::string{renderViewAxisLight}, static_cast<std::uint32_t>(index)}},
        });
    }
    return result;
}

} // namespace Pelican
