#pragma once

#include "passfieldownership.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Pelican {

// RC-3a2: what a raster pass does with an attachment before and after it
// draws, in the engine's own terms. The planner, the physical plan and the
// executor all hold these; core maps them to Vulkan's load / store ops in
// vulkanimagevocabulary, once, where a rendering scope is recorded.
enum class AttachmentAspect : std::uint8_t {
    color,
    depth,
};

enum class AttachmentLoadOp : std::uint8_t {
    load,
    clear,
    dont_care,
};

enum class AttachmentStoreOp : std::uint8_t {
    store,
    dont_care,
};

struct PassAttachmentOperations {
    AttachmentLoadOp load_op;
    AttachmentStoreOp store_op;

    // No default: a resolved attachment always states both, from the pass's
    // own fields or defaultPassAttachmentOperations.
    PassAttachmentOperations() = delete;
    explicit constexpr PassAttachmentOperations(
        AttachmentLoadOp load,
        AttachmentStoreOp store) noexcept
        : load_op{load}, store_op{store} {}

    bool operator==(
        const PassAttachmentOperations &) const = default;
};

// The sole authority for omitted attachment operations: what a pass of this
// type does with an attachment of this aspect when neither the attachment
// nor the pass says.
PassAttachmentOperations defaultPassAttachmentOperations(
    RenderPassType pass_type, AttachmentAspect aspect);

// The order every reader resolves an attachment's operations in: the
// attachment's own, then the pass's, then the defaults.
constexpr PassAttachmentOperations resolveAttachmentOperations(
    std::optional<AttachmentLoadOp> attachment_load_op,
    std::optional<AttachmentStoreOp> attachment_store_op,
    std::optional<AttachmentLoadOp> pass_load_op,
    std::optional<AttachmentStoreOp> pass_store_op,
    PassAttachmentOperations defaults) {
    return PassAttachmentOperations{
        attachment_load_op.value_or(
            pass_load_op.value_or(defaults.load_op)),
        attachment_store_op.value_or(
            pass_store_op.value_or(defaults.store_op)),
    };
}

// Spellings. One table (attachmentoperations.cpp) holds every spelling of
// every op; each way in admits its own set of them:
//   authored (a pass's color_load_op ..., an attachment's load_op / store_op,
//   Studio's schema): load "clear" "Clear" "load" "Load" "dont_care"
//   "DontCare" "dontCare"; store "store" "Store" "dont_care" "DontCare"
//   "dontCare"
//   normalized rendering JSON: the canonical "clear" / "load" / "dont_care"
//   and "store" / "dont_care"
//   physical plan (fragment and complete-plan packages, the plan dump and
//   its fingerprint): "clear" / "load" / "discard" and "store" / "discard"
std::string_view attachmentAspectName(AttachmentAspect aspect);
std::string_view attachmentLoadOpName(AttachmentLoadOp op);
std::string_view attachmentStoreOpName(AttachmentStoreOp op);
std::string_view attachmentLoadOpPlanName(AttachmentLoadOp op);
std::string_view attachmentStoreOpPlanName(AttachmentStoreOp op);

std::optional<AttachmentLoadOp> findAuthoredAttachmentLoadOp(
    std::string_view spelling);
std::optional<AttachmentStoreOp> findAuthoredAttachmentStoreOp(
    std::string_view spelling);
// findAuthored*, throwing "Unknown attachment load op: <spelling>" /
// "Unknown attachment store op: <spelling>".
AttachmentLoadOp parseAuthoredAttachmentLoadOp(std::string_view spelling);
AttachmentStoreOp parseAuthoredAttachmentStoreOp(std::string_view spelling);
std::optional<AttachmentLoadOp> findPlanAttachmentLoadOp(
    std::string_view spelling);
std::optional<AttachmentStoreOp> findPlanAttachmentStoreOp(
    std::string_view spelling);

// Every authored spelling, canonical first, for Studio's choices.
std::span<const std::string_view> authoredAttachmentLoadOpSpellings();
std::span<const std::string_view> authoredAttachmentStoreOpSpellings();

} // namespace Pelican
