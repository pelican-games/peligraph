#pragma once

#include "types.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Pelican::ui {

struct WidgetId {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    auto operator<=>(const WidgetId &) const = default;
    explicit operator bool() const noexcept { return index != std::numeric_limits<std::uint32_t>::max(); }
};

struct WidgetState {
    std::string stable_id;
    std::string type;
    RectI rect_ui{};
    RectI clip_ui{};
    std::int32_t layer = 0;
    std::int32_t decl_seq = 0;
    bool visible = true;
    bool enabled = true;
    bool hit_testable = true;
    bool overflow_clip = false;
};

class WidgetArena {
  public:
    WidgetId create(WidgetState value);
    bool erase(WidgetId id) noexcept;
    WidgetState *resolve(WidgetId id) noexcept;
    const WidgetState *resolve(WidgetId id) const noexcept;
    std::size_t liveCount() const noexcept { return live_count_; }
    void clear() noexcept;

  private:
    struct Slot {
        std::optional<WidgetState> value;
        std::uint32_t generation = 1;
    };
    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_;
    std::size_t live_count_ = 0;
};

} // namespace Pelican::ui
