#pragma once

#include "../model/frameplanmodel.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// Q-14. A fullscreen pass is given an input its shader does not read (a pass
// whose project shader reads nothing, and a G-buffer dragged onto it; the
// Blank pipeline's present draws with engine://passthrough since VI-2, which
// takes any input). The engine's preflight refuses that correctly -- the pass's inputs and
// the shader's sampler declarations disagree -- but its sentence does not say
// what to do. These functions turn that one refusal into a line that says it,
// and name the two ways to fix it on the spot. They are pure, so a test reads
// the decision without a window or an engine.

// The two ways out, in the order they are offered. VI-3b: the first is no
// longer a shader written for the inputs but engine://passthrough, whose
// inputs are all variadic -- the inputs and that fragment go in one edit, and
// no file is written.
inline constexpr std::string_view ShaderInputContractUsePassthrough =
    "use_passthrough";
inline constexpr std::string_view ShaderInputContractOpenShader = "open_shader";

// Q-15 (after Q-14f): which way round the pass and its shader disagree, as
// the engine's shader_input_mismatch.direction names it.
// The pass lists an input the shader does not declare (Q-14e). A refusal with
// no direction is read as this one: it is what an engine before Q-14f meant.
inline constexpr std::string_view ShaderInputContractPassInputUndeclared =
    "pass_input_undeclared";
// The shader declares a pass input the pass does not supply, of a type the
// engine cannot fill for it (an input attachment, a storage image, a uniform
// buffer, ...). Giving the pass more inputs does not help: the declaration
// has to go.
inline constexpr std::string_view ShaderInputContractShaderInputUnsupplied =
    "shader_input_unsupplied";

struct ShaderInputContractAction {
    std::string id;
    std::string label;
};

struct ShaderInputContractNotice {
    // False for every other refusal: nothing below is then shown.
    bool matched = false;
    // ShaderInputContractPassInputUndeclared or
    // ShaderInputContractShaderInputUnsupplied.
    std::string direction{ShaderInputContractPassInputUndeclared};
    std::string pass;
    // The fragment reference as the pass names it, and the stem the line
    // shows ("project://shaders/present" -> "present").
    std::string fragment;
    std::string fragment_stem;
    // How many inputs the shader declares, when it is known: from the
    // engine's shader_input_mismatch first, then from its message, then from
    // the pass's published declarations (the caller's count, VI-3b).
    std::optional<std::size_t> declared;
    // The input that did not match, when the engine named it.
    std::string resource;
    // The binding the engine named. For shader_input_unsupplied, that of the
    // declaration the pass cannot supply, and its type in words ("input
    // attachment"), from the engine's shader_pass_inputs, else its message.
    std::optional<std::size_t> binding;
    std::string declared_type;
    // The one line the status row shows.
    std::string line;
    std::vector<ShaderInputContractAction> actions;
    // Open the shader can only open a file of the project -- or, VI-3b,
    // engine://passthrough, which it copies into the project first.
    bool can_open = false;
};

// Reads an engine refusal's `error` object. `pass` and `fragment` are what the
// panel sent; the engine's shader_input_mismatch, when it carries them, wins.
// `declared_by_shader` is the caller's count from the pass's published
// declarations (shaderInputDeclaredFor), used only when the engine gave none.
ShaderInputContractNotice shaderInputContractNotice(
    const nlohmann::json &error, std::string_view pass,
    std::string_view fragment,
    std::optional<std::size_t> declared_by_shader = std::nullopt);

// The line alone, for the Pass Editor's check before Save (no refusal yet).
std::string shaderInputContractLine(std::string_view pass,
                                    std::string_view fragment,
                                    std::optional<std::size_t> declared);

// Q-15. The line for shader_input_unsupplied: the shader declares an input
// the pass cannot supply, and the declaration is what to remove. `type` is in
// words ("input attachment"); either part may be unknown.
std::string shaderInputUnsuppliedLine(std::string_view pass,
                                      std::string_view fragment,
                                      std::string_view type,
                                      std::optional<std::size_t> binding);

// What the line says once Open the shader opened it for
// shader_input_unsupplied: nothing is copied, the declaration is removed.
std::string shaderInputUnsuppliedOpenedLine(std::optional<std::size_t> binding);

// The actions the line offers. Both are there for pass_input_undeclared (and
// with no direction); for shader_input_unsupplied only Open the shader, since
// no pass input fills a type the engine cannot. can_open says whether Open
// the shader has a project file to open.
std::vector<ShaderInputContractAction> shaderInputContractActions(
    std::string_view direction = ShaderInputContractPassInputUndeclared);
bool shaderInputContractOffers(const ShaderInputContractNotice &notice,
                               std::string_view action);

// --- VI-3b: the counts come from the declarations the engine published for
// the pass (get_frame_plan nodes[].pass_inputs), not from the file's lines.

// How many inputs the pass's shader takes: its fixed declarations, and up to
// the variadic group's max -- giving a variadic shader another input does not
// make it an input the shader "does not read".
std::size_t shaderInputCapacity(const FramePlanPassInputs &inputs);
// Its fixed image declarations: where the lines Open the shader copies start.
std::size_t shaderInputFixedImageCount(const FramePlanPassInputs &inputs);

// Whether the declarations are `fragment`'s: they describe the shader the
// pass runs now, and say nothing about another one. A trailing ".frag" is
// not a difference.
bool shaderInputDeclarationsAreFor(const FramePlanPassInputs &inputs,
                                   std::string_view fragment);

// The count a line about `fragment` may say: the capacity, when `inputs` are
// declared and are that fragment's; nothing otherwise.
std::optional<std::size_t> shaderInputDeclaredFor(
    const FramePlanPassInputs *inputs, std::string_view fragment);

// The Pass Editor's check before Save. Empty -- nothing to say -- when the
// pass has no published declarations (a new pass), when the draft names a
// fragment other than the one they describe, or when `listed` fits.
std::string shaderInputPreSaveLine(std::string_view pass,
                                   std::string_view draft_fragment,
                                   std::size_t listed,
                                   const FramePlanPassInputs *current);

// Where a project fragment reference lives: project://<path> is
// <project_root>/<path>, with .frag added when the reference has no
// extension (the stage decides it). Nothing for any other scheme, or for a
// path that would leave the project.
std::optional<std::filesystem::path> projectFragmentSourcePath(
    const std::filesystem::path &project_root, std::string_view fragment);

} // namespace PelicanStudio
