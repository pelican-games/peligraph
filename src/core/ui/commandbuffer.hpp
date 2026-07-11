#pragma once

#include "widgetarena.hpp"

#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace Pelican::ui {

struct SetVisibility { WidgetId target; bool visible; };
struct SetEnabled { WidgetId target; bool enabled; };
struct SetRect { WidgetId target; RectI rect; };
struct RemoveWidget { WidgetId target; };
using UiCommand = std::variant<SetVisibility, SetEnabled, SetRect, RemoveWidget>;

struct CommitStatus {
    std::size_t applied = 0;
    std::size_t stale = 0;
    std::size_t dropped = 0;
};

class UiCommandBuffer {
  public:
    void push(UiCommand command);
    std::size_t size() const noexcept { return commands_.size(); }
    bool empty() const noexcept { return commands_.empty(); }
    CommitStatus commit(WidgetArena &arena);
    std::size_t discard() noexcept;

  private:
    std::vector<UiCommand> commands_;
    bool committing_ = false;
};

} // namespace Pelican::ui
