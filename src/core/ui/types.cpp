#include "types.hpp"

#include <algorithm>
#include <cassert>
#include <cfenv>
#include <cmath>
#include <limits>
#include <stdexcept>

#ifndef _MSC_VER
#pragma STDC FENV_ACCESS ON
#endif

namespace Pelican::ui {

RectI intersect(RectI a, RectI b) noexcept {
    RectI result{std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right),
                 std::min(a.bottom, b.bottom)};
    if (result.right < result.left) result.right = result.left;
    if (result.bottom < result.top) result.bottom = result.top;
    return result;
}

std::string_view toString(UiPhase value) noexcept {
    switch (value) {
    case UiPhase::Parse: return "parse";
    case UiPhase::Layout: return "layout";
    case UiPhase::Route: return "route";
    case UiPhase::Commit: return "commit";
    }
    return "parse";
}

std::string_view toString(UiErrorCode value) noexcept {
    switch (value) {
    case UiErrorCode::UnknownEvent: return "unknown_event";
    case UiErrorCode::UnknownSource: return "unknown_source";
    case UiErrorCode::UnknownWidgetValuePath: return "unknown_widget_value_path";
    case UiErrorCode::TypeMismatch: return "type_mismatch";
    case UiErrorCode::RangeViolation: return "range_violation";
    case UiErrorCode::RequiredMissing: return "required_missing";
    case UiErrorCode::UnknownField: return "unknown_field";
    case UiErrorCode::NoPayloadEvent: return "no_payload_event";
    case UiErrorCode::UnknownWidgetType: return "unknown_widget_type";
    case UiErrorCode::AxisConflict: return "axis_conflict";
    case UiErrorCode::LayoutCycle: return "layout_cycle";
    case UiErrorCode::DuplicateStableId: return "duplicate_stable_id";
    case UiErrorCode::LimitExceeded: return "limit_exceeded";
    case UiErrorCode::UnsupportedControl: return "unsupported_control";
    }
    return "type_mismatch";
}

static std::int64_t checkedFloor(double value) {
    if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        value >= -static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        throw std::overflow_error("UI coordinate exceeds int64");
    }
    return static_cast<std::int64_t>(std::floor(value));
}

std::int64_t anchorEdge(std::int64_t parent_edge, std::int64_t parent_size, double anchor) {
    assert(std::fegetround() == FE_TONEAREST && "UI layout requires FE_TONEAREST");
    const double t1 = static_cast<double>(parent_size) * anchor;
    const double t2 = static_cast<double>(parent_edge) + t1;
    const double t3 = t2 + 0.5;
    return checkedFloor(t3);
}

std::int64_t scaleEdge(std::int32_t edge, double ui_scale) {
    assert(std::fegetround() == FE_TONEAREST && "UI layout requires FE_TONEAREST");
    return checkedFloor(static_cast<double>(edge) * ui_scale + 0.5);
}

static std::int32_t checkedI32(std::int64_t value) {
    if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max() ||
        std::abs(value) > (INT64_C(1) << 24)) {
        throw std::overflow_error("UI pixel coordinate exceeds the exact float32 integer domain");
    }
    return static_cast<std::int32_t>(value);
}

ViewportTransform ViewportTransform::letterboxed(PointI framebuffer, double scale) {
    if (framebuffer.x <= 0 || framebuffer.y <= 0 || !std::isfinite(scale) || scale <= 0.0 || scale > 16.0) {
        throw std::invalid_argument("invalid UI viewport");
    }
    const auto ui_w = static_cast<std::int32_t>(std::floor(framebuffer.x / scale));
    const auto ui_h = static_cast<std::int32_t>(std::floor(framebuffer.y / scale));
    const auto px_w = checkedI32(scaleEdge(ui_w, scale));
    const auto px_h = checkedI32(scaleEdge(ui_h, scale));
    const auto left = (framebuffer.x - px_w) / 2;
    const auto top = (framebuffer.y - px_h) / 2;
    return {framebuffer, scale, {left, top, left + px_w, top + px_h}, {0, 0, ui_w, ui_h}};
}

PointI ViewportTransform::windowToUi(double x, double y) const {
    return {static_cast<std::int32_t>(std::floor((x - content_rect_px.left) / ui_scale)),
            static_cast<std::int32_t>(std::floor((y - content_rect_px.top) / ui_scale))};
}

RectI ViewportTransform::uiToPx(RectI rect) const {
    return {checkedI32(scaleEdge(rect.left, ui_scale) + content_rect_px.left),
            checkedI32(scaleEdge(rect.top, ui_scale) + content_rect_px.top),
            checkedI32(scaleEdge(rect.right, ui_scale) + content_rect_px.left),
            checkedI32(scaleEdge(rect.bottom, ui_scale) + content_rect_px.top)};
}

} // namespace Pelican::ui
