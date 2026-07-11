#include "layout.hpp"

#include <algorithm>
#include <cassert>
#include <cfenv>
#include <limits>
#include <numeric>
#include <stdexcept>

#ifndef _MSC_VER
#pragma STDC FENV_ACCESS ON
#endif

namespace Pelican::ui {
namespace {

struct Solver {
    LayoutResult result;

    void fail(UiErrorCode code, const DocumentNode &node, std::string message) {
        result.errors.push_back({UiPhase::Layout, code, "/widgets/" + node.path, std::move(message)});
    }

    static std::int32_t edge(std::int64_t value) {
        if (value < INT32_MIN || value > INT32_MAX) throw std::overflow_error("UI rect exceeds int32");
        return static_cast<std::int32_t>(value);
    }

    PointI intrinsic(const DocumentNode &node) {
        if (node.visibility == Visibility::Collapsed) return {};
        std::int64_t width = node.intrinsic_size.x;
        std::int64_t height = node.intrinsic_size.y;
        if (!node.children.empty()) {
            if (node.layout.stack == StackDirection::Horizontal) {
                width = node.layout.padding.left + node.layout.padding.right;
                height = 0;
                bool first = true;
                for (const auto &child : node.children) {
                    const auto value = intrinsic(child);
                    if (child.visibility == Visibility::Collapsed) continue;
                    if (!first) width += node.layout.gap;
                    width += value.x;
                    height = std::max<std::int64_t>(height, value.y);
                    first = false;
                }
                height += node.layout.padding.top + node.layout.padding.bottom;
            } else if (node.layout.stack == StackDirection::Vertical) {
                height = node.layout.padding.top + node.layout.padding.bottom;
                width = 0;
                bool first = true;
                for (const auto &child : node.children) {
                    const auto value = intrinsic(child);
                    if (child.visibility == Visibility::Collapsed) continue;
                    if (!first) height += node.layout.gap;
                    height += value.y;
                    width = std::max<std::int64_t>(width, value.x);
                    first = false;
                }
                width += node.layout.padding.left + node.layout.padding.right;
            } else {
                // Only absolute (non-stretch) children participate in a content panel.
                for (const auto &child : node.children) {
                    const auto &l = child.layout;
                    if (l.anchor_min_x != l.anchor_max_x || l.anchor_min_y != l.anchor_max_y) continue;
                    const auto value = intrinsic(child);
                    width = std::max<std::int64_t>(width, std::int64_t{l.offsets.left} + value.x);
                    height = std::max<std::int64_t>(height, std::int64_t{l.offsets.top} + value.y);
                }
            }
        }
        if (width < 0 || height < 0 || width > INT32_MAX || height > INT32_MAX)
            throw std::overflow_error("intrinsic size exceeds int32");
        return {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
    }

    static std::int64_t axisSize(const AxisSpec &axis, std::int32_t intrinsic_value) {
        std::int64_t size = axis.mode == SizeMode::Content ? intrinsic_value : axis.value;
        return std::clamp<std::int64_t>(size, axis.min, axis.max);
    }

    RectI anchored(const DocumentNode &node, RectI parent, PointI content) {
        const auto pw = parent.width();
        const auto ph = parent.height();
        auto left = anchorEdge(parent.left, pw, node.layout.anchor_min_x) + node.layout.offsets.left;
        auto top = anchorEdge(parent.top, ph, node.layout.anchor_min_y) + node.layout.offsets.top;
        auto right = anchorEdge(parent.left, pw, node.layout.anchor_max_x) + node.layout.offsets.right;
        auto bottom = anchorEdge(parent.top, ph, node.layout.anchor_max_y) + node.layout.offsets.bottom;
        if (node.layout.anchor_min_x == node.layout.anchor_max_x) right = left + axisSize(node.layout.x, content.x);
        if (node.layout.anchor_min_y == node.layout.anchor_max_y) bottom = top + axisSize(node.layout.y, content.y);
        return {edge(left), edge(top), edge(right), edge(bottom)};
    }

    std::vector<std::int64_t> distribute(const std::vector<const DocumentNode *> &children, std::int64_t available,
                                         bool horizontal) {
        std::vector<std::int64_t> sizes(children.size());
        std::vector<std::size_t> fill;
        std::int64_t fixed = 0;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const auto &node = *children[i];
            const auto content = intrinsic(node);
            const auto &axis = horizontal ? node.layout.x : node.layout.y;
            if (axis.mode == SizeMode::Fill) fill.push_back(i);
            else { sizes[i] = axisSize(axis, horizontal ? content.x : content.y); fixed += sizes[i]; }
        }
        auto remaining = available - fixed;
        if (remaining < 0) {
            for (auto i : fill) sizes[i] = (horizontal ? children[i]->layout.x : children[i]->layout.y).min;
            return sizes;
        }
        std::vector<std::size_t> pool = fill;
        while (!pool.empty()) {
            std::uint64_t total_weight = 0;
            for (auto i : pool) total_weight += horizontal ? children[i]->layout.x.weight : children[i]->layout.y.weight;
            bool clamped = false;
            for (auto it = pool.begin(); it != pool.end();) {
                const auto i = *it;
                const auto &axis = horizontal ? children[i]->layout.x : children[i]->layout.y;
                const auto share = total_weight == 0 ? 0 : remaining * axis.weight / static_cast<std::int64_t>(total_weight);
                const auto bounded = std::clamp<std::int64_t>(share, axis.min, axis.max);
                if (bounded != share) {
                    sizes[i] = bounded;
                    remaining -= bounded;
                    total_weight -= axis.weight;
                    it = pool.erase(it);
                    clamped = true;
                } else ++it;
            }
            if (!clamped) {
                std::int64_t assigned = 0;
                for (auto i : pool) {
                    const auto weight = horizontal ? children[i]->layout.x.weight : children[i]->layout.y.weight;
                    sizes[i] = remaining * weight / static_cast<std::int64_t>(total_weight);
                    assigned += sizes[i];
                }
                for (std::int64_t rem = remaining - assigned; rem > 0; --rem) ++sizes[pool[(remaining - assigned - rem) % pool.size()]];
                break;
            }
        }
        return sizes;
    }

    void place(const DocumentNode &node, RectI rect, RectI inherited_clip) {
        if (node.visibility == Visibility::Collapsed) return;
        const auto clip = intersect(inherited_clip, rect);
        result.widgets.push_back({node.path, node.type, rect, inherited_clip, node.layer, node.decl_seq, node.overflow_clip});
        const auto child_clip = node.overflow_clip ? clip : inherited_clip;
        if (node.layout.stack == StackDirection::None) {
            for (const auto &child : node.children) {
                if (node.layout.x.mode == SizeMode::Content && child.layout.x.mode == SizeMode::Fill) {
                    fail(UiErrorCode::LayoutCycle, child, "content parent contains fill child");
                    continue;
                }
                if (node.layout.y.mode == SizeMode::Content && child.layout.y.mode == SizeMode::Fill) {
                    fail(UiErrorCode::LayoutCycle, child, "content parent contains fill child");
                    continue;
                }
                place(child, anchored(child, rect, intrinsic(child)), child_clip);
            }
            return;
        }
        const bool horizontal = node.layout.stack == StackDirection::Horizontal;
        std::vector<const DocumentNode *> children;
        for (const auto &child : node.children) if (child.visibility != Visibility::Collapsed) {
            if (child.layout.anchor_min_x != 0 || child.layout.anchor_max_x != 0 ||
                child.layout.anchor_min_y != 0 || child.layout.anchor_max_y != 0) {
                fail(UiErrorCode::AxisConflict, child, "stack child must not define anchors");
            }
            children.push_back(&child);
        }
        if (children.empty()) return;
        const auto main_extent = horizontal ? rect.width() - node.layout.padding.left - node.layout.padding.right
                                            : rect.height() - node.layout.padding.top - node.layout.padding.bottom;
        const auto available = main_extent - std::int64_t{node.layout.gap} * (children.size() - 1);
        const auto sizes = distribute(children, available, horizontal);
        const auto used = std::accumulate(sizes.begin(), sizes.end(), std::int64_t{0}) +
                          std::int64_t{node.layout.gap} * (children.size() - 1);
        const auto spare = std::max<std::int64_t>(0, main_extent - used);
        std::int64_t leading = 0;
        std::int64_t extra_gap = 0;
        std::int64_t gap_remainder = 0;
        if (node.layout.justify == Justify::Center) leading = spare / 2;
        else if (node.layout.justify == Justify::End) leading = spare;
        else if (node.layout.justify == Justify::SpaceBetween && children.size() > 1) {
            extra_gap = spare / static_cast<std::int64_t>(children.size() - 1);
            gap_remainder = spare % static_cast<std::int64_t>(children.size() - 1);
        }
        std::int64_t cursor = horizontal ? std::int64_t{rect.left} + node.layout.padding.left
                                         : std::int64_t{rect.top} + node.layout.padding.top;
        cursor += leading;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const auto &child = *children[i];
            const auto content = intrinsic(child);
            RectI child_rect;
            if (horizontal) {
                const auto available_cross = rect.height() - node.layout.padding.top - node.layout.padding.bottom;
                const auto cross = node.layout.align == Align::Stretch ? available_cross : axisSize(child.layout.y, content.y);
                const auto cross_spare = std::max<std::int64_t>(0, available_cross - cross);
                const auto cross_offset = node.layout.align == Align::Center ? cross_spare / 2 : node.layout.align == Align::End ? cross_spare : 0;
                const auto top = std::int64_t{rect.top} + node.layout.padding.top + cross_offset;
                child_rect = {edge(cursor), edge(top), edge(cursor + sizes[i]), edge(top + cross)};
            } else {
                const auto available_cross = rect.width() - node.layout.padding.left - node.layout.padding.right;
                const auto cross = node.layout.align == Align::Stretch ? available_cross : axisSize(child.layout.x, content.x);
                const auto cross_spare = std::max<std::int64_t>(0, available_cross - cross);
                const auto cross_offset = node.layout.align == Align::Center ? cross_spare / 2 : node.layout.align == Align::End ? cross_spare : 0;
                const auto left = std::int64_t{rect.left} + node.layout.padding.left + cross_offset;
                child_rect = {edge(left), edge(cursor), edge(left + cross), edge(cursor + sizes[i])};
            }
            place(child, child_rect, child_clip);
            cursor += sizes[i] + node.layout.gap + extra_gap + (static_cast<std::int64_t>(i) < gap_remainder ? 1 : 0);
        }
    }
};

} // namespace

LayoutResult solveLayout(const UiDocument &document, RectI content_rect_ui) {
    Solver solver;
    assert(std::fegetround() == FE_TONEAREST && "UI layout requires FE_TONEAREST");
    if (!content_rect_ui.ordered()) {
        solver.result.errors.push_back({UiPhase::Layout, UiErrorCode::RangeViolation, "/viewport/content_rect_ui", "inverted content rect"});
        return std::move(solver.result);
    }
    try {
        (void)solver.intrinsic(document.root);
        solver.place(document.root, content_rect_ui, content_rect_ui);
    } catch (const std::overflow_error &e) {
        solver.result.errors.push_back({UiPhase::Layout, UiErrorCode::LimitExceeded, "/widgets", e.what()});
        solver.result.widgets.clear();
    }
    return std::move(solver.result);
}

} // namespace Pelican::ui
