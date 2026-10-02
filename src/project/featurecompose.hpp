#pragma once

#include "renderpipeline.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct RenderFeatureComposeDependencies {
    std::function<std::string(std::string_view)> load_feature_json;
    bool runtime_shader_compiler_enabled = false;
    // Called after the feature envelope has been validated and before it is
    // composed.  This is a startup-only policy seam used by immutable graph
    // variants; returning false removes the whole feature from that variant.
    std::function<bool(std::string_view, const nlohmann::json &)> include_feature;
    // Kept separate from feature loading so embedders may apply distinct
    // allowlists.  Registration normally supplies the same PathResolver-backed
    // loader to both seams.
    RenderPipelinePresetLoader load_pipeline_json;
    // Runs after preset expansion and before feature parsing. The production
    // renderer uses this pure seam for an optional renderer-wide strategy;
    // the returned config is still compiled by every normal validation stage.
    std::function<nlohmann::json(const nlohmann::json &)>
        transform_resolved_config;
    // Engine/runtime policy is supplied explicitly so the same feature
    // availability decision can be used by authoring catalogs and the real
    // compile/apply path.  Pure project-layer callers may leave it empty.
    std::function<void(std::string_view, const nlohmann::json &)>
        validate_feature;
    // Authoring-only observer of concrete declarations at the actual binding
    // position. Runtime compilation leaves this empty.
    std::function<void(std::string_view, const nlohmann::json &)>
        materialized_feature;
    // Reports only integration fragments whose feature/pass requirements match.
    std::function<void(std::string_view, const nlohmann::json &)>
        applied_integration;
    std::set<std::string> inactive_graph_scopes;
    // LP-Vb: sizes a graph.loop over view_axis (RenderPipelineResolveDependencies).
    RenderGraphViewAxisLengthResolver view_axis_length;
};

struct RenderFeatureComposeResult {
    nlohmann::json config;
    std::vector<std::string> shader_defines;
    std::vector<std::string> feature_names;
    std::vector<std::string> excluded_feature_names;
    std::optional<nlohmann::json> projection_jitter;
    nlohmann::json feature_instances = nlohmann::json::array();
    nlohmann::json surface_resource_contracts =
        nlohmann::json::array();
    nlohmann::json material_routing;
    nlohmann::json draw_sort;
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    std::vector<RenderPassProvenance> pass_provenance;
    std::vector<RenderResourceProvenance> resource_provenance;
    // LP-1a: every graph.loop of a pelican.graph root, as the resolution
    // stage expanded it (get_frame_plan's `loops[]`).
    std::vector<RenderGraphLoopReport> graph_loops;
    // LP-3a: the draw-step rules the graph's draw_loops give their material
    // passes (null when there is none).
    std::shared_ptr<const MaterialDrawLoopRules> material_draw_loops;
    // LP-0d: the graph nodes this composition made inactive because they
    // depend on a feature include_feature left out (a graph variant's).
    std::vector<RenderGraphDeactivatedNode> deactivated_graph_nodes;
    // FG-1: the root's fold groups (their digest and state), and what the
    // verified ones added to the config's target_planning.
    std::vector<GraphFoldGroupReport> fold_groups;
    std::vector<GraphFoldPlanningEntry> fold_planning;
    bool used_features = false;
};

RenderFeatureComposeResult composeRenderFeatureConfig(
    const nlohmann::json &config,
    const RenderFeatureComposeDependencies &dependencies = {});

// Resolve a feature's parameters into editable declarations. The config must
// contain the resources visible at this feature's composition position.
// The returned feature can be serialized and loaded by the normal composer;
// it retains provider parameters but no longer accepts parameter overrides.
nlohmann::json materializeRenderFeatureDocument(
    const nlohmann::json &feature,
    const nlohmann::json &instance_parameters,
    const nlohmann::json &config);

inline constexpr std::string_view
    renderFeatureRuntimeCompilerRequiredMessage =
        "render feature には実行時コンパイラが必要です (runtime shader compiler is required)";

bool renderFeatureRequiresRuntimeShaderCompiler(
    const nlohmann::json &feature,
    std::string_view feature_name);
std::vector<std::string> renderFeatureRequiredCapabilities(
    const nlohmann::json &feature,
    std::string_view feature_name);

// G-6: whether `before:<name>` / `after:<name>` names one of the canonical
// color pipeline's anchors. Composition resolves such an insert to the anchor
// before any pass that happens to share the name.
bool isCanonicalRenderPassAnchor(std::string_view name);
// CT-4: every name isCanonicalRenderPassAnchor takes, in pipeline order.
std::vector<std::string> canonicalRenderPassAnchors();

// The pass an insert names, in the pass family that holds it. A canonical
// anchor is found (in every family) before any pass that shares its name;
// only when no canonical anchor matches is a pass of that name looked for.
struct RenderPassAnchorMatch {
    nlohmann::json *passes = nullptr; // the family's `passes` array
    std::size_t index = 0;
    std::string family;
};
std::vector<RenderPassAnchorMatch> findRenderPassAnchorMatches(
    nlohmann::json &config, const std::string &anchor);

// LP-0b: the one placement rule feature passes and graph passes share. Puts
// `run` next to passes[anchor_index] as one contiguous run, in the given
// order. After a canonical anchor the run goes to the end of that anchor's
// bucket. The pass next to the anchor is ordered against it (`after` for the
// first, `before` for the last); the canonical order composition settles
// afterwards chains the rest in declaration order. A feature inserts one pass
// at a time, so several feature passes after the same non-canonical pass end
// up in reverse; a run keeps its order.
void insertRenderPassRunAtAnchor(nlohmann::json &passes,
                                 std::size_t anchor_index, bool after,
                                 std::vector<nlohmann::json> run);

// FR-1: the render targets a feature document's own declaration reads or
// writes and leaves to the pipeline to declare -- its passes' color / depth
// outputs and inputs and its render_target_overrides, a `$parameter` name by
// that parameter's default -- as [{name, type, usage?}], in the order they are
// first named. A target the feature declares itself, `swapchain`, `display`,
// an integration's fragment and a parameter with no default are left out; an
// `@history` read names its target. `type` is the logical image type a
// render.target_ref naming it declares: the one the feature's
// `required_targets` entry of that name gives, else pelican.render.depth@1
// (device, projection) for a depth attachment, else
// pelican.render.legacy_opaque_image@1. `usage` is what the feature uses it
// for (COLOR_ATTACHMENT / DEPTH_STENCIL_ATTACHMENT / SAMPLED, and the override's
// and the parameter's usage). Never throws on a malformed document; the
// composer refuses that.
// FR-3: `with_needs_writer` adds `needs_writer` to each entry: true when a
// pass of the feature reads the target this frame (an input that is not an
// `@history` read) and none of its passes writes it (a color or depth
// output) -- a target the pipeline has to have drawn by someone else
// (feature.required_target_unwritten@1 otherwise).
nlohmann::json renderFeatureRequiredTargets(const nlohmann::json &feature, bool with_needs_writer = false);

// FR-3: a feature reads a target (renderFeatureRequiredTargets'
// needs_writer) that no pass or compute task of the composed pipeline
// writes: no declaration names it except as something it reads, and it has
// no initial contents. Fields: {feature, target, type, writer_candidates:
// [{node?, pass, port, resource}], document} -- `type` as
// renderFeatureRequiredTargets gives it, and `writer_candidates` the outputs
// that, in the family of the feature's pass that reads it and before that
// pass, write a target of the same kind (depth for a pelican.render.depth@1
// type, color otherwise; judged by the target's format) -- a pass that draws
// another view (`view_family`, a shadow map's) and a feature's own pass are
// not among them: `pass` the frame
// pass, `resource` the target it writes now, `port` the graph port that
// writes it when a root node of the rendering graph made the pass (`node`
// is then that node's id), else "depth" or "color" (with `.<index>` past the
// first color output). Writing the required target from one of them (for a
// graph node: connecting that port to a render.target_ref of the target)
// lets the feature compose. Raised as GraphRegistryError.
inline constexpr std::string_view featureRequiredTargetUnwrittenCode =
    "feature.required_target_unwritten@1";

// FR-1: a feature composed into a pipeline that declares none of a target it
// needs. Fields: {feature, target, type, document} -- the feature's name, the
// target, its renderFeatureRequiredTargets type, and the feature document's
// reference. Raised as GraphRegistryError.
inline constexpr std::string_view featureRequiredTargetMissingCode =
    "feature.required_target_missing@1";

// FR-2: where a feature document's passes go: one {pass, anchor, position}
// per passes entry inserted `after:<anchor>` or `before:<anchor>` (position
// "after" / "before"), in document order. `begin` / `end` entries are left
// out. Never throws on a malformed document; the composer refuses that.
nlohmann::json renderFeatureInsertions(const nlohmann::json &feature);

// FR-2: a feature pass inserted after: / before: a name that no anchor and no
// pass of the pipeline carries. Fields: {feature, anchor, candidates,
// document} -- the feature's name (an integration's as provider/integration),
// the name it pointed at, the names after: / before: resolves to exactly one
// frame node in the pipeline as composed so far (an anchor by its anchor name,
// a pass by its name, in pipeline order), and the feature document's
// reference. Raised as GraphRegistryError.
inline constexpr std::string_view featureInsertionAnchorMissingCode =
    "feature.insertion_anchor_missing@1";

// FR-1: a rendering.snapshots entry whose `after` names no pass or anchor in
// some pass family. Fields: {snapshot, after, resolved?, candidates} --
// `resolved` is the frame node an anchor name stands for
// (`__anchor_<anchor>`), `candidates` renderSnapshotAfterCandidates of the
// pipeline. Raised as GraphRegistryError.
inline constexpr std::string_view snapshotAfterUnknownCode =
    "snapshot.after_unknown@1";

// FR-1: the names a rendering.snapshots entry's `after` may take in a composed
// (or canonicalized) config: in every pass family, the frame nodes from the
// post_ldr anchor up to the pelican_ui anchor -- an anchor by its anchor name
// (`post_ldr`), a pass by its name -- that every family has, in the first
// family's order. Snapshot copies are not among them.
std::vector<std::string> renderSnapshotAfterCandidates(const nlohmann::json &config);

} // namespace Pelican
