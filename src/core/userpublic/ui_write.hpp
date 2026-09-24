#pragma once

#include <cstdint>
#include <string_view>

namespace Pelican {

// Why a write through GameContext::ui() was refused. When more than one
// applies, the first in this order is the one reported.
enum class UiWriteError : std::uint32_t {
    // ui.feature_unreferenced@1: the render config does not reference the ui
    // feature, so the project has no UI to write to.
    feature_unreferenced = 1,
    // ui.node_unknown@1: the UI document has no node at that path, or the
    // document key names a document the project does not have.
    node_unknown = 2,
    // ui.property_mismatch@1: the node's type does not have the property
    // (text belongs to label and button, value to gauge), or the value is
    // not a finite number.
    property_mismatch = 3,
    // ui.sink_conflict@1: a ui.* sink of the render config's graph writes the
    // same (document, path, property). The graph is that value's one writer.
    sink_conflict = 4,
};

constexpr std::string_view uiWriteErrorName(UiWriteError error) noexcept {
    switch (error) {
    case UiWriteError::feature_unreferenced: return "ui.feature_unreferenced@1";
    case UiWriteError::node_unknown: return "ui.node_unknown@1";
    case UiWriteError::property_mismatch: return "ui.property_mismatch@1";
    case UiWriteError::sink_conflict: return "ui.sink_conflict@1";
    }
    return "ui.unknown_error@1";
}

// A write that was accepted, or the named reason it was not. An accepted
// write is queued and lands at the frame boundary after game code runs, so
// the frame being drawn next already shows it.
class UiWriteResult {
  public:
    UiWriteResult() = default;
    UiWriteResult(UiWriteError error) : error_{error}, ok_{false} {}

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // Meaningful only when !ok().
    UiWriteError error() const noexcept { return error_; }
    // The error's name, or empty when the write was accepted.
    std::string_view errorName() const noexcept {
        return ok_ ? std::string_view{} : uiWriteErrorName(error_);
    }

  private:
    UiWriteError error_ = UiWriteError::node_unknown;
    bool ok_ = true;
};

} // namespace Pelican
