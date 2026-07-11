#include "commandbuffer.hpp"

#include <stdexcept>

namespace Pelican::ui {

void UiCommandBuffer::push(UiCommand command) {
    if (committing_) throw std::logic_error("UI command enqueue during commit");
    commands_.push_back(std::move(command));
}

CommitStatus UiCommandBuffer::commit(WidgetArena &arena) {
    if (committing_) throw std::logic_error("recursive UI command commit");
    committing_ = true;
    CommitStatus status;
    for (const auto &command : commands_) {
        const bool applied = std::visit([&](const auto &typed) {
            using T = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<T, RemoveWidget>) return arena.erase(typed.target);
            auto *widget = arena.resolve(typed.target);
            if (widget == nullptr) return false;
            if constexpr (std::is_same_v<T, SetVisibility>) widget->visible = typed.visible;
            if constexpr (std::is_same_v<T, SetEnabled>) widget->enabled = typed.enabled;
            if constexpr (std::is_same_v<T, SetRect>) widget->rect_ui = typed.rect;
            return true;
        }, command);
        applied ? ++status.applied : ++status.stale;
    }
    commands_.clear();
    committing_ = false;
    return status;
}

std::size_t UiCommandBuffer::discard() noexcept {
    const auto count = commands_.size();
    commands_.clear();
    return count;
}

} // namespace Pelican::ui
