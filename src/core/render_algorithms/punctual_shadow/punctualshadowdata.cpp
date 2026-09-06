#include "punctualshadowdata.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican {

std::vector<glm::uvec4> packPunctualShadowDataV1(
    std::span<const PunctualShadowDataInput> lights) {
    constexpr auto max_index = std::numeric_limits<std::uint32_t>::max();
    // Include the header in the representable element count before adding
    // matrices, so all later record/matrix offsets fit the shader's uint ABI.
    if (lights.size() >= max_index) {
        throw std::overflow_error("punctual shadow record count exceeds the data ABI");
    }
    std::uint64_t element_count = 1 + static_cast<std::uint64_t>(lights.size());
    std::vector<const PunctualShadowDataInput *> sorted;
    sorted.reserve(lights.size());
    for (const auto &light : lights) {
        std::uint32_t matrix_count;
        switch (light.kind) {
        case PunctualShadowKind::point: matrix_count = 6; break;
        case PunctualShadowKind::spot: matrix_count = 1; break;
        default:
            throw std::invalid_argument("punctual shadow light kind is invalid");
        }
        if (light.view_projections.size() != matrix_count) {
            throw std::invalid_argument("punctual shadow matrix count does not match light kind");
        }
        if (light.first_layer > max_index - (matrix_count - 1)) {
            throw std::overflow_error("punctual shadow layer index exceeds the data ABI");
        }
        element_count += static_cast<std::uint64_t>(matrix_count) * 4;
        if (element_count > max_index) {
            throw std::overflow_error("punctual shadow element offsets exceed the data ABI");
        }
        for (const auto &matrix : light.view_projections) {
            for (glm::length_t column = 0; column < 4; ++column) {
                for (glm::length_t row = 0; row < 4; ++row) {
                    if (!std::isfinite(matrix[column][row])) {
                        throw std::invalid_argument("punctual shadow matrix is non-finite");
                    }
                }
            }
        }
        sorted.push_back(&light);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto *left, const auto *right) {
        return left->inventory_index < right->inventory_index;
    });
    for (std::size_t index = 1; index < sorted.size(); ++index) {
        if (sorted[index - 1]->inventory_index == sorted[index]->inventory_index) {
            throw std::invalid_argument("punctual shadow inventory indices are duplicated");
        }
    }

    std::vector<glm::uvec4> result(static_cast<std::size_t>(element_count));
    const auto record_count = static_cast<std::uint32_t>(sorted.size());
    result[0] = {punctualShadowDataV1Magic, punctualShadowDataV1Version, record_count, 0};
    auto matrix_element = 1u + record_count;
    for (std::uint32_t index = 0; index < record_count; ++index) {
        const auto &light = *sorted[index];
        result[1u + index] = {
            light.inventory_index, static_cast<std::uint32_t>(light.kind),
            matrix_element, light.first_layer};
        for (const auto &matrix : light.view_projections) {
            for (glm::length_t column = 0; column < 4; ++column) {
                auto &packed = result[matrix_element++];
                for (glm::length_t row = 0; row < 4; ++row) {
                    packed[row] = std::bit_cast<std::uint32_t>(matrix[column][row]);
                }
            }
        }
    }
    return result;
}

} // namespace Pelican
