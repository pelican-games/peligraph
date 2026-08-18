#pragma once

#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string_view>

namespace Pelican {

enum class RenderPassType {
    material,
    fullscreen,
    raster,
    output_transform,
    debug_draw,
    gizmo,
    debug_text,
    shadow_depth,
    velocity,
    picking,
    ui,
    imgui,
    canonical_anchor,
    snapshot_copy,
};

struct PassFieldOwnershipCapabilities {
    bool imgui_enabled = false;
};

struct PassFieldOwnershipEntry {
    RenderPassType type;
    std::string_view type_name;
    std::span<const std::string_view> fields;
    bool requires_imgui = false;
};

// This table is the schema authority for valid pass types and every field
// whose meaning is owned by one or more pass types. Common pass fields such as
// name, input, output, and ordering constraints are intentionally not listed.
std::span<const PassFieldOwnershipEntry> passFieldOwnershipTable();

// Returns the authored JSON fields emitted by a pass-authoring surface for
// the selected type. Unlike passFieldOwnershipTable(), this schema includes
// the common authored fields (name/type/input/output) as well as the
// type-owned fields represented by that surface. It is therefore the
// authority for projection tests and must not be inferred from UI widgets.
std::span<const std::string_view> passAuthoringProjectionFields(
    RenderPassType type);

// Validates the type and all type-owned fields, returning the resolved type.
// Build-dependent capabilities are data, rather than pelican_project compile
// definitions, so every caller makes its supported schema explicit.
RenderPassType validatePassFieldOwnership(
    const nlohmann::json &pass,
    PassFieldOwnershipCapabilities capabilities,
    std::string_view context = "pass");

std::string_view renderPassTypeName(RenderPassType type);

} // namespace Pelican
