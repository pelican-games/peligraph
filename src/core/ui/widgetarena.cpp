#include "widgetarena.hpp"

namespace Pelican::ui {

WidgetId WidgetArena::create(WidgetState value) {
    std::uint32_t index;
    if (free_.empty()) {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back({});
    } else {
        index = free_.back();
        free_.pop_back();
    }
    auto &slot = slots_[index];
    slot.value.emplace(std::move(value));
    ++live_count_;
    return {index, slot.generation};
}

bool WidgetArena::erase(WidgetId id) noexcept {
    auto *value = resolve(id);
    if (value == nullptr) return false;
    (void)value;
    auto &slot = slots_[id.index];
    slot.value.reset();
    ++slot.generation;
    if (slot.generation == 0) ++slot.generation;
    free_.push_back(id.index);
    --live_count_;
    return true;
}

WidgetState *WidgetArena::resolve(WidgetId id) noexcept {
    if (id.index >= slots_.size()) return nullptr;
    auto &slot = slots_[id.index];
    return slot.generation == id.generation && slot.value ? &*slot.value : nullptr;
}

const WidgetState *WidgetArena::resolve(WidgetId id) const noexcept {
    return const_cast<WidgetArena *>(this)->resolve(id);
}

void WidgetArena::clear() noexcept {
    for (std::uint32_t i = 0; i < slots_.size(); ++i) {
        auto &slot = slots_[i];
        if (slot.value) {
            slot.value.reset();
            ++slot.generation;
            if (slot.generation == 0) ++slot.generation;
        }
    }
    free_.clear();
    for (std::uint32_t i = 0; i < slots_.size(); ++i) free_.push_back(i);
    live_count_ = 0;
}

} // namespace Pelican::ui
