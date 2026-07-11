#pragma once

#include "document.hpp"

#include <string>
#include <vector>

namespace Pelican::ui {

struct LayoutRecord {
    std::string id;
    std::string type;
    RectI rect_ui{};
    RectI clip_ui{};
    std::int32_t layer = 0;
    std::int32_t decl_seq = 0;
    bool overflow_clip = false;
};

struct LayoutResult {
    std::vector<LayoutRecord> widgets;
    std::vector<UiError> errors;
    explicit operator bool() const noexcept { return errors.empty(); }
};

LayoutResult solveLayout(const UiDocument &document, RectI content_rect_ui);

} // namespace Pelican::ui
