#include "directionalshadowcascade.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace Pelican {
namespace {

struct SourceFrustum {
    glm::mat4 inverse_view_projection{1.0f};
    float near_distance = 0.0f;
    float far_distance = 0.0f;
};

bool finite(glm::vec3 value) {
    return std::isfinite(value.x) &&
           std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool finite(const glm::mat4 &value) {
    for (glm::length_t column = 0;
         column < 4; ++column) {
        for (glm::length_t row = 0;
             row < 4; ++row) {
            if (!std::isfinite(
                    value[column][row])) {
                return false;
            }
        }
    }
    return true;
}

glm::vec3 projectPoint(
    const glm::mat4 &matrix,
    glm::vec3 point) {
    const auto projected =
        matrix * glm::vec4{point, 1.0f};
    if (!std::isfinite(projected.w) ||
        std::abs(projected.w) <
            1.0e-7f) {
        throw std::runtime_error(
            "directional shadow cascade projection is singular");
    }
    const auto result =
        glm::vec3{projected} /
        projected.w;
    if (!finite(result)) {
        throw std::runtime_error(
            "directional shadow cascade projection is non-finite");
    }
    return result;
}

SourceFrustum sourceFrustum(
    const RenderViewParameters &view) {
    if (!finite(view.view) ||
        !finite(view.projection)) {
        throw std::runtime_error(
            "directional shadow cascade source view is non-finite");
    }
    const auto inverse_projection =
        glm::inverse(view.projection);
    const auto inverse_view_projection =
        glm::inverse(
            view.projection * view.view);
    if (!finite(inverse_projection) ||
        !finite(inverse_view_projection)) {
        throw std::runtime_error(
            "directional shadow cascade source view is singular");
    }
    const auto near_center =
        projectPoint(
            inverse_projection,
            {0.0f, 0.0f, 0.0f});
    const auto far_center =
        projectPoint(
            inverse_projection,
            {0.0f, 0.0f, 1.0f});
    auto near_distance =
        std::abs(near_center.z);
    auto far_distance =
        std::abs(far_center.z);
    if (near_distance > far_distance) {
        std::swap(
            near_distance, far_distance);
    }
    near_distance =
        std::max(near_distance, 1.0e-4f);
    if (!std::isfinite(near_distance) ||
        !std::isfinite(far_distance) ||
        far_distance <=
            near_distance + 1.0e-4f) {
        throw std::runtime_error(
            "directional shadow cascade source projection has an "
            "invalid depth range");
    }
    return {
        .inverse_view_projection =
            inverse_view_projection,
        .near_distance = near_distance,
        .far_distance = far_distance,
    };
}

glm::mat4 vulkanOrtho(
    float left, float right,
    float bottom, float top,
    float near_distance,
    float far_distance) {
    glm::mat4 result{1.0f};
    result[0][0] =
        2.0f / (right - left);
    result[1][1] =
        2.0f / (top - bottom);
    result[2][2] =
        1.0f /
        (near_distance - far_distance);
    result[3][0] =
        -(right + left) /
        (right - left);
    result[3][1] =
        -(top + bottom) /
        (top - bottom);
    result[3][2] =
        near_distance /
        (near_distance - far_distance);
    return result;
}

std::vector<float> cascadeSplits(
    float near_distance,
    float far_distance,
    const DirectionalShadowCascadeSettings
        &settings) {
    std::vector<float> result(
        settings.cascade_count + 1);
    result.front() = near_distance;
    result.back() = far_distance;
    for (std::uint32_t index = 1;
         index < settings.cascade_count;
         ++index) {
        const auto fraction =
            static_cast<float>(index) /
            static_cast<float>(
                settings.cascade_count);
        const auto logarithmic =
            near_distance *
            std::pow(
                far_distance /
                    near_distance,
                fraction);
        const auto uniform =
            near_distance +
            (far_distance -
             near_distance) *
                fraction;
        result[index] =
            glm::mix(
                uniform, logarithmic,
                settings.split_lambda);
    }
    return result;
}

std::vector<glm::vec3> cascadeCorners(
    std::span<const SourceFrustum> sources,
    float near_distance,
    float far_distance) {
    std::vector<glm::vec3> result;
    result.reserve(
        sources.size() * 8);
    for (const auto &source : sources) {
        const auto denominator =
            source.far_distance -
            source.near_distance;
        const auto near_fraction =
            std::clamp(
                (near_distance -
                 source.near_distance) /
                    denominator,
                0.0f, 1.0f);
        const auto far_fraction =
            std::clamp(
                (far_distance -
                 source.near_distance) /
                    denominator,
                0.0f, 1.0f);
        for (const float y :
             std::array{-1.0f, 1.0f}) {
            for (const float x :
                 std::array{-1.0f, 1.0f}) {
                const auto near_corner =
                    projectPoint(
                        source
                            .inverse_view_projection,
                        {x, y, 0.0f});
                const auto far_corner =
                    projectPoint(
                        source
                            .inverse_view_projection,
                        {x, y, 1.0f});
                result.push_back(
                    glm::mix(
                        near_corner,
                        far_corner,
                        near_fraction));
                result.push_back(
                    glm::mix(
                        near_corner,
                        far_corner,
                        far_fraction));
            }
        }
    }
    return result;
}

RenderViewParameters cascadeView(
    std::span<const glm::vec3> corners,
    glm::vec3 direction,
    glm::uvec2 extent,
    bool stabilize,
    std::uint32_t cascade_index,
    RenderViewDepthRange depth_range) {
    glm::vec3 center{0.0f};
    for (const auto corner : corners) {
        center += corner;
    }
    center /=
        static_cast<float>(
            corners.size());

    float radius = 0.0f;
    for (const auto corner : corners) {
        radius = std::max(
            radius,
            glm::length(corner - center));
    }
    if (!std::isfinite(radius) ||
        radius <= 1.0e-4f) {
        throw std::runtime_error(
            "directional shadow cascade has degenerate bounds");
    }
    // Quantizing the radius prevents tiny source-camera changes from
    // continuously changing the orthographic scale.
    radius =
        std::ceil(radius * 16.0f) /
        16.0f;

    const auto world_up =
        std::abs(glm::dot(
            direction,
            glm::vec3{0.0f, 1.0f, 0.0f})) >
                0.95f
            ? glm::vec3{0.0f, 0.0f, 1.0f}
            : glm::vec3{0.0f, 1.0f, 0.0f};
    const auto right =
        glm::normalize(
            glm::cross(
                direction, world_up));
    const auto up =
        glm::normalize(
            glm::cross(
                right, direction));
    if (stabilize) {
        const auto texel_x =
            (2.0f * radius) /
            static_cast<float>(extent.x);
        const auto texel_y =
            (2.0f * radius) /
            static_cast<float>(extent.y);
        const auto right_position =
            glm::dot(center, right);
        const auto up_position =
            glm::dot(center, up);
        center +=
            right *
                (std::round(
                     right_position /
                     texel_x) *
                     texel_x -
                 right_position);
        center +=
            up *
                (std::round(
                     up_position /
                     texel_y) *
                     texel_y -
                 up_position);
    }

    constexpr float depth_padding = 10.0f;
    const auto eye =
        center -
        direction *
            (radius + depth_padding);
    const auto view =
        glm::lookAt(
            eye, center, world_up);
    auto minimum_depth =
        std::numeric_limits<float>::max();
    auto maximum_depth =
        std::numeric_limits<float>::lowest();
    for (const auto corner : corners) {
        const auto light_space =
            view *
            glm::vec4{corner, 1.0f};
        const auto depth =
            -light_space.z;
        minimum_depth =
            std::min(
                minimum_depth, depth);
        maximum_depth =
            std::max(
                maximum_depth, depth);
    }
    const auto near_plane =
        std::max(
            0.01f,
            minimum_depth -
                depth_padding);
    const auto far_plane =
        std::max(
            near_plane + 0.01f,
            maximum_depth +
                depth_padding);
    const auto projection =
        vulkanOrtho(
            -radius, radius,
            -radius, radius,
            near_plane, far_plane);
    if (!finite(view) ||
        !finite(projection)) {
        throw std::runtime_error(
            "directional shadow cascade produced a non-finite view");
    }
    return {
        .view = view,
        .projection = projection,
        .camera_position = eye,
        .first_person_view = false,
        .view_id =
            directionalShadowCascadeRenderViewId(
                cascade_index),
        .depth_range = depth_range,
    };
}

} // namespace

RenderViewFamily
buildDirectionalShadowCascadeFamily(
    const RenderViewFamily &main_family,
    glm::vec3 light_direction,
    glm::uvec2 shadow_extent,
    const DirectionalShadowCascadeSettings
        &settings) {
    if (main_family.family_id !=
            mainRenderViewFamilyId ||
        main_family.views.empty()) {
        throw std::runtime_error(
            "directional shadow cascades require a non-empty '$main' "
            "view family");
    }
    if (settings.cascade_count == 0 ||
        settings.cascade_count >
            maximumDirectionalShadowCascades) {
        throw std::runtime_error(
            "directional shadow cascade_count is outside the supported "
            "range");
    }
    if (!std::isfinite(settings.max_distance) ||
        settings.max_distance <= 0.0f ||
        !std::isfinite(settings.split_lambda) ||
        settings.split_lambda < 0.0f ||
        settings.split_lambda > 1.0f) {
        throw std::runtime_error(
            "directional shadow cascade settings are invalid");
    }
    if (shadow_extent.x == 0 ||
        shadow_extent.y == 0) {
        throw std::runtime_error(
            "directional shadow cascades require a non-zero extent");
    }
    const auto direction_length =
        glm::length(light_direction);
    if (!std::isfinite(direction_length) ||
        direction_length <= 1.0e-4f) {
        throw std::runtime_error(
            "directional shadow cascades require a valid light "
            "direction");
    }
    light_direction /=
        direction_length;

    std::vector<SourceFrustum> sources;
    sources.reserve(
        main_family.views.size());
    auto near_distance =
        std::numeric_limits<float>::max();
    auto far_distance = 0.0f;
    for (const auto &view :
         main_family.views) {
        auto source =
            sourceFrustum(view);
        near_distance =
            std::min(
                near_distance,
                source.near_distance);
        far_distance =
            std::max(
                far_distance,
                source.far_distance);
        sources.push_back(
            std::move(source));
    }
    far_distance =
        std::min(
            far_distance,
            settings.max_distance);
    if (far_distance <=
        near_distance + 1.0e-4f) {
        throw std::runtime_error(
            "directional shadow max_distance does not reach beyond the "
            "main-view near plane");
    }

    const auto splits =
        cascadeSplits(
            near_distance,
            far_distance,
            settings);
    RenderViewFamily result{
        .family_id =
            std::string{
                directionalShadowRenderViewFamilyId},
    };
    result.views.reserve(
        settings.cascade_count);
    for (std::uint32_t cascade = 0;
         cascade < settings.cascade_count;
         ++cascade) {
        const auto depth_range =
            RenderViewDepthRange{
                .near_distance =
                    splits[cascade],
                .far_distance =
                    splits[cascade + 1],
            };
        const auto corners =
            cascadeCorners(
                sources,
                depth_range.near_distance,
                depth_range.far_distance);
        result.views.push_back(
            cascadeView(
                corners, light_direction,
                shadow_extent,
                settings.stabilize,
                cascade, depth_range));
    }
    return result;
}

} // namespace Pelican
