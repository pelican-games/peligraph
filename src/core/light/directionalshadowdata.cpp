#include "directionalshadowdata.hpp"

#include "../../project/viewfamilyrelation.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {

glm::uvec4 matrixColumnBits(
    const glm::vec4 &column) {
    glm::uvec4 result{};
    static_assert(sizeof(result) == sizeof(column));
    std::memcpy(&result, &column, sizeof(result));
    return result;
}

} // namespace

PackedDirectionalShadowDataV1
packDirectionalShadowDataV1(
    std::span<const std::uint32_t>
        light_inventory_indices,
    std::uint32_t cascade_count,
    std::span<const glm::mat4>
        view_projections) {
    if (cascade_count == 0 ||
        cascade_count >
            maximumDirectionalShadowCascades) {
        throw std::runtime_error(
            "directional shadow data cascade count is outside the v1 ABI");
    }
    for (std::size_t index = 1;
         index < light_inventory_indices.size();
         ++index) {
        if (light_inventory_indices[index - 1] >=
            light_inventory_indices[index]) {
            throw std::runtime_error(
                "directional shadow inventory indices must be strictly increasing");
        }
    }
    if (light_inventory_indices.size() >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "directional shadow light count exceeds the v1 index range");
    }
    const auto shadow_light_count =
        static_cast<std::uint32_t>(
            light_inventory_indices.size());
    const auto matrix_count =
        static_cast<std::uint64_t>(
            shadow_light_count) *
        cascade_count;
    if (matrix_count != view_projections.size()) {
        throw std::runtime_error(
            "directional shadow matrix count does not match light and cascade counts");
    }
    const auto element_count =
        static_cast<std::uint64_t>(
            directionalShadowDataV1HeaderElements) +
        shadow_light_count +
        matrix_count *
            directionalShadowDataV1MatrixElements;
    if (element_count >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error(
            "directional shadow data exceeds the v1 element index range");
    }

    PackedDirectionalShadowDataV1 result;
    result.shadow_light_count =
        shadow_light_count;
    result.cascade_count = cascade_count;
    result.elements.resize(
        static_cast<std::size_t>(
            element_count));
    result.elements[0] = glm::uvec4{
        directionalShadowDataV1Magic,
        directionalShadowDataV1Version,
        shadow_light_count,
        cascade_count,
    };

    const auto matrix_elements_begin =
        directionalShadowDataV1HeaderElements +
        shadow_light_count;
    for (std::uint32_t light = 0;
         light < shadow_light_count;
         ++light) {
        const auto first_layer =
            light * cascade_count;
        const auto first_matrix_element =
            matrix_elements_begin +
            first_layer *
                directionalShadowDataV1MatrixElements;
        result.elements[
            directionalShadowDataV1HeaderElements +
            light] = glm::uvec4{
            light_inventory_indices[light],
            first_matrix_element,
            first_layer,
            cascade_count,
        };
    }
    for (std::size_t matrix = 0;
         matrix < view_projections.size();
         ++matrix) {
        const auto element =
            static_cast<std::size_t>(
                matrix_elements_begin) +
            matrix *
                directionalShadowDataV1MatrixElements;
        for (glm::length_t column = 0;
             column < 4; ++column) {
            result.elements[
                element +
                static_cast<std::size_t>(column)] =
                matrixColumnBits(
                    view_projections[matrix][column]);
        }
    }
    return result;
}

} // namespace Pelican
