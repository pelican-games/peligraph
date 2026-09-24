#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

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

// The shader text: one declaration and one read per image input, in list
// order. With no input it draws the UV gradient; with inputs it writes input 0
// through unchanged, so a new pass shows something before it is edited.
std::string fullscreenFragmentShaderTemplate(
    std::string_view pass_name,
    std::span<const FullscreenShaderTemplateInput> inputs);

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

// Writes `contents` to shaderTemplatePath() unless a file is already there.
// Throws for an invalid name or a write that fails; never overwrites.
ShaderTemplateWrite writeShaderTemplate(
    const std::filesystem::path &project_root, std::string_view name,
    std::string_view contents);

} // namespace PelicanStudio
