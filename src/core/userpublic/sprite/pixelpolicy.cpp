#include "pixelpolicy.hpp"

#include <algorithm>
#include <cmath>

namespace Pelican::sprite {
namespace {

bool closeToInteger(double value, double rounded) {
    return std::abs(value - rounded) <=
           pixelContractTolerance * std::max(1.0, std::abs(value));
}

bool finiteBasis(const std::array<double, 3> &basis) {
    return std::ranges::all_of(basis, [](double value) { return std::isfinite(value); });
}

bool nearlyZero(double value, double scale) {
    return std::abs(value) <= pixelContractTolerance * std::max(1.0, scale);
}

} // namespace

PixelContract evaluatePixelContract(bool requested, bool orthographic,
                                    double xmag, double ymag, double pixels_per_unit,
                                    std::uint32_t viewport_width,
                                    std::uint32_t viewport_height) {
    PixelContract result;
    result.requested = requested;
    result.pixels_per_unit = pixels_per_unit;
    result.viewport_width = viewport_width;
    result.viewport_height = viewport_height;
    if (!requested) {
        result.failure = PixelContractFailure::disabled;
        return result;
    }
    if (!orthographic) {
        result.failure = PixelContractFailure::perspective_camera;
        return result;
    }
    if (!std::isfinite(pixels_per_unit) || pixels_per_unit <= 0.0) {
        result.failure = PixelContractFailure::invalid_pixels_per_unit;
        return result;
    }
    if (viewport_width == 0 || viewport_height == 0) {
        result.failure = PixelContractFailure::invalid_viewport;
        return result;
    }
    if (!std::isfinite(xmag) || !std::isfinite(ymag) || xmag <= 0.0 || ymag <= 0.0) {
        result.failure = PixelContractFailure::invalid_projection;
        return result;
    }

    result.world_units_per_pixel_x = 2.0 * xmag / static_cast<double>(viewport_width);
    result.world_units_per_pixel_y = 2.0 * ymag / static_cast<double>(viewport_height);
    result.zoom_x = (1.0 / pixels_per_unit) / result.world_units_per_pixel_x;
    result.zoom_y = (1.0 / pixels_per_unit) / result.world_units_per_pixel_y;
    const auto rounded_x = std::round(result.zoom_x);
    const auto rounded_y = std::round(result.zoom_y);
    if (!closeToInteger(result.zoom_x, rounded_x)) {
        result.failure = PixelContractFailure::non_integer_zoom_x;
        return result;
    }
    if (!closeToInteger(result.zoom_y, rounded_y)) {
        result.failure = PixelContractFailure::non_integer_zoom_y;
        return result;
    }
    if (rounded_x < 1.0 || rounded_y < 1.0) {
        result.failure = PixelContractFailure::zoom_below_one;
        return result;
    }
    if (rounded_x != rounded_y) {
        result.failure = PixelContractFailure::anisotropic_zoom;
        return result;
    }
    result.integer_zoom = static_cast<std::uint32_t>(rounded_x);
    result.failure = PixelContractFailure::none;
    result.active = true;
    return result;
}

const char *pixelContractFailureName(PixelContractFailure failure) noexcept {
    switch (failure) {
    case PixelContractFailure::none: return "none";
    case PixelContractFailure::disabled: return "disabled";
    case PixelContractFailure::perspective_camera: return "perspective_camera";
    case PixelContractFailure::invalid_pixels_per_unit: return "invalid_pixels_per_unit";
    case PixelContractFailure::invalid_viewport: return "invalid_viewport";
    case PixelContractFailure::invalid_projection: return "invalid_projection";
    case PixelContractFailure::non_integer_zoom_x: return "non_integer_zoom_x";
    case PixelContractFailure::non_integer_zoom_y: return "non_integer_zoom_y";
    case PixelContractFailure::zoom_below_one: return "zoom_below_one";
    case PixelContractFailure::anisotropic_zoom: return "anisotropic_zoom";
    }
    return "unknown";
}

PixelSnapReason classifyStrictSprite(const PixelContract &contract,
                                     const StrictSpriteInput &input) {
    if (!contract.requested) return PixelSnapReason::not_requested;
    if (!contract.active) return PixelSnapReason::camera_contract_invalid;
    if (input.sampler != SamplerKey::nearest) return PixelSnapReason::linear_sampler;
    if (input.billboard != SpriteBillboard::none) return PixelSnapReason::billboard;
    if (input.source_width == 0 || input.source_height == 0)
        return PixelSnapReason::invalid_source_extent;
    if (!finiteBasis(input.view_basis_x) || !finiteBasis(input.view_basis_y))
        return PixelSnapReason::rotated_or_tilted;

    const auto x_scale = std::max({1.0, std::abs(input.view_basis_x[0]),
                                   std::abs(input.view_basis_x[1]),
                                   std::abs(input.view_basis_x[2])});
    const auto y_scale = std::max({1.0, std::abs(input.view_basis_y[0]),
                                   std::abs(input.view_basis_y[1]),
                                   std::abs(input.view_basis_y[2])});
    if (nearlyZero(input.view_basis_x[0], x_scale) ||
        nearlyZero(input.view_basis_y[1], y_scale) ||
        !nearlyZero(input.view_basis_x[1], x_scale) ||
        !nearlyZero(input.view_basis_x[2], x_scale) ||
        !nearlyZero(input.view_basis_y[0], y_scale) ||
        !nearlyZero(input.view_basis_y[2], y_scale)) {
        return PixelSnapReason::rotated_or_tilted;
    }

    const auto pixels_x = std::abs(input.view_basis_x[0]) /
                          contract.world_units_per_pixel_x /
                          static_cast<double>(input.source_width);
    const auto pixels_y = std::abs(input.view_basis_y[1]) /
                          contract.world_units_per_pixel_y /
                          static_cast<double>(input.source_height);
    const auto rounded_x = std::round(pixels_x);
    const auto rounded_y = std::round(pixels_y);
    if (rounded_x < 1.0 || rounded_y < 1.0 ||
        !closeToInteger(pixels_x, rounded_x) || !closeToInteger(pixels_y, rounded_y)) {
        return PixelSnapReason::non_integer_texel_scale;
    }
    return PixelSnapReason::eligible;
}

const char *pixelSnapReasonName(PixelSnapReason reason) noexcept {
    switch (reason) {
    case PixelSnapReason::not_requested: return "not_requested";
    case PixelSnapReason::camera_contract_invalid: return "camera_contract_invalid";
    case PixelSnapReason::eligible: return "eligible";
    case PixelSnapReason::linear_sampler: return "linear_sampler";
    case PixelSnapReason::billboard: return "billboard";
    case PixelSnapReason::rotated_or_tilted: return "rotated_or_tilted";
    case PixelSnapReason::invalid_source_extent: return "invalid_source_extent";
    case PixelSnapReason::non_integer_texel_scale: return "non_integer_texel_scale";
    }
    return "unknown";
}

double quantizePixelBoundary(double framebuffer_coordinate) noexcept {
    return std::floor(framebuffer_coordinate + 0.5);
}

} // namespace Pelican::sprite
