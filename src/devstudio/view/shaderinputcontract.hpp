#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// Q-14. A fullscreen pass is given an input its shader does not read (the
// Blank pipeline's present, which reads nothing, and a G-buffer dragged onto
// it). The engine's preflight refuses that correctly -- the pass's inputs and
// the shader's sampler declarations disagree -- but its sentence does not say
// what to do. These functions turn that one refusal into a line that says it,
// and name the two ways to fix it on the spot. They are pure, so a test reads
// the decision without a window or an engine.

// The two ways out, in the order they are offered.
inline constexpr std::string_view ShaderInputContractMakePassThrough =
    "make_pass_through";
inline constexpr std::string_view ShaderInputContractOpenShader = "open_shader";

struct ShaderInputContractAction {
    std::string id;
    std::string label;
};

struct ShaderInputContractNotice {
    // False for every other refusal: nothing below is then shown.
    bool matched = false;
    std::string pass;
    // The fragment reference as the pass names it, and the stem the line
    // shows ("project://shaders/present" -> "present").
    std::string fragment;
    std::string fragment_stem;
    // How many inputs the shader declares, when it is known: from the
    // engine's shader_input_mismatch first, then from its message, then from
    // counting the file's declaration lines (the caller's count).
    std::optional<std::size_t> declared;
    // The input that did not match, when the engine named it.
    std::string resource;
    // The one line the status row shows.
    std::string line;
    std::vector<ShaderInputContractAction> actions;
    // Open the shader can only open a file of the project.
    bool can_open = false;
};

// Reads an engine refusal's `error` object. `pass` and `fragment` are what the
// panel sent; the engine's shader_input_mismatch, when it carries them, wins.
// `declared_in_file` is the caller's count of the fragment file's declaration
// lines (shaderInputDeclarationCount), used only when the engine gave none.
ShaderInputContractNotice shaderInputContractNotice(
    const nlohmann::json &error, std::string_view pass,
    std::string_view fragment,
    std::optional<std::size_t> declared_in_file = std::nullopt);

// The line alone, for the Pass Editor's check before Save (no refusal yet).
std::string shaderInputContractLine(std::string_view pass,
                                    std::string_view fragment,
                                    std::optional<std::size_t> declared);

// The actions the line offers. Both are always there; can_open says whether
// Open the shader has a project file to open.
std::vector<ShaderInputContractAction> shaderInputContractActions();

// The simple check before Save: the pass-input indices a fragment source
// declares, counted from its lines, not from reflection. A line counts when,
// outside a // comment, it calls PELICAN_DECLARE_INPUT_<i>( or declares
// `set = PELICAN_SET_PASS_INPUT, binding = <i>` by hand. #define lines do not.
// Distinct indices, in ascending order.
std::vector<std::size_t> shaderInputDeclaredIndices(std::string_view source);
std::size_t shaderInputDeclarationCount(std::string_view source);

// Where a project fragment reference lives: project://<path> is
// <project_root>/<path>, with .frag added when the reference has no
// extension (the stage decides it). Nothing for any other scheme, or for a
// path that would leave the project.
std::optional<std::filesystem::path> projectFragmentSourcePath(
    const std::filesystem::path &project_root, std::string_view fragment);

// The file's declaration count, or nothing when it cannot be read.
std::optional<std::size_t> projectFragmentDeclarationCount(
    const std::filesystem::path &project_root, std::string_view fragment);

// The shader name Make a pass-through shader writes: <pass>_pass_through
// (T-1's folding of the pass name), then _2, _3... past every file already in
// <project_root>/shaders, which is never overwritten.
std::string passThroughShaderName(const std::filesystem::path &project_root,
                                  std::string_view pass);

} // namespace PelicanStudio
