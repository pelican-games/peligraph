#pragma once

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

struct MaterialDrawLoopPassRule {
    // The graph node the rule came from, reported in refusals. Empty when
    // the rule was not written in a graph.
    std::string node;
    MaterialDrawLoopRule rule;
};

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
