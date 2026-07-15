#pragma once

#include "sampler.hpp"
#include "../components/spriteview.hpp"

#include <array>
#include <cstdint>

namespace Pelican::sprite {

inline constexpr double pixelContractTolerance = 1.0e-4;

enum class PixelContractFailure : std::uint8_t {
    none,
    disabled,
    perspective_camera,
    invalid_pixels_per_unit,
    invalid_viewport,
    invalid_projection,
    non_integer_zoom_x,
    non_integer_zoom_y,
    zoom_below_one,
    anisotropic_zoom,
};

struct PixelContract {
    bool requested = false;
    bool active = false;
    double pixels_per_unit = 100.0;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    double world_units_per_pixel_x = 0.0;
    double world_units_per_pixel_y = 0.0;
    double zoom_x = 0.0;
    double zoom_y = 0.0;
    std::uint32_t integer_zoom = 0;
    PixelContractFailure failure = PixelContractFailure::disabled;
};

PixelContract evaluatePixelContract(bool requested, bool orthographic,
                                    double xmag, double ymag, double pixels_per_unit,
                                    std::uint32_t viewport_width,
                                    std::uint32_t viewport_height);
const char *pixelContractFailureName(PixelContractFailure failure) noexcept;

enum class PixelSnapReason : std::uint8_t {
    not_requested,
    camera_contract_invalid,
    eligible,
    linear_sampler,
    billboard,
    rotated_or_tilted,
    invalid_source_extent,
    non_integer_texel_scale,
};

struct StrictSpriteInput {
    SamplerKey sampler = SamplerKey::linear;
    SpriteBillboard billboard = SpriteBillboard::none;
    // Columns of view * world for the local unit-quad X and Y axes. The
    // translation is irrelevant to eligibility and is quantized in the shader.
    std::array<double, 3> view_basis_x{};
    std::array<double, 3> view_basis_y{};
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
};

PixelSnapReason classifyStrictSprite(const PixelContract &contract,
                                     const StrictSpriteInput &input);
const char *pixelSnapReasonName(PixelSnapReason reason) noexcept;

// Strict rendering snaps the projected local (0,0) atlas corner to the nearest
// pixel boundary. Texel samples then land at framebuffer centers n + 0.5.
double quantizePixelBoundary(double framebuffer_coordinate) noexcept;

} // namespace Pelican::sprite
