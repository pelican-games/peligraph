#pragma once

#include "graphloop.hpp"
#include "renderpipeline.hpp"
#include "surfaceformat.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// LP-2: how one material pass draws one range (the consecutive draws of one
// material). A pass without a rule draws each material the way it always
// has: one step, the pass's material_variant when it names one, the material
// itself otherwise. A rule replaces that with a list of steps, drawn in
// order; an empty list draws nothing.
//
// The named refusals travel as "<code>: <JSON fields>", the same way the
// view_select refusals do, so splitCodedRefusalMessage recovers them.

// {node, material, binding}: an override's pipeline layout (a set layout's
// bindings and stage flags, or the push-constant ranges) differs from the
// base material's, so it cannot share the base's descriptor sets.
inline constexpr std::string_view materialDrawOverrideInterfaceCode =
    "graph.loop_override_interface@1";
// {node}: an override with defines needs the runtime shader compiler and
// this build has none.
inline constexpr std::string_view materialDrawOverrideNeedsCompilerCode =
    "graph.loop_override_needs_compiler@1";
// {node, material}: an override moves the material between the opaque and
// the transparent phase (adds blending to an opaque material, or removes it
// from a transparent one).
inline constexpr std::string_view materialDrawPhaseCrossingCode =
    "graph.loop_phase_crossing@1";
// {node, material, variant}: a variant step names a variant the material
// does not have.
inline constexpr std::string_view materialDrawVariantMissingCode =
    "graph.loop_variant_missing@1";
// {node, count, limit}: one generation would compile more define overrides
// than materialDrawOverrideCompileLimit.
inline constexpr std::string_view materialDrawOverrideCompileLimitCode =
    "graph.loop_override_compile_limit@1";

// Define overrides one generation may compile. Each distinct (shader pair,
// define set) compiles the material's vertex and fragment shader again; LP-2
// measured about 210 ms per pair with a shader-cache miss (32 took 6.7 s), so
// 16 bounds the stall of one render-pipeline publication to about 3.5 s.
// Materials sharing a surface share one compile.
inline constexpr std::size_t materialDrawOverrideCompileLimit = 16;

inline bool isMaterialDrawLoopRefusalCode(std::string_view code) noexcept {
    return code == materialDrawOverrideInterfaceCode ||
           code == materialDrawOverrideNeedsCompilerCode ||
           code == materialDrawPhaseCrossingCode ||
           code == materialDrawVariantMissingCode ||
           code == materialDrawOverrideCompileLimitCode;
}

[[noreturn]] void throwMaterialDrawLoopRefusal(
    std::string_view code, const nlohmann::json &fields);

// Every field left unset keeps the base material's state.
struct MaterialDrawRenderStateOverride {
    std::optional<SurfaceBlendMode> blend;
    std::optional<SurfaceCullMode> cull;
    std::optional<bool> depth_test;
    std::optional<bool> depth_write;
    std::optional<SurfaceDepthCompare> depth_compare;

    bool empty() const noexcept {
        return !blend && !cull && !depth_test && !depth_write &&
               !depth_compare;
    }
    bool operator==(const MaterialDrawRenderStateOverride &) const =
        default;
};

enum class MaterialDrawStepKind {
    // The material itself: its pipeline, descriptor set and GPU slot.
    material,
    // A named variant of the material (a separately registered record).
    variant,
    // The material's descriptor set and GPU slot with another pipeline:
    // the base pipeline description with the render state and defines
    // replaced. No material is registered for it.
    override_pipeline,
};

struct MaterialDrawStep {
    MaterialDrawStepKind kind = MaterialDrawStepKind::material;
    // variant only.
    std::string variant;
    // override_pipeline only.
    MaterialDrawRenderStateOverride render_state;
    std::vector<std::string> defines;

    static MaterialDrawStep material() { return {}; }
    static MaterialDrawStep named(std::string variant_name) {
        MaterialDrawStep step;
        step.kind = MaterialDrawStepKind::variant;
        step.variant = std::move(variant_name);
        return step;
    }
    static MaterialDrawStep overridden(
        MaterialDrawRenderStateOverride render_state,
        std::vector<std::string> defines = {}) {
        MaterialDrawStep step;
        step.kind = MaterialDrawStepKind::override_pipeline;
        step.render_state = std::move(render_state);
        step.defines = std::move(defines);
        return step;
    }

    bool operator==(const MaterialDrawStep &) const = default;
};

using MaterialDrawStepList = std::vector<MaterialDrawStep>;

// What a rule sees of one registered material. Everything here is fixed
// when the material is registered; values are not part of it.
struct MaterialDrawSubject {
    // The matching key (§14.2 of the loop design). A project material is
    // "<material document>#<material name>"; a glTF material is
    // "<asset>#material[<index>]" whatever its name (glTF names may repeat
    // or be empty); anything else is its registered name.
    std::string key;
    std::string name;
    std::vector<std::string> tags;
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    MaterialShaderContract contract = MaterialShaderContract::gbuffer_v1;
    bool skinned = false;
    // Sorted variant names; has_variant is !variants.empty().
    std::vector<std::string> variants;
    // LP-3a: the one material pass the material asks for by name, if any
    // (materialPassAcceptsMaterial).
    std::optional<std::string> exact_pass;
};

std::string materialDrawSubjectKey(
    std::string_view name, std::string_view source_document,
    std::string_view source_asset,
    std::optional<std::uint32_t> source_material_index);

bool materialRouteIsTransparent(MaterialRouteClass route) noexcept;

// nullopt leaves the material to the pass's default drawing (one step, as
// today); a list replaces it.
using MaterialDrawLoopRule =
    std::function<std::optional<MaterialDrawStepList>(
        const MaterialDrawSubject &)>;

// LP-3a: which branch of a draw_loop a material takes. `served` is false when
// the branch has a body that draws nothing (material_route_unserved@1); a
// `"body": null` branch draws nothing on purpose and is served.
struct MaterialDrawLoopChoice {
    std::size_t branch = 0;
    bool served = true;
};
// nullopt: the material is outside the loop's over / where.
using MaterialDrawLoopClassifier =
    std::function<std::optional<MaterialDrawLoopChoice>(
        const MaterialDrawSubject &)>;

struct MaterialDrawLoopPassRule {
    // The graph node the rule came from, reported in refusals. Empty when
    // the rule was not written in a graph.
    std::string node;
    MaterialDrawLoopRule rule;
    // LP-3a: set when the rule is a graph.loop over draw_items. The frame
    // plan's loops[] elements and the served check read it.
    MaterialDrawLoopClassifier classify;
    std::size_t branch_count = 0;
};

// LP-3a: a graph.loop over draw_items as the draw_loop stage runs it inside
// the material pass `pass` (docs/design_graph_loops.md 4.1).
struct GraphDrawLoop {
    // The graph.loop id.
    std::string loop;
    // The render.material_pass id, which is the pass name.
    std::string pass;
    MaterialPassContract contract = MaterialPassContract::forward_opaque_v1;
    // The tags `where` tests (the pass's material_filter, and require_all).
    std::optional<GraphLoopTagFilter> where;
    struct Branch {
        GraphLoopDrawCondition when;
        // nullopt for `"body": null`: the material is not drawn in the pass.
        // Otherwise the body's draw.* chain, first to last.
        std::optional<MaterialDrawStepList> steps;
    };
    std::vector<Branch> branches;
};

bool graphLoopTagFilterMatches(const GraphLoopTagFilter &filter,
                               const std::vector<std::string> &tags);
bool graphLoopDrawConditionMatches(const GraphLoopDrawCondition &condition,
                                   const MaterialDrawSubject &subject);
// over (the pass accepts the material: route, shader contract, exact pass)
// and where, then the first branch whose `when` holds.
std::optional<MaterialDrawLoopChoice> classifyGraphDrawLoopSubject(
    const GraphDrawLoop &loop, const MaterialDrawSubject &subject);
// The rule the draw-step table evaluates: outside over or the tags of where
// (include / exclude) -- nullopt, the pass's default drawing, which draws
// none of them (the queue filter or the route leaves them out); outside
// require_all -- nothing; else the branch's steps, nothing for a null body.
MaterialDrawLoopPassRule makeGraphDrawLoopPassRule(GraphDrawLoop loop);

// LP-3a: get_frame_plan's draw_loop entries (loops[] with "tier":
// "draw_loop" and "pass") list the materials of the running generation, in
// registration order: elements [{element: <matching key>, index, branch}],
// count, and the branches none of them took. A material outside the loop's
// over / where is not an element.
void applyMaterialDrawLoopElementsToFramePlanJson(
    nlohmann::json &plan, const MaterialDrawLoopRules &rules,
    const std::vector<MaterialDrawSubject> &subjects);

// LP-3b: get_frame_plan's paging of a draw_loop's elements. The request is
// `get_frame_plan {loop_elements: {node, offset?, limit?}}` (node: the
// graph.loop id; offset defaults to 0, limit to 256 and is 1..4096); every
// other draw_loop gets the default page.
inline constexpr std::size_t drawLoopElementPageDefaultLimit = 256;
inline constexpr std::size_t drawLoopElementPageMaxLimit = 4096;

struct DrawLoopElementPage {
    std::string node;
    std::size_t offset = 0;
    std::size_t limit = drawLoopElementPageDefaultLimit;
};

// get_frame_plan's params: `{}` gives nullopt. Throws std::invalid_argument
// for anything else than `loop_elements` of the shape above.
std::optional<DrawLoopElementPage> parseDrawLoopElementPage(const nlohmann::json &params);

// Each draw_loop entry of plan.loops keeps elements [offset, offset + limit)
// -- `page`'s for page->node, the first 256 for the others -- and gains
// `total` (every element; `count` stays that too), `offset` and `limit`.
// Returns false, changing nothing, when page names no draw_loop of the plan.
bool pageDrawLoopElements(nlohmann::json &plan, const std::optional<DrawLoopElementPage> &page);

// LP-3a, section 4.2.5: the materials a draw_loop leaves unserved -- inside
// its over and where, on a branch whose body draws nothing (a `"body": null`
// branch counts as served). material_route_unserved@1 lists them.
std::vector<std::size_t> materialDrawLoopUnservedSubjects(
    const MaterialDrawLoopRules &rules,
    const std::vector<MaterialDrawSubject> &subjects);

// Keyed by material pass name. A generation evaluates these for every
// registered material when it is prepared, and again for each material
// registered while it is alive.
struct MaterialDrawLoopRules {
    std::map<std::string, MaterialDrawLoopPassRule, std::less<>> passes;

    bool empty() const noexcept { return passes.empty(); }
};

// The refusals a step can be judged by without a GPU: phase crossing, a
// missing variant, and defines in a build without the runtime compiler.
// Throws the named refusal.
void validateMaterialDrawStep(const MaterialDrawStep &step,
                              const MaterialDrawSubject &subject,
                              std::string_view node,
                              bool runtime_shader_compiler);

// The render-state half of an override applied to a surface render state.
SurfaceRenderState applyMaterialDrawRenderStateOverride(
    SurfaceRenderState base,
    const MaterialDrawRenderStateOverride &override_state);

// Stable text of an override step: equal overrides share one pipeline per
// base pipeline.
std::string materialDrawOverrideKey(const MaterialDrawStep &step);

} // namespace Pelican
