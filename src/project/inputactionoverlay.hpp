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

enum class EditorTransformInputPreset {
    Blender,
    Grab,
};

// A bare --editor-transform has to mean what the flag says it does. Grab binds
// no key at all - editor_transform_grab.json is "bindings": [] - so defaulting
// to it made the bare flag identical to omitting the flag, and the studio, which
// passes it bare, shipped with G/R/S dead. Grab remains reachable by asking for
// it, which is the only way a caller would ever want handle-dragging-only.
inline constexpr EditorTransformInputPreset defaultEditorTransformInputPreset =
    EditorTransformInputPreset::Blender;
inline constexpr std::string_view
    editorTransformBlenderInputActionOverlayReference =
        "engine://input/overlays/editor_transform_blender.json";
inline constexpr std::string_view
    editorTransformGrabInputActionOverlayReference =
        "engine://input/overlays/editor_transform_grab.json";

std::string_view editorTransformInputPresetName(
    EditorTransformInputPreset preset) noexcept;
std::string_view editorTransformInputActionOverlayReference(
    EditorTransformInputPreset preset) noexcept;

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
