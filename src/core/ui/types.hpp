#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::ui {

struct PointI {
    std::int32_t x = 0;
    std::int32_t y = 0;
    auto operator<=>(const PointI &) const = default;
};

struct RectI {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    auto operator<=>(const RectI &) const = default;

    std::int64_t width() const noexcept { return std::int64_t{right} - left; }
    std::int64_t height() const noexcept { return std::int64_t{bottom} - top; }
    bool ordered() const noexcept { return left <= right && top <= bottom; }
    bool contains(PointI p) const noexcept {
        return p.x >= left && p.x < right && p.y >= top && p.y < bottom;
    }
};

RectI intersect(RectI a, RectI b) noexcept;

enum class UiPhase { Parse, Layout, Route, Commit };
enum class UiErrorCode {
    UnknownEvent,
    UnknownSource,
    UnknownWidgetValuePath,
    TypeMismatch,
    RangeViolation,
    RequiredMissing,
    UnknownField,
    NoPayloadEvent,
    UnknownWidgetType,
    AxisConflict,
    LayoutCycle,
    DuplicateStableId,
    LimitExceeded,
    UnsupportedControl,
};

struct UiError {
    UiPhase phase = UiPhase::Parse;
    UiErrorCode code = UiErrorCode::TypeMismatch;
    std::string path;
    std::string message;
    auto operator<=>(const UiError &) const = default;
};

std::string_view toString(UiPhase value) noexcept;
std::string_view toString(UiErrorCode value) noexcept;

// The exact design operation: floor((binary64(edge) + binary64(size)*anchor) + 0.5).
// Throws std::overflow_error if the int64 result cannot be represented.
std::int64_t anchorEdge(std::int64_t parent_edge, std::int64_t parent_size, double anchor);
std::int64_t scaleEdge(std::int32_t edge, double ui_scale);

struct ViewportTransform {
    PointI framebuffer_px{};
    double ui_scale = 1.0;
    RectI content_rect_px{};
    RectI content_rect_ui{};

    static ViewportTransform letterboxed(PointI framebuffer_px, double ui_scale);
    PointI windowToUi(double framebuffer_x, double framebuffer_y) const;
    RectI uiToPx(RectI rect) const;
};

} // namespace Pelican::ui
