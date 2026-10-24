#include "attachmentoperations.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

namespace {

// One row per op: the canonical (normalized) spelling, the physical plan's,
// and the other spellings an author may write.
struct LoadOpSpelling {
    AttachmentLoadOp op;
    std::string_view canonical;
    std::string_view plan;
    std::array<std::string_view, 2> aliases;
};

struct StoreOpSpelling {
    AttachmentStoreOp op;
    std::string_view canonical;
    std::string_view plan;
    std::array<std::string_view, 2> aliases;
};

constexpr std::array kLoadOps{
    LoadOpSpelling{AttachmentLoadOp::clear, "clear", "clear", {"Clear", {}}},
    LoadOpSpelling{AttachmentLoadOp::load, "load", "load", {"Load", {}}},
    LoadOpSpelling{AttachmentLoadOp::dont_care, "dont_care", "discard",
                   {"DontCare", "dontCare"}},
};

constexpr std::array kStoreOps{
    StoreOpSpelling{AttachmentStoreOp::store, "store", "store", {"Store", {}}},
    StoreOpSpelling{AttachmentStoreOp::dont_care, "dont_care", "discard",
                    {"DontCare", "dontCare"}},
};

// Canonical spellings first, then the aliases, in the table's order.
template <typename Rows>
std::vector<std::string_view> authoredSpellings(const Rows &rows) {
    std::vector<std::string_view> result;
    for (const auto &row : rows) result.push_back(row.canonical);
    for (const auto &row : rows)
        for (const auto alias : row.aliases)
            if (!alias.empty()) result.push_back(alias);
    return result;
}

template <typename Rows, typename Op>
const auto &rowOf(const Rows &rows, Op op) {
    const auto found = std::find_if(rows.begin(), rows.end(),
                                    [&](const auto &row) { return row.op == op; });
    if (found == rows.end()) {
        throw std::logic_error("unknown attachment operation");
    }
    return *found;
}

template <typename Rows>
auto findAuthored(const Rows &rows, std::string_view spelling)
    -> std::optional<decltype(rows.front().op)> {
    for (const auto &row : rows) {
        if (row.canonical == spelling) return row.op;
        for (const auto alias : row.aliases)
            if (!alias.empty() && alias == spelling) return row.op;
    }
    return std::nullopt;
}

template <typename Rows>
auto findPlan(const Rows &rows, std::string_view spelling)
    -> std::optional<decltype(rows.front().op)> {
    for (const auto &row : rows)
        if (row.plan == spelling) return row.op;
    return std::nullopt;
}

} // namespace

PassAttachmentOperations defaultPassAttachmentOperations(
    RenderPassType pass_type, AttachmentAspect aspect) {
    switch (aspect) {
    case AttachmentAspect::color:
        return PassAttachmentOperations{
            pass_type == RenderPassType::ui ||
                    pass_type == RenderPassType::imgui
                ? AttachmentLoadOp::load
                : AttachmentLoadOp::clear,
            AttachmentStoreOp::store,
        };
    case AttachmentAspect::depth:
        return PassAttachmentOperations{
            AttachmentLoadOp::clear,
            AttachmentStoreOp::dont_care,
        };
    }
    throw std::logic_error("unknown pass attachment aspect");
}

std::string_view attachmentAspectName(AttachmentAspect aspect) {
    switch (aspect) {
    case AttachmentAspect::color: return "color";
    case AttachmentAspect::depth: return "depth";
    }
    throw std::logic_error("unknown attachment aspect");
}

std::string_view attachmentLoadOpName(AttachmentLoadOp op) {
    return rowOf(kLoadOps, op).canonical;
}

std::string_view attachmentStoreOpName(AttachmentStoreOp op) {
    return rowOf(kStoreOps, op).canonical;
}

std::string_view attachmentLoadOpPlanName(AttachmentLoadOp op) {
    return rowOf(kLoadOps, op).plan;
}

std::string_view attachmentStoreOpPlanName(AttachmentStoreOp op) {
    return rowOf(kStoreOps, op).plan;
}

std::optional<AttachmentLoadOp> findAuthoredAttachmentLoadOp(
    std::string_view spelling) {
    return findAuthored(kLoadOps, spelling);
}

std::optional<AttachmentStoreOp> findAuthoredAttachmentStoreOp(
    std::string_view spelling) {
    return findAuthored(kStoreOps, spelling);
}

AttachmentLoadOp parseAuthoredAttachmentLoadOp(std::string_view spelling) {
    if (const auto op = findAuthoredAttachmentLoadOp(spelling)) return *op;
    throw std::runtime_error("Unknown attachment load op: " + std::string{spelling});
}

AttachmentStoreOp parseAuthoredAttachmentStoreOp(std::string_view spelling) {
    if (const auto op = findAuthoredAttachmentStoreOp(spelling)) return *op;
    throw std::runtime_error("Unknown attachment store op: " + std::string{spelling});
}

std::optional<AttachmentLoadOp> findPlanAttachmentLoadOp(
    std::string_view spelling) {
    return findPlan(kLoadOps, spelling);
}

std::optional<AttachmentStoreOp> findPlanAttachmentStoreOp(
    std::string_view spelling) {
    return findPlan(kStoreOps, spelling);
}

std::span<const std::string_view> authoredAttachmentLoadOpSpellings() {
    static const auto spellings = authoredSpellings(kLoadOps);
    return spellings;
}

std::span<const std::string_view> authoredAttachmentStoreOpSpellings() {
    static const auto spellings = authoredSpellings(kStoreOps);
    return spellings;
}

} // namespace Pelican
