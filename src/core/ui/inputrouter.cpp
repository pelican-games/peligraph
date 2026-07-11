#include "inputrouter.hpp"

#include <algorithm>
#include <cmath>

namespace Pelican::ui {

std::string_view toString(PointerKind value) noexcept {
    switch (value) { case PointerKind::Down: return "pointer_down"; case PointerKind::Move: return "pointer_move";
    case PointerKind::Up: return "pointer_up"; case PointerKind::Cancel: return "pointer_cancel"; }
    return "pointer_move";
}
std::string_view toString(PointerButton value) noexcept {
    switch (value) { case PointerButton::Left: return "left"; case PointerButton::Right: return "right";
    case PointerButton::Middle: return "middle"; }
    return "left";
}
std::string_view toString(PointerEffect value) noexcept {
    switch (value) { case PointerEffect::Capture: return "capture"; case PointerEffect::ReleaseCapture: return "release_capture";
    case PointerEffect::Click: return "click"; case PointerEffect::DragStart: return "drag_start";
    case PointerEffect::Drag: return "drag"; case PointerEffect::Cancel: return "cancel";
    case PointerEffect::HoverEnter: return "hover_enter"; case PointerEffect::HoverExit: return "hover_exit"; }
    return "capture";
}

std::optional<WidgetId> InputRouter::hitTest(PointI px, const WidgetArena &arena, std::span<const WidgetId> traversal,
                                             const ViewportTransform &viewport) const {
    if (!viewport.content_rect_px.contains(px)) return std::nullopt;
    const WidgetState *best = nullptr;
    std::optional<WidgetId> best_id;
    for (const auto id : traversal) {
        const auto *widget = arena.resolve(id);
        if (widget == nullptr || !widget->visible || !widget->enabled || !widget->hit_testable) continue;
        const auto rect = intersect(viewport.uiToPx(widget->rect_ui), viewport.uiToPx(widget->clip_ui));
        if (!rect.contains(px)) continue;
        if (best == nullptr || widget->layer > best->layer ||
            (widget->layer == best->layer && widget->decl_seq > best->decl_seq)) {
            best = widget;
            best_id = id;
        }
    }
    return best_id;
}

RouteFrameResult InputRouter::route(std::span<const UiPointerEvent> events, const WidgetArena &arena,
                                    std::span<const WidgetId> traversal, const ViewportTransform &viewport,
                                    const PointerHandler &handler) {
    RouteFrameResult result;
    std::uint64_t previous = 0;
    bool first = true;
    for (const auto &event : events) {
        if (!first && event.event_seq <= previous) throw std::invalid_argument("UI input event_seq must be strictly increasing");
        first = false; previous = event.event_seq;
        auto &state = pointers_[event.pointer_id];
        RoutedPointerEvent routed{.input = event, .position_ui = viewport.windowToUi(event.position_px.x, event.position_px.y)};
        auto physical_target = event.kind == PointerKind::Cancel ? std::optional<WidgetId>{} : hitTest(event.position_px, arena, traversal, viewport);
        if (state.capture && arena.resolve(state.capture->owner) == nullptr) state.capture.reset();
        if (state.hover && arena.resolve(*state.hover) == nullptr) state.hover.reset();

        if (event.kind == PointerKind::Move) {
            if (physical_target != state.hover) {
                if (state.hover) routed.effects.push_back(PointerEffect::HoverExit);
                if (physical_target) routed.effects.push_back(PointerEffect::HoverEnter);
                state.hover = physical_target;
            }
            if (state.capture) {
                routed.target = state.capture->owner;
                routed.drag_delta_ui = {routed.position_ui.x - state.capture->last_ui.x,
                                        routed.position_ui.y - state.capture->last_ui.y};
                state.capture->last_ui = routed.position_ui;
                const auto dx = std::abs(routed.position_ui.x - state.capture->press_ui.x);
                const auto dy = std::abs(routed.position_ui.y - state.capture->press_ui.y);
                if (!state.capture->drag_started && dx + dy >= 4) {
                    state.capture->drag_started = true;
                    routed.effects.push_back(PointerEffect::DragStart);
                }
                if (state.capture->drag_started) routed.effects.push_back(PointerEffect::Drag);
            } else routed.target = physical_target;
        } else if (event.kind == PointerKind::Down) {
            if (!event.button) throw std::invalid_argument("pointer down requires button");
            if (state.capture) throw std::invalid_argument("second button down while pointer is captured");
            routed.target = physical_target;
            if (physical_target) {
                state.capture = Capture{*physical_target, *event.button, routed.position_ui, routed.position_ui, false};
                routed.effects.push_back(PointerEffect::Capture);
            }
        } else if (event.kind == PointerKind::Up) {
            if (!event.button) throw std::invalid_argument("pointer up requires button");
            if (state.capture) {
                if (*event.button != state.capture->button) throw std::invalid_argument("pointer up button differs from capture");
                routed.target = state.capture->owner;
                routed.effects.push_back(PointerEffect::ReleaseCapture);
                if (!state.capture->drag_started && physical_target == state.capture->owner)
                    routed.effects.push_back(PointerEffect::Click);
                state.capture.reset();
            } else routed.target = physical_target;
        } else {
            if (event.button) throw std::invalid_argument("pointer cancel must not carry button");
            if (state.capture) {
                routed.target = state.capture->owner;
                routed.effects = {PointerEffect::Cancel, PointerEffect::ReleaseCapture};
                state.capture.reset();
            }
            state.hover.reset();
        }
        if (routed.target || state.capture) result.consumed_pointer = true;
        if (handler) handler(routed);
        result.events.push_back(std::move(routed));
    }
    return result;
}

RouteFrameResult InputRouter::routeFrameInput(const FrameInput &input, const WidgetArena &arena,
                                              std::span<const WidgetId> traversal, const ViewportTransform &viewport,
                                              const PointerHandler &handler) {
    std::vector<UiPointerEvent> converted;
    PointI position{static_cast<std::int32_t>(input.snapshot.mouse_x), static_cast<std::int32_t>(input.snapshot.mouse_y)};
    for (const auto &event : input.ordered_events) {
        UiPointerEvent out{.event_seq = event.event_seq, .pointer_id = 0, .position_px = position};
        if (event.type == InputEvent::Type::cursor_move) {
            position = {static_cast<std::int32_t>(event.mouse_x), static_cast<std::int32_t>(event.mouse_y)};
            out.position_px = position;
            out.kind = PointerKind::Move;
        } else if (event.type == InputEvent::Type::button &&
                   (event.code == KeyCode::MouseLeft || event.code == KeyCode::MouseRight || event.code == KeyCode::MouseMiddle)) {
            out.kind = event.pressed ? PointerKind::Down : PointerKind::Up;
            out.button = event.code == KeyCode::MouseLeft ? PointerButton::Left
                       : event.code == KeyCode::MouseRight ? PointerButton::Right : PointerButton::Middle;
        } else continue;
        converted.push_back(out);
    }
    return route(converted, arena, traversal, viewport, handler);
}

std::optional<RoutedPointerEvent> InputRouter::cancelCapture(std::uint8_t pointer_id, const WidgetArena &arena,
                                                             std::uint64_t event_seq) {
    auto it = pointers_.find(pointer_id);
    if (it == pointers_.end() || !it->second.capture) return std::nullopt;
    RoutedPointerEvent result;
    result.input = {.event_seq = event_seq, .kind = PointerKind::Cancel, .pointer_id = pointer_id};
    result.target = it->second.capture->owner;
    result.position_ui = it->second.capture->last_ui;
    result.effects = {PointerEffect::Cancel, PointerEffect::ReleaseCapture};
    it->second.capture.reset();
    it->second.hover.reset();
    (void)arena;
    return result;
}

} // namespace Pelican::ui
