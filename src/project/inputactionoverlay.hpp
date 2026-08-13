#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::string_view freeCameraBlenderInputActionOverlayReference =
    "engine://input/overlays/free_camera_blender.json";
inline constexpr std::string_view freeCameraUnityInputActionOverlayReference =
    "engine://input/overlays/free_camera_unity.json";

using InputActionOverlayLoader =
    std::function<std::string(std::string_view reference)>;

struct AppliedInputActionOverlay {
    std::string name;
    std::string reference;
    std::string actions_json;
    std::optional<std::string> profile_json;
    std::vector<std::string> action_set_names;
};

struct InputActionOverlayApplication {
    std::optional<std::string> effective_actions_json;
    std::vector<AppliedInputActionOverlay> overlays;
};

// Applies separately supplied startup overlays to authored input actions.
// Project parsing never calls this function: launch/tooling code must opt into
// this named path and provide both the references and their loader.
InputActionOverlayApplication applyInputActionOverlays(
    const std::optional<std::string> &authored_actions_json,
    std::span<const std::string> overlay_references,
    const InputActionOverlayLoader &load_overlay_json);

} // namespace Pelican
