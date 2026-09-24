#pragma once

#include <cstdint>
#include <string_view>

namespace Pelican {

// Why a query through GameContext::render() has no answer. It travels in a
// Resolved<T, RenderError> (object_ref.hpp), as ObjectError does for the
// objects facade.
enum class RenderError : std::uint32_t {
    // render.screen_ray_unavailable@1: the frame is not drawn from one flat
    // camera -- XR is active, and its eyes are views of their own -- so a
    // screen position names no single ray.
    screen_ray_unavailable = 1,
    // render.screen_position_invalid@1: the position is not a finite number.
    screen_position_invalid = 2,
};

constexpr std::string_view renderErrorName(RenderError error) noexcept {
    switch (error) {
    case RenderError::screen_ray_unavailable: return "render.screen_ray_unavailable@1";
    case RenderError::screen_position_invalid: return "render.screen_position_invalid@1";
    }
    return "render.unknown_error@1";
}

constexpr std::string_view errorCodeName(RenderError error) noexcept {
    return renderErrorName(error);
}

} // namespace Pelican
