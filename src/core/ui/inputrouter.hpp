#pragma once

#include "types.hpp"
#include "widgetarena.hpp"

#include "../os/inputstate.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Pelican::ui {

enum class PointerKind { Down, Move, Up, Cancel };
enum class PointerButton { Left, Right, Middle };
enum class PointerEffect { Capture, ReleaseCapture, Click, DragStart, Drag, Cancel, HoverEnter, HoverExit };

struct UiPointerEvent {
    std::uint64_t event_seq = 0;
    PointerKind kind = PointerKind::Move;
    std::uint8_t pointer_id = 0;
    std::optional<PointerButton> button;
    PointI position_px{};
};

struct RoutedPointerEvent {
    UiPointerEvent input;
    std::optional<WidgetId> target;
    std::optional<WidgetId> hover_target;
    PointI position_ui{};
    PointI drag_delta_ui{};
    std::vector<PointerEffect> effects;
};

struct RouteFrameResult {
    std::vector<RoutedPointerEvent> events;
    bool consumed_pointer = false;
};

using PointerHandler = std::function<void(const RoutedPointerEvent &)>;

class InputRouter {
  public:
    RouteFrameResult route(std::span<const UiPointerEvent> events, const WidgetArena &arena,
                           std::span<const WidgetId> traversal, const ViewportTransform &viewport,
                           const PointerHandler &handler = {});
    RouteFrameResult routeFrameInput(const FrameInput &input, const WidgetArena &arena,
                                     std::span<const WidgetId> traversal, const ViewportTransform &viewport,
                                     const PointerHandler &handler = {});
    std::optional<RoutedPointerEvent> cancelCapture(std::uint8_t pointer_id, const WidgetArena &arena,
                                                    std::uint64_t event_seq = 0);
    void reset() noexcept { pointers_.clear(); }

  private:
    struct Capture {
        WidgetId owner;
        PointerButton button;
        PointI last_ui{};
        PointI press_ui{};
        bool drag_started = false;
    };
    struct PointerState {
        std::optional<Capture> capture;
        std::optional<WidgetId> hover;
    };
    std::map<std::uint8_t, PointerState> pointers_;

    std::optional<WidgetId> hitTest(PointI position_px, const WidgetArena &arena,
                                    std::span<const WidgetId> traversal, const ViewportTransform &viewport) const;
};

std::string_view toString(PointerKind value) noexcept;
std::string_view toString(PointerButton value) noexcept;
std::string_view toString(PointerEffect value) noexcept;

} // namespace Pelican::ui
