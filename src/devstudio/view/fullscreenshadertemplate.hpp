#pragma once

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// F-3. A fullscreen pass is built shader and all from the Pass Editor: "New..."
// beside the fragment stem writes a starting shader into the project and puts
// its stem in the field. Everything here is a pure function of its arguments,
// or a single file write, so a test can hold it without a window or an engine.

// One entry of the pass's `input` list, as the form holds it. `image` is false
// for a resource the frame plan publishes as a buffer: that one has no
// PELICAN_DECLARE_INPUT form and is left to the author, by name.
struct FullscreenShaderTemplateInput {
    std::string resource;
    bool image = true;
};

// The engine's include offers PELICAN_DECLARE_INPUT_0 ... _7 (pelican_view.glsl).
inline constexpr std::size_t FullscreenShaderTemplateMacroInputs = 8;

// G-3. The starting points "New..." offers.
enum class FullscreenShaderTemplateKind {
    // F-3's: input 0 written through, or a UV gradient with no input.
    pass_through,
    // A G-buffer lit by the scene lights (pelican_lighting_v1.glsl).
    deferred_lighting,
    // A scene-linear colour turned into display colour, once.
    tone_map,
};

inline constexpr std::array FullscreenShaderTemplateKinds{
    FullscreenShaderTemplateKind::pass_through,
    FullscreenShaderTemplateKind::deferred_lighting,
    FullscreenShaderTemplateKind::tone_map,
};

// A stable name for a kind ("pass_through", "deferred_lighting", "tone_map")
// and the words the Pass Editor shows for it.
std::string_view fullscreenShaderTemplateKindName(
    FullscreenShaderTemplateKind kind);
std::optional<FullscreenShaderTemplateKind>
fullscreenShaderTemplateKindFromName(std::string_view name);
std::string_view fullscreenShaderTemplateKindLabel(
    FullscreenShaderTemplateKind kind);

// What a kind reads at each input position, in order: the Pass Editor shows
// these beside the input list. Empty for a kind that reads inputs as they are.
struct FullscreenShaderTemplateRole {
    std::string_view name;
    bool optional = false;
};
std::span<const FullscreenShaderTemplateRole> fullscreenShaderTemplateInputRoles(
    FullscreenShaderTemplateKind kind);

// The pass settings a kind's shader needs from the pass that runs it. An empty
// push_constants asks for none.
struct FullscreenShaderTemplatePassSettings {
    bool uses_light_data = false;
    std::string_view push_constants;
};
FullscreenShaderTemplatePassSettings fullscreenShaderTemplatePassSettings(
    FullscreenShaderTemplateKind kind);

// The shader text: one declaration and one read per image input, in list
// order. Pass-through: with no input it draws the UV gradient; with inputs it
// writes input 0 through unchanged, so a new pass shows something before it is
// edited. Deferred lighting and tone map read their inputs by role; a role the
// list does not fill is a named constant, so the shader still compiles.
std::string fullscreenFragmentShaderTemplate(
    std::string_view pass_name,
    std::span<const FullscreenShaderTemplateInput> inputs,
    FullscreenShaderTemplateKind kind =
        FullscreenShaderTemplateKind::pass_through);

// Q-14. The lines "Open the shader" puts on the clipboard: for each image
// input from `first` on, its declaration (for the top of the file) and its read
// (for main), named exactly as the template above names them.
//   PELICAN_DECLARE_INPUT_0(in_gbuffer_albedo);
//   vec4 value0 = PELICAN_TEXTURE_2D_0(in_gbuffer_albedo, inUV);
// An input the macros cannot declare (a buffer, or past input 7) gets the
// same comment the template writes for it.
std::string fullscreenShaderInputSnippet(
    std::span<const FullscreenShaderTemplateInput> inputs, std::size_t first);

// A name that can only land in <project>/shaders: letters, digits, '_' and
// '-', starting with a letter, digit or '_'. No separator, no dot, no drive.
bool isValidShaderTemplateName(std::string_view name);

// The name "New..." proposes for a pass: the pass name with every character a
// shader name cannot hold replaced by '_'. Empty when nothing usable is left.
std::string suggestedShaderTemplateName(std::string_view pass_name);

// The stem the fragment field takes: project://shaders/<name>, no extension --
// the stage decides it (shaderreference.cpp refuses an explicit one).
std::string shaderTemplateReference(std::string_view name);

// Where the source of that stem lives: <project>/shaders/<name>.frag.
std::filesystem::path shaderTemplatePath(
    const std::filesystem::path &project_root, std::string_view name);

struct ShaderTemplateWrite {
    std::filesystem::path path;
    std::string reference;
    // False when a file was already there. It is then left exactly as it was.
    bool created = false;
};

// T-1. What the Pass Editor's shader fields offer to pick from, so a stem is
// chosen rather than spelled: the project's own shaders/<name>.frag (or
// .vert), as project://shaders/<name> in name order, then the engine's
// fullscreen shaders the render compiler knows by name. The fields still take
// any text. A project without a shaders folder offers the engine's alone.
enum class FullscreenShaderStage { vertex, fragment };
std::vector<std::string> fullscreenShaderChoices(
    FullscreenShaderStage stage,
    const std::optional<std::filesystem::path> &project_root);

// T-1. What a template reads fills itself in: for each role, in order, the
// declared resource named as G-1 names that role's target (gbuffer_albedo,
// gbuffer_normal, gbuffer_material, gbuffer_worldpos, gbuffer_emissive), else
// the only declared name that carries the role's word. The inputs are read by
// position, so the list stops at the first role nothing matches; an optional
// role is never filled. Empty for a kind with no roles filled this way (only
// Deferred lighting has them).
std::vector<std::string> fullscreenTemplateInputFill(
    FullscreenShaderTemplateKind kind,
    const std::vector<std::string> &declared);

// T-1. The target "New target:" in the colour output's list proposes:
// <pass>_color, every character a target name cannot hold folded to '_'
// ("fullscreen_pass_color" for a pass with no name yet), then _2, _3... past
// the names already taken.
std::string proposedColorTargetName(std::string_view pass_name,
                                    const std::vector<std::string> &taken);

// Writes `contents` to shaderTemplatePath() unless a file is already there.
// Throws for an invalid name or a write that fails; never overwrites.
ShaderTemplateWrite writeShaderTemplate(
    const std::filesystem::path &project_root, std::string_view name,
    std::string_view contents);

} // namespace PelicanStudio
