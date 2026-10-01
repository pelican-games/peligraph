#pragma once

#include <array>
#include <filesystem>
#include <functional>
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

// UX-5b, decision 1: the engine part of that list is the engine's own
// (get_graph_catalog.engine_shaders, EL-4), by stage, once a catalog has
// handed it over; the fixed table above is kept for an engine that did not
// list them. `ports_by_hand`: the engine could not reflect the shader alone
// (it includes the resource-port header the pass's ports generate), so its
// ports are connected by the author. Studio talks to one engine, so the list
// is the process's: whoever reads the catalog sets it, and every shader
// field (the Graph's and the Pass Editor's) reads it.
struct EngineShaderListEntry {
    std::string reference;
    FullscreenShaderStage stage = FullscreenShaderStage::fragment;
    bool ports_by_hand = false;

    bool operator==(const EngineShaderListEntry &) const = default;
};
// False when the list was already this one.
bool setEngineShaderList(std::vector<EngineShaderListEntry> entries);
const std::vector<EngineShaderListEntry> &engineShaderList();
bool engineShaderPortsByHand(std::string_view reference);
// What a shader list shows for a reference: the reference, and for one whose
// ports are connected by hand a mark after it. The tooltip says why.
std::string shaderChoiceLabel(std::string_view reference);
std::string shaderChoiceTooltip(std::string_view reference);
// EL-4b: the engine answers its list only when asked (get_engine_shaders,
// seconds of shader compiling), so a field that shows the list asks for it
// when it is opened. Whoever can ask the engine sets the requester; a field
// that shows the list is told when it changed (an observer per field, taken
// out by the id addEngineShaderListObserver gave).
void setEngineShaderListRequester(std::function<void()> requester);
void requestEngineShaderList();
std::size_t addEngineShaderListObserver(std::function<void()> observer);
void removeEngineShaderListObserver(std::size_t id);

// T-1. What a template reads fills itself in: for each role, in order, the
// declared resource named as G-1 names that role's target (gbuffer_albedo,
// gbuffer_normal, gbuffer_material, gbuffer_worldpos, gbuffer_emissive), else
// the only declared name that carries the role's word. The inputs are read by
// position, so the list stops at the first role nothing matches; an optional
// role is never filled. Empty for a kind with no roles filled this way (only
// Deferred lighting has them).
// UX-5b (RS-10): when the five are filled and `declared` holds shadow_map
// (the shadow_directional feature's), it follows them: the template's shadow.
std::vector<std::string> fullscreenTemplateInputFill(
    FullscreenShaderTemplateKind kind,
    const std::vector<std::string> &declared);

// UX-5b (RS-10): the target the shadow_directional feature declares, which a
// fullscreen pass lists among its inputs to be the feature's receiver. In the
// Deferred lighting template an input named so (or `<anything>_shadow_map`)
// is the directional shadow, not a G-buffer role: it is declared as
// PELICAN_TEMPLATE_SHADOW_MAP and sampled only when the feature defines
// PELICAN_FEATURE_SHADOW for the pass.
inline constexpr std::string_view ShadowMapInput = "shadow_map";
bool isTemplateShadowMapInput(std::string_view resource);

// UX-6a, decision 6: the Deferred lighting template's optional shadow mask
// (an R8 image: 1 lit, 0 shadowed, as engine://rt_shadow_mask_pipeline
// writes it). The key light's shadow is the smaller of the shadow map's and
// the mask's. An input named so (or `<anything>_shadow_mask`) is the mask;
// a pass with none gets the binding after its inputs declared all the same,
// so the node shows it as an empty socket to connect a mask to. A binding
// nothing is connected to reads zeros, alpha included, and the template
// reads that as no mask (1).
inline constexpr std::string_view ShadowMaskInput = "shadow_mask";
bool isTemplateShadowMaskInput(std::string_view resource);

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

// --- VI-3b: fixing a variadic group's sockets, and the engine's passthrough
// copied into a project.

// The engine fragment whose inputs are all variadic (VI-1), and the define
// that picks which one it shows (pass shader.defines, default 0).
inline constexpr std::string_view EnginePassthroughFragment =
    "engine://passthrough";
inline constexpr std::string_view PassthroughInputDefine =
    "PELICAN_PASSTHROUGH_INPUT";

// The source of an engine fragment Studio carries, byte for byte the file the
// engine compiles (embedded at build time from src/core/resources). Only
// engine://passthrough in v1; nothing for any other reference.
std::optional<std::string_view> engineFragmentSource(std::string_view reference);

// The name a copy of an engine shader takes in <project>/shaders: the pass's
// name (as suggestedShaderTemplateName folds it), then _2, _3... past every
// file already there, which is never overwritten.
std::string engineShaderCopyName(const std::filesystem::path &project_root,
                                 std::string_view pass);

// The two lines that bracket what Fix sockets wrote in place of the
// PELICAN_DECLARE_VARIADIC_INPUTS line. The first one carries the line it
// replaced, verbatim, after "was ".
inline constexpr std::string_view FixedSocketsBegin =
    "// pelican: fixed sockets, was ";
inline constexpr std::string_view FixedSocketsEnd =
    "// pelican: end fixed sockets";

struct ShaderSocketRewrite {
    std::string source;
    // Why nothing was rewritten; empty when `source` is the new text.
    std::string refusal;
    [[nodiscard]] bool ok() const noexcept { return refusal.empty(); }
};

// Fix sockets: the one PELICAN_DECLARE_VARIADIC_INPUTS(first, max); line of
// `source` becomes, between the two bracket lines, one raw layout line per
// connected input -- `layout(set = PELICAN_SET_PASS_INPUT, binding =
// <first+k>) uniform sampler2D in_<resource>;` -- and the function and the
// constant the variadic macros would have made
// (pelican_variadic_texture(int, vec2), pelican_variadic_input_count = n), so
// a body that reads PELICAN_VARIADIC_TEXTURE / PELICAN_VARIADIC_INPUT_COUNT
// still compiles. Raw layout lines, not PELICAN_DECLARE_INPUT_<i>, so more
// than eight inputs can be fixed. `inputs` are the connected members of the
// group in order, as the pass's `input` names them ("x@history" too);
// `taken` the variable names the shader already declares. The line ending is
// the replaced line's. Refused when there is no such line or more than one,
// and when no input is connected.
ShaderSocketRewrite fixShaderSockets(std::string_view source, std::size_t first,
                                     const std::vector<std::string> &inputs,
                                     const std::vector<std::string> &taken = {});

// Make sockets variadic: the bracketed block back to the line it replaced,
// byte for byte. Refused for a source without the brackets -- a declaration
// written by hand is never touched.
ShaderSocketRewrite variadicShaderSockets(std::string_view source);

// Whether `source` holds a block Fix sockets wrote.
bool shaderHasFixedSockets(std::string_view source);

} // namespace PelicanStudio
