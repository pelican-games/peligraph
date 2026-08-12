#pragma once

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view editorFeatureOverlayReference =
    "engine://features/editor.json";

using RenderFeatureOverlayLoader =
    std::function<std::string(std::string_view reference)>;

// Applies separately supplied startup overlays to an authored rendering
// config. Project parsing never calls this function: launch/tooling code must
// opt into this named path and provide both the references and their loader.
nlohmann::json applyRenderFeatureOverlays(
    const nlohmann::json &authored_config,
    std::span<const std::string> overlay_references,
    const RenderFeatureOverlayLoader &load_overlay_json);

} // namespace Pelican
