#pragma once

#include "frameplanmodel.hpp"
#include "inspectormodel.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {

// One input port of one node as the canvas draws it. Where the port list came
// from is part of what is shown: a declared contract and a key read off the
// authored node are not the same claim, and the canvas must not pretend the
// second is the first.
struct GraphCanvasPort {
    std::string port;
    // The semantic type id the catalog says flows through this port, e.g.
    // "pelican.value.scalar@1". Empty when nothing declared one -- which the
    // canvas draws as the unknown socket rather than guessing a type.
    std::string type;
    // The operation catalog declared this port; otherwise it was read from the
    // authored document (the node's own inputs/parameters, or, for an
    // instance, the definition's graph.input nodes).
    bool declared = false;
    bool required = false;
    std::string accepted_type;
    // The authored edge into this port, when there is one.
    bool connected = false;
    std::string source_node;
    std::string source_port;
    // LP-4c: an edge that reads one element of a loop's `p[]` names the
    // element; this is that `element` as the document holds it (dumped), or
    // empty for an ordinary edge.
    std::string source_element;
    // The authored scalar stored on the node for this port.
    bool has_parameter = false;
    nlohmann::json parameter;
    // What the published generation actually fed the port. Only root-scope
    // graph.instance nodes can carry this: get_graph_values answers for an
    // instance, never for a node inside a definition (AG-3).
    bool has_value = false;
    double value = 0;
    // "connected" | "parameter" | "default" | "override"
    std::string value_source;
    std::string value_connected_node;
    std::string value_connected_port;
};

// One output port as the canvas draws it: the name the document connects to
// and, when the catalog declared it, the type that leaves it.
struct GraphCanvasOutput {
    std::string port;
    std::string type;
    // What the published generation's evaluation left on this port, read from
    // get_graph_node_values. Unlike the input side, this reaches every scope:
    // the node values answer for a definition body as well as for the root.
    bool has_value = false;
    double value = 0;
    // LP-4c: a loop's `p[]` -- one value per element, not one value. `type`
    // is then the type of each element.
    bool bundle = false;

    bool operator==(const GraphCanvasOutput &) const = default;
};

// Q-19. One input socket of a render node (render.fullscreen, render.raster)
// whose pass's shader declared its inputs -- get_frame_plan's pass_inputs on
// the pass the node made -- drawn the way the Frame Plan draws it (VI-3a):
// one socket per fixed declaration, named by its variable, and the variadic
// group as a band of its connected members over an empty drop target. What
// connects there is a numbered port of the node (`"binding": "input"`,
// `"input_index": n`, LP-1d), and binding n is input n.
struct GraphCanvasInputSocket {
    enum class Kind : std::uint8_t {
        // One fixed declaration: one socket, connected or empty.
        declared,
        // One connected member of the variadic group, inside the band.
        variadic,
        // The empty drop target at the lower end of the variadic band.
        variadic_drop,
        // A numbered port no declaration takes. Drawn so its line has an end.
        undeclared,
    };
    Kind kind = Kind::declared;
    // The declaration's binding, a member's, or -- for the drop target --
    // the one the next input takes.
    std::size_t binding = 0;
    // The declaration's variable name ("" for variadic members and extras).
    std::string declaration;
    // A buffer declaration: a graph pass reads only images by number.
    bool buffer = false;
    // The numbered port connected here and what feeds it; "" when empty.
    std::string port;
    std::string source_node;
    std::string source_port;
    std::string label;

    [[nodiscard]] bool connected() const noexcept { return !port.empty(); }
    // What the socket is named by: "in@<binding>", "in:variadic", or the
    // port's own name for an extra.
    [[nodiscard]] std::string key() const;

    bool operator==(const GraphCanvasInputSocket &) const = default;
};

struct GraphCanvasNode {
    std::string id;
    std::string op;
    // The authored, human-chosen name for this node. Any node may carry a
    // `label`; when it does not, the heading falls back to the id.
    std::string label;
    // From the catalog. Empty when the catalog has not arrived or does not
    // know the operation -- which is a visible state, not a default.
    std::string domain;
    std::string evaluation;
    bool sink = false;
    bool contract_known = false;
    // Set for graph.instance nodes; names the definition scope to descend to.
    // For a graph.loop it is the body a descent lands in (the branch chosen
    // for it, else the first branch that has a body).
    std::string definition;
    bool instance = false;
    // LP-4c: a graph.loop. Its input ports are its first body's graph.input
    // nodes and its outputs the `p` / `p[]` it declares; its parameters are
    // the Loop fields, never ports.
    bool loop = false;
    // What the heading shows beside the name for a loop: the mark and the
    // tier with its count when the report has them ("expand x3"), or "loop".
    std::string badge;
    // LP-4c, decision 9: the last refused edit named this node.
    bool refused = false;
    std::string refusal;
    bool has_owner = false;
    std::string owner_scene;
    std::uint64_t owner_object = 0;
    nlohmann::json parameters = nlohmann::json::object();
    std::vector<GraphCanvasPort> inputs;
    std::vector<GraphCanvasOutput> outputs;
    // Q-19: set when the node's pass published its shader's declarations.
    // The sockets are then these, and the numbered ports are not among
    // `inputs`; without them the node keeps its rows.
    bool declared_sockets = false;
    std::vector<GraphCanvasInputSocket> input_sockets;

    // What the heading shows in bold. The id is the fallback, not a second
    // label: a node without one is named by its id.
    [[nodiscard]] const std::string &heading() const noexcept {
        return label.empty() ? id : label;
    }
};

struct GraphCanvasEdge {
    std::string from_node;
    std::string from_port;
    std::string to_node;
    std::string to_port;
    // LP-4d2, decision 4: an edge that reads one element of a loop's `p[]`
    // says which, as the label on its line shows it ("element", "2");
    // empty for an ordinary edge.
    std::string element = {};
};

// LP-4d1: one output of a definition body -- its name and the port inside
// that makes it -- which a loop zone's right edge draws.
struct GraphCanvasBodyOutput {
    std::string name;
    std::string node;
    std::string port;
};

// A scope is either the root document or one definition body. The root scope
// carries the empty id so a definition named "root" stays addressable.
struct GraphCanvasScope {
    std::string id;
    std::string label;
    bool root = false;
};

// Q-8. One node of the project's UI document, named the way a `ui.value` or
// `ui.visible` sink names it: the ids from the root joined by "/" (the same
// string the UI's emit calls stable_id). `type` is the widget type, which is
// what decides whether a value can be written to it at all.
struct GraphCanvasUiNode {
    std::string path;
    std::string type;

    bool operator==(const GraphCanvasUiNode &) const = default;
};

// Every node of one UI document, in document order. A node without a string
// id has no path and is skipped, with its children.
std::vector<GraphCanvasUiNode> graphCanvasUiDocumentNodes(const nlohmann::json &document);

struct GraphCanvasUiNodesRead {
    std::vector<GraphCanvasUiNode> nodes;
    // Why there are none, when there are none for a reason worth saying:
    // project.json names no UI document, or the one it names cannot be read.
    std::string error;
};

// The UI document project.json's basic_config.ui_config_json names, read the
// way the engine reads it (through the project path resolver), and walked.
// No engine RPC answers with the UI tree, and the document is the source of
// truth for the paths anyway: an unknown path is refused by the engine by name.
GraphCanvasUiNodesRead readProjectUiNodes(const std::filesystem::path &project_root);

// Q-10. The sounds a project holds, as the `asset` of an audio.playback node
// names them ("project://sounds/hit.wav"). No engine RPC lists them -- they
// are files the node names directly, not asset_data entries -- so the Studio
// walks the project for the two extensions AU-2 plays (.wav and .ogg, in any
// case), skipping dot directories (.pelican and the like). Sorted.
struct GraphCanvasAudioAssetsRead {
    std::vector<std::string> assets;
    // Why there are none, when there are none for a reason worth saying.
    std::string error;
};
GraphCanvasAudioAssetsRead readProjectAudioAssets(const std::filesystem::path &project_root);

// One operation the "Add node..." palette can offer in the current scope.
// `structural` marks graph.input and graph.instance: the catalog does not
// declare their ports, and each belongs to one side of the scope boundary.
struct GraphCanvasCatalogEntry {
    std::string op;
    std::string domain;
    std::string evaluation;
    bool sink = false;
    bool structural = false;

    bool operator==(const GraphCanvasCatalogEntry &) const = default;
};

// LP-4e. A material's tags as a draw_loop's `where` and a branch's `when`
// test them ({"tags": {include, exclude, require_all}}); empty lists are left
// out of the document.
struct GraphCanvasTagFilter {
    std::vector<std::string> include;
    std::vector<std::string> exclude;
    std::vector<std::string> require_all;

    [[nodiscard]] bool empty() const noexcept {
        return include.empty() && exclude.empty() && require_all.empty();
    }
    bool operator==(const GraphCanvasTagFilter &) const = default;
};

// LP-4e. A draw_loop branch's condition: any of the material's attributes,
// all of them to hold (AND). An empty condition is the else.
struct GraphCanvasDrawWhen {
    // The material's matching key (`<document>#<name>`, `<asset>#material[i]`).
    std::optional<std::string> material;
    std::optional<GraphCanvasTagFilter> tags;
    // The material's shader contract: gbuffer_v1, forward_scene_color_v1 or
    // legacy_gbuffer_v1.
    std::optional<std::string> contract;
    std::optional<bool> skinned;
    std::optional<bool> has_variant;
    // UX-3, decision 5: has_variant naming one variant (EL-1's variants).
    std::optional<std::string> has_variant_named;

    [[nodiscard]] bool empty() const noexcept {
        return !material && !tags && !contract && !skinned && !has_variant &&
               !has_variant_named;
    }
    bool operator==(const GraphCanvasDrawWhen &) const = default;
};

// LP-4c. One branch of a graph.loop as the branch table shows it.
struct GraphCanvasLoopBranch {
    // "else" (no `when`), "element", "element_in", "element_not_in", or
    // "other" for a condition the table has no form for (an expression).
    // LP-4e: "draw" for a draw_loop's condition, read into `draw_when`.
    std::string when = "else";
    std::vector<std::int64_t> values;
    GraphCanvasDrawWhen draw_when;
    // The definition the branch runs; empty with `nothing` for `"body": null`.
    std::string body;
    bool nothing = false;
    // From get_frame_plan's loops[]: the elements that took this branch, and
    // whether none did (`unmatched_branches`). Empty without a report.
    std::vector<std::int64_t> matched;
    // LP-4e: a draw_loop's elements are materials, by their matching key.
    std::vector<std::string> matched_materials;
    bool unmatched = false;
};

struct GraphCanvasLoopCarry {
    std::string from;
    std::string to;
    // The carried input has no loop input edge, which the engine refuses:
    // its first element would have nothing to start from.
    bool to_unbound = false;
};

// LP-4c. What the Loop fields show for one root graph.loop: its parameters
// read into the fields' own terms, its body's boundary, and the report.
struct GraphCanvasLoop {
    std::string id;
    // over.range.count: an integer, or an expression ({"$expr": ...}).
    bool count_expression = false;
    std::int64_t count = 0;
    std::string count_text;
    bool descending = false;
    std::optional<std::int64_t> limit;
    // "" (automatic), "expand" or "draw_loop".
    std::string tier;
    // "" (none), "element_in" or "element_not_in".
    std::string where;
    std::vector<std::int64_t> where_values;
    // Where the copies land among the passes (LP-0b): a canonical anchor's
    // name, or "" for none. `insert` names a pass instead, shown as it is.
    std::string anchor;
    std::string insert;
    // A body with a fullscreen or raster pass: only such a loop takes a place
    // (graph.insert_on_compute@1 otherwise).
    bool body_draws = false;
    std::vector<GraphCanvasLoopBranch> branches;
    std::vector<GraphCanvasLoopCarry> carry;
    // The loop node's own `outputs`: which `p` and `p[]` it declares.
    std::vector<std::string> outputs;
    // The boundary every branch shares, read off the first branch that has a
    // body (its graph.input nodes, its outputs).
    std::string body;
    std::vector<std::string> body_inputs;
    std::vector<std::string> body_outputs;
    // get_frame_plan's loops[] entry for this loop, when one arrived.
    bool reported = false;
    std::string reported_tier;
    std::int64_t reported_count = 0;

    // LP-4e: a loop over the draws of one material pass (over.draw_items):
    // one element per material, drawn inside that pass (draw_loop). Its
    // contract is the pass's, read only; its `where` tests tags; it takes no
    // count, order, limit or carry.
    bool draw_items = false;
    std::string contract;
    GraphCanvasTagFilter where_tags;
    // The render.material_pass whose `draws` this loop's output feeds, from
    // the document (or the report's `pass`).
    std::string pass;
    // Every material the report names, all pages read (the branch table's
    // `material` choices), and how many the report says there are.
    std::vector<std::string> materials;
    std::size_t reported_total = 0;

    // LP-4f1: a loop over a view axis (over.view_axis): the family and the
    // axis, and the axis as the frame plan lists it on a pass of that family
    // (its length and labels), when one does.
    std::string view_family;
    std::string view_axis;
    std::optional<FramePlanViewAxis> view_axis_info;
    // How many elements the loop is known to run over before a report: an
    // integer count, or the view axis's length. Empty for an expression, a
    // draw_loop, or an axis the frame plan does not list.
    std::optional<std::int64_t> known_count;
};

// LP-4f1: one view family a pass of the frame plan runs in, with the axes
// get_frame_plan lists on it (LP-Va2's view_axes, the same on every node of
// the family). Only families with axes are kept.
struct GraphCanvasViewFamily {
    std::string family;
    std::vector<FramePlanViewAxis> axes;

    bool operator==(const GraphCanvasViewFamily &) const = default;
};

// LP-4f1, decision 2: what a render node's View field shows. `axes` are the
// axes of `family`; `view_select` the node's integer choices; a node in the
// body of a loop over a view axis also gets `element_axis` -- the axis whose
// row offers "Each element (loop)" -- and `element_chosen` when its
// view_select already says `{"$expr": "element"}` there. `writes_family` is
// set when the node does not name `family` itself: a choice then writes it
// too, since a pass that names none runs in the first family of the frame.
struct GraphCanvasViewField {
    std::string family;
    std::vector<FramePlanViewAxis> axes;
    std::vector<FramePlanViewSelectAxis> view_select;
    std::string loop;
    std::string element_axis;
    bool element_chosen = false;
    bool writes_family = false;

    bool operator==(const GraphCanvasViewField &) const = default;
};

// UX-2: one argument of a semantic type, as the catalog's type schema
// declares it -- the values it takes (`parameters[].allowed`, or EL-1's
// `arguments.<name>.values` where the catalog carries it), and the one it has
// when it is left out (empty when it has to be given). `allowed` is empty
// for an argument with no list: its field shows the document's value.
struct GraphCanvasTypeArgument {
    std::string name;
    std::vector<std::string> allowed;
    std::string default_value;

    bool operator==(const GraphCanvasTypeArgument &) const = default;
};

// UX-2: a semantic type a render.image can carry (constructor `image` in the
// catalog), with a name a person reads ("Color signal") and its arguments.
// `depth` is the schema's depth_like trait; `legacy` its legacy_untyped one.
struct GraphCanvasImageType {
    std::string semantic;
    std::string label;
    bool depth = false;
    bool legacy = false;
    std::vector<GraphCanvasTypeArgument> arguments;

    bool operator==(const GraphCanvasImageType &) const = default;
};

// UX-2, decision 4: one value a pill offers instead of a line of text. The
// node's whole parameters object with the choice made, so one pick is one
// set_node_parameters (a semantic that changes the format too travels as one).
struct GraphCanvasValueChoice {
    std::string label;
    nlohmann::json parameters;
};

// UX-5b, decision 1: one shader of get_graph_catalog.engine_shaders (EL-4).
// `ports_by_hand`: the engine could not reflect it on its own because it
// includes the resource-port header (bloom_composite, ssao): its ports come
// from the pass that runs it, so the author connects them. `resource_ports`
// are "<port> (<descriptor type>)" as the reflection named them.
struct GraphCanvasEngineShader {
    std::string reference;
    std::string stage;
    bool ports_by_hand = false;
    std::vector<std::string> resource_ports;

    bool operator==(const GraphCanvasEngineShader &) const = default;
};

// UX-5d, decision 2: a ray tracing pipeline get_engine_shaders lists, which a
// new render.compute can trace with (`ray_tracing {raygen, miss,
// closesthit}`), and its resource ports: {port, descriptor type} --
// sampled_image, storage_image, storage_buffer...
struct GraphCanvasRayTracingPipeline {
    std::string reference;
    std::string raygen;
    std::string miss;
    std::string closesthit;
    std::vector<std::pair<std::string, std::string>> ports;
    // EL-4c: a storage image port's format, when the engine read it from the
    // shader (rt_shadow_mask_pipeline's shadow_mask: R8_UNORM).
    std::map<std::string, std::string> formats;

    bool operator==(const GraphCanvasRayTracingPipeline &) const = default;
};

// EL-5s: one resource port a project shader declares, as the engine's
// get_shader_interface (EL-5) reflected it: its name, its descriptor type
// (sampled_image, storage_image, storage_buffer...) and, for a storage
// image whose layout names one, its format.
struct GraphCanvasShaderPort {
    std::string port;
    std::string type;
    std::string format;

    bool operator==(const GraphCanvasShaderPort &) const = default;
};

// EL-5s: what get_shader_interface answered for one shader stem. `status`
// is the engine's: "building" (asked again half a second later), "ready" or
// "failed" (`error`, e.g. shader_not_found). A ready stem is a compute
// shader (`compute`, its `local_size`) or a ray tracing set (raygen, miss
// and closesthit). `ports` are its stages' resource ports, each once, in
// the order the stages list them. `reflection_error`: a stage the engine
// could not compile on its own (its message names the file by project://).
struct GraphCanvasShaderInterface {
    std::string reference;
    std::string status;
    std::string error;
    bool compute = false;
    bool ray_tracing = false;
    std::vector<GraphCanvasShaderPort> ports;
    std::array<std::int64_t, 3> local_size{0, 0, 0};
    std::string reflection_error;

    [[nodiscard]] bool ready() const noexcept { return status == "ready"; }
    bool operator==(const GraphCanvasShaderInterface &) const = default;
};

// EL-5s, decision 4: a compute node whose shader file changed its ports
// since the canvas last read them -- the names added and removed. "Update
// ports" makes the node's ports the shader's again (decision 2).
struct GraphCanvasPortChange {
    std::string scope;
    std::string node;
    std::string shader;
    std::vector<std::string> added;
    std::vector<std::string> removed;

    bool operator==(const GraphCanvasPortChange &) const = default;
};

// UX-5b, decision 4: a render.image a root render.material_pass draws into
// at one of its contract's sockets, and the formats the contract takes there
// (get_graph_catalog.material_pass_contracts).
struct GraphCanvasMaterialPassImage {
    std::string pass;
    std::string contract;
    std::string socket;
    std::vector<std::string> formats;
};

// UX-5b, decision 2: an image a fullscreen template can read, and the end a
// line to it starts from -- the pass that writes it (GL-1f), else the image.
struct GraphCanvasTemplateSource {
    std::string resource;
    std::string node;
    std::string port;

    bool operator==(const GraphCanvasTemplateSource &) const = default;
};

// UX-3, decision 1: a render target declared outside the graph (the rendering
// section, the preset, a feature), as get_render_authoring_context lists it,
// and the image type a render.target_ref names it with: the declaration's own
// `type` when it has one, else its kind (a depth format or not) and topology
// (cube, layers) in a logical type -- materialpassdecompose's targetType.
struct GraphCanvasDeclaredTarget {
    std::string name;
    std::string source;
    nlohmann::json declaration;
    nlohmann::json type;
    bool depth = false;

    bool operator==(const GraphCanvasDeclaredTarget &) const = default;
};

// UX-3, decision 2: where a render node lands among the passes. `kind` is
// "automatic" (no insert, no canonical_anchor), "before" / "after" (an
// insert naming `target`, a pass), "anchor" (canonical_anchor `target`) or
// "other" (an insert the field has no form for, shown as it is). `declared`
// is an insert's `declared: true`, kept while the pass stays where it is.
struct GraphCanvasPlace {
    std::string kind = "automatic";
    std::string target;
    bool declared = false;
    // The family it runs in; empty: the frame's first.
    std::string pass_set;

    bool operator==(const GraphCanvasPlace &) const = default;
};

// UX-3, decision 3: what one port of a render node's `ports` says about how
// it is read or written, and which of it the engine lets the port say: a
// previous frame only on an image read that is no attachment, sampling on a
// sampled image read, a subresource on an image that is no attachment (a
// numbered input too since UX-7: engine RS-14), a location on a colour
// attachment.
struct GraphCanvasPortSettings {
    bool history = false;
    // Empty: the engine's default (linear, repeat).
    std::string filter;
    std::string address;
    std::int64_t mip = 0;
    std::int64_t layer = 0;
    std::int64_t location = 0;
    bool history_allowed = false;
    bool sampling_allowed = false;
    bool subresource_allowed = false;
    bool location_allowed = false;

    bool operator==(const GraphCanvasPortSettings &) const = default;
};

// UX-3, decision 5 (engine EL-1): one material the running engine has, as
// get_render_authoring_context's materials[] lists it. `id` is what a
// draw_loop branch's when.material compares; `name` what a person reads.
struct GraphCanvasMaterial {
    std::string id;
    std::string name;
    std::vector<std::string> tags;
    std::vector<std::string> variants;
    bool builtin = false;

    bool operator==(const GraphCanvasMaterial &) const = default;
};

// LP-4c, decision 9: where a refused edit points. A loop refused by name
// marks the loop; a node of a loop's copy (`loop_origin`) marks the loop and
// the body node in its own scope.
struct GraphCanvasRefusalMark {
    std::string scope;
    std::string node;
    std::string reason;

    bool operator==(const GraphCanvasRefusalMark &) const = default;
};

// UX-12a, decision 4: graph.render_node_parameter_invalid@1 (engine CT-3) --
// which node's which parameter the engine refused, and the sentence it reads
// as. `scope` is the scope the refused edit was made in; `parameter` is the
// engine's path ("size", "size_from_extent.tile_width", "keys[0].time").
struct GraphCanvasParameterRefusal {
    std::string scope;
    std::string node;
    std::string parameter;
    std::string sentence;

    bool operator==(const GraphCanvasParameterRefusal &) const = default;
};

// UX-13b, decision 2 (engine CT-5): a buffer a render.scalar can write, and
// what the edit that picks it changes on it so that it can be written --
// "runtime_f32" (its host_source, and its size with it), "<n> bytes" (its
// size alone), or nothing.
struct GraphCanvasScalarTarget {
    std::string buffer;
    std::string fix;

    bool operator==(const GraphCanvasScalarTarget &) const = default;
};

// UX-13b, decision 3: graph.render_scalar_target_incompatible@1 -- the buffer
// a render.scalar cannot write and what it has to be. `in_graph`: a buffer of
// the graph (the refusal names a node), which one edit makes so, in `scope`;
// else one declared outside the graph, which only a sentence can point at.
struct GraphCanvasScalarTargetFix {
    std::string scope;
    std::string target;
    std::optional<std::uint64_t> size;
    bool in_graph = false;

    bool operator==(const GraphCanvasScalarTargetFix &) const = default;
};

// GL-2b (design_graph_layers 5.1). Where the graph node that made a pass is
// authored: get_frame_plan's `origin`. `definition` and `instance_path` are
// empty for a root node; `loop` is set for one copy of a graph.loop.
struct GraphCanvasPlanOrigin {
    std::string graph;
    std::string node;
    std::string definition;
    std::string instance_path;
    std::string loop;
    std::optional<std::size_t> loop_branch;
    std::optional<std::int64_t> element;

    bool operator==(const GraphCanvasPlanOrigin &) const = default;
};

// GL-2b: one feature pass a graph node's pass took the place of (`replaces`).
struct GraphCanvasPlanReplace {
    std::string feature;
    std::string pass;

    bool operator==(const GraphCanvasPlanReplace &) const = default;
};

// GL-1 (design_graph_layers 1, 3.3, 3.4). One node of get_frame_plan, as the
// Graph tab reads it to draw what no graph node made: the rendering
// section's passes, the features' and the preset's.
struct GraphCanvasPlanPass {
    std::string name;
    std::string authored_name;
    // The plan's kind: render, compute, anchor, snapshot_copy, ...
    std::string kind;
    // "project", "feature:<name>" or "engine".
    std::string source;
    // provider_feature: the feature reference that made it, or empty.
    std::string feature;
    std::vector<std::string> reads;
    std::vector<std::string> history_reads;
    std::vector<std::string> writes;
    std::size_t order = 0;
    // LP-1a: the graph.loop whose copy it is (loop_origin.loop), or empty.
    std::string loop;
    // And loop_origin's element and body node.
    std::int64_t loop_element = 0;
    std::string loop_node;
    // UX-5c: the loop outputs it names as `<loop>.<output>` (RS-13), and
    // what each read this frame.
    std::vector<FramePlanLoopOutput> loop_outputs;
    // GL-2b: the graph node that made it; none for a pass of the preset, a
    // feature or the rendering section (and for any pass of an engine that
    // predates GL-2a).
    std::optional<GraphCanvasPlanOrigin> origin;
    std::vector<GraphCanvasPlanReplace> replaces;
    // The endpoint the execution plan selected for it, when it says.
    std::string backend;
    // A material pass: its shader is the materials' (shader_resolution
    // material_owned), or it carries a material filter or variant.
    bool material = false;
    std::string material_variant;
    // The contract material_routing names for it, when it names one.
    std::string contract;
    // material_filter.resolved_draw_count, when the engine resolved it.
    std::optional<std::size_t> draw_count;
    // UX-11, decision 4: the material_routing routes that name it (the
    // Frame Plan's Materials rows), and the tags its material_filter takes
    // and leaves out, as the plan says.
    std::vector<std::string> routes;
    std::vector<std::string> material_include;
    std::vector<std::string> material_exclude;
    // LC-1: its shader reads pelican_loop_element() / _count()
    // (get_frame_plan's reads_loop_index).
    bool reads_loop_index = false;
    // PS-1s (engine PS-1): `schedule: "once"`, and when it last ran -- the
    // runtime generation and the logical frame; empty before it has.
    bool once = false;
    std::optional<std::uint64_t> last_run_generation;
    std::optional<std::uint64_t> last_run_frame;
    // UX-12c (engine MP-1): the materials it drew in the last published
    // frame; none before one was, or from an engine that predates MP-1.
    std::optional<FramePlanDrawnMaterials> drawn_materials;

    bool operator==(const GraphCanvasPlanPass &) const = default;
};

// PS-1s, decision 3: what the plan says of a once node's passes (all of a
// loop body node's copies): the latest generation and frame any of them ran
// in, empty while none has run.
struct GraphCanvasOnceRun {
    std::optional<std::uint64_t> generation;
    std::optional<std::uint64_t> frame;
    // How many of the plan's passes the node made run once.
    std::size_t passes = 0;

    bool operator==(const GraphCanvasOnceRun &) const = default;
};

// LC-1s, decision 2: whether a pass node's shader reads the loop element, as
// the frame plan says of the passes the node made. `outside_loop`: it reads
// it, but no loop copied the node, so it reads element 0.
enum class GraphCanvasLoopElementRead { none, in_loop, outside_loop };

// GL-1, decision 1. A read-only node on the Graph canvas standing for passes
// no graph node made: every pass of one feature folded into one node, or one
// pass of the rendering section (or the engine's preset) on its own.
struct GraphCanvasFoldedNode {
    // The pass's name, or "feature:<name>" for a feature's fold.
    std::string id;
    std::string label;
    // What the heading shows where a graph node shows its operation:
    // "feature", or the pass's kind ("material" for a material pass).
    std::string type;
    // The plan's source ("project", "feature:<name>", "engine").
    std::string source;
    // The feature reference (a fold, or a pass inside a feature's fold).
    std::string feature;
    std::vector<std::string> passes;
    // What it reads from outside itself and what it hands on, by resource.
    std::vector<std::string> reads;
    std::vector<std::string> writes;
    bool feature_group = false;
    // One material pass: its inside is a render.material_pass and a draw_loop.
    bool material = false;
    // A feature with a shipped decomposed version (get_graph_templates).
    bool decomposable = false;
    // GL-1e, decision 1: a pass of the rendering section that takes lines.
    // The canvas fills these from the authoring context: the pass's input
    // sockets top to bottom, shown in place of its reads.
    struct InputSocket {
        std::string label;
        // The resource connected there, or "in:+" for the empty slot.
        std::string port;
        std::string resource;
        // FramePlanInputSocket::key(): "in@<binding>", "in:variadic".
        std::string key;
        std::optional<std::size_t> binding;
        // Where it is in the pass's `input`; none for the empty slot.
        std::optional<std::size_t> input_index;

        bool operator==(const InputSocket &) const = default;
    };
    bool takes_lines = false;
    std::vector<InputSocket> input_sockets;
    // GL-5a, decision 1: who owns it. A pass the root's rendering section
    // declares (F-6) is edited in the Graph's detail column; one of the
    // preset (`preset` its name, when the authoring context said) or of a
    // feature is read only here.
    bool rendering_section = false;
    std::string preset;

    [[nodiscard]] bool enterable() const noexcept { return feature_group || material; }
    // "From the rendering section: edit it here.", "From preset <name>: read
    // only here. Expand to edit…", "From feature <name>: ...".
    [[nodiscard]] std::string tooltip() const;
    bool operator==(const GraphCanvasFoldedNode &) const = default;
};

// GL-1, decision 2. A line the frame plan's reads and writes imply between
// two nodes of the canvas, one of them at least a folded node. Read only;
// several resources between the same two nodes are one line.
struct GraphCanvasDerivedEdge {
    std::string from;
    std::string to;
    std::vector<std::string> resources;

    bool operator==(const GraphCanvasDerivedEdge &) const = default;
};

// GL-1, decision 5. Where a line to a node the domain filter hides is cut:
// on the socket of the node that is shown, `← value (N)` on an input or
// `→ pose (N)` on an output. N counts the hidden nodes of that domain.
struct GraphCanvasDomainStub {
    std::string node;
    std::string port;
    bool input = true;
    std::string domain;
    std::size_t count = 0;

    [[nodiscard]] std::string text() const;
    bool operator==(const GraphCanvasDomainStub &) const = default;
};

// GL-1, decision 3. One step into a folded node: a feature's fold ("feature"),
// a material pass ("pass"), or a feature's decomposed version ("decomposed").
struct GraphCanvasFoldedStep {
    std::string kind;
    std::string id;
    std::string label;
    std::string feature;

    bool operator==(const GraphCanvasFoldedStep &) const = default;
};

// GL-1, decision 4: what "Expand to edit…" runs for the folded view on
// screen -- the Frame Plan's Decompose into loop for a material pass, the
// Features tab's Replace with loop version for a feature with a template.
struct GraphCanvasFoldedExpand {
    // "decompose", "replace", or empty (no link).
    std::string kind;
    std::string graph;
    std::string pass;
    std::string feature;
};

// GL-2b, decision 3 (design_graph_layers 3.2). One row of a node's Layers
// section: L1 a frame plan node it expanded to (or a feature pass it took the
// place of), L2 a resource, alias group, scope or barrier those nodes touch,
// L3 a decision or backend candidate about them.
struct GraphCanvasLayerRow {
    // "L1", "L2" or "L3".
    std::string layer;
    // "pass", "replaces", "replaced", "deactivated" (L1); "resource",
    // "alias", "scope", "barrier" (L2); "decision", "backend" (L3).
    std::string kind;
    std::string text;
    // The frame plan node the row stands for (an L1 pass row): a click on
    // it selects that node in the Frame Plan.
    std::string plan_node;

    bool operator==(const GraphCanvasLayerRow &) const = default;
};

// GL-2b, decision 4. Where the Graph tab goes for one frame plan node: a
// root node (a graph node, or a folded node), `node` in the body `scope` of
// the instance at `instance_path`, or `node` in the body of `loop`'s copy
// `element` (the zone's tab on the branch that element ran). `kind` is
// empty when the plan does not hold the node (or it is an anchor).
struct GraphCanvasPlanTarget {
    // "root", "instance", "loop" or empty.
    std::string kind;
    std::string node;
    std::string scope;
    std::string instance_path;
    std::string loop;
    std::int64_t element = 0;
    bool folded = false;

    bool operator==(const GraphCanvasPlanTarget &) const = default;
};

// GL-2b: get_frame_plan's diagnostics[] and deactivated_graph_nodes[].
struct GraphCanvasPlanDiagnostic {
    std::string kind;
    std::string subject;
    std::string detail;
    // UX-6b (PO-1): a detail with fields -- unread_output's and
    // unused_binding's resource, and unused_binding's binding.
    std::string resource;
    std::optional<std::size_t> binding;

    bool operator==(const GraphCanvasPlanDiagnostic &) const = default;
};

// UX-6b, decision 4: one thing the plan says a root node's passes do for
// nothing -- the band's words and the details' sentence.
struct GraphCanvasPlanNote {
    // "output unread" or "binding N unused".
    std::string band;
    std::string sentence;

    bool operator==(const GraphCanvasPlanNote &) const = default;
};

struct GraphCanvasDeactivatedNode {
    std::string node;
    std::string feature;
    std::string reason;

    bool operator==(const GraphCanvasDeactivatedNode &) const = default;
};

// GL-3a, decision 6: one of get_frame_plan's fold_decisions -- a decision a
// verified fold group added to target_planning, by its expanded name.
struct GraphCanvasFoldDecision {
    std::string group;
    // "node" or "resource".
    std::string kind;
    std::string name;
    // serial, isolate or no_alias.
    std::string field;

    bool operator==(const GraphCanvasFoldDecision &) const = default;
};

// One animation sink `get_status.animation` reported for the owner the watched
// instance names. Read-only: the Timeline draws it beside the value traces so
// "what the pose side is doing" and "what the values partition produced" are
// on one axis without either pretending to be the other.
struct GraphCanvasAnimationSink {
    struct Cursor {
        std::string clip;
        double time_seconds = 0;
        double normalized_phase = 0;
    };

    std::string node;
    // "pose_graph_clip" | "pose_graph_state_machine" | "component_graph" |
    // "component_clip".
    std::string source;
    std::string state;
    bool transition_active = false;
    std::string transition_target;
    double transition_progress = 0;
    std::vector<Cursor> cursors;
};

// One reading the Timeline kept. The buffer is the client's: AG-3a publishes a
// snapshot of the current generation and no history at all, so a strip over
// time can only be what this session watched go by.
struct GraphCanvasTimelineSample {
    // get_status.time -- scene time, which is the Timeline's x axis.
    double time = 0;
    // The published generation the values came from, so a sample taken while
    // the engine was paused is still distinguishable from a fresh one.
    std::uint64_t frame = 0;
    // One per port of timelinePorts(), in that order.
    std::vector<double> values;
    bool has_phase = false;
    double phase = 0;
    bool has_progress = false;
    double progress = 0;
};

// UX-8, decision 4: one key of a value.curve, as the engine reads it. What
// the table does not show (a cubic key's tangents) rides in `rest`.
struct GraphCanvasCurveKey {
    double time = 0;
    double value = 0;
    std::string interpolation = "linear";
    nlohmann::json rest = nlohmann::json::object();

    bool operator==(const GraphCanvasCurveKey &) const = default;
};

// Reads the common graph for a read-only node canvas. Qt-independent, in the
// same request/read two-slot shape as GraphAttachmentModel: the caller drains
// takeRpcRequests(), sends them over whatever transport it has, and feeds the
// answers back.
//
// It only ever asks for the three reads the canvas can justify:
// get_graph_instances, get_graph_catalog and get_graph_values.
class GraphCanvasModel {
    enum class RequestKind : std::uint8_t {
        Context,
        Catalog,
        Values,
        NodeValues,
        Status,
        Submit,
        Result,
        // LP-4c: get_frame_plan, for its loops[] only.
        Loops,
        // LP-4e: get_frame_plan {loop_elements}, one more page of a
        // draw_loop's materials.
        LoopPage,
        // GL-1: get_graph_templates, which features have a decomposed
        // version to show.
        Templates,
        // GL-1e: get_render_authoring_context, the rendering section's
        // passes as edit_preset_pass takes them.
        Authoring,
        // UX-5b: get_engine_shaders (EL-4b), asked only when a shader list
        // is opened -- the engine compiles its shaders to answer it.
        EngineShaders,
        // PS-1s, decision 4: rerun_once {node?}.
        RerunOnce,
    };
    struct PendingRequest {
        std::uint64_t id = 0;
        RequestKind kind = RequestKind::Context;
    };

    bool connected_ = false;
    std::uint64_t next_request_ = 1;
    std::vector<InspectorRpcRequest> requests_;
    // The document read and the reports run on separate slots: asking for
    // values never delays the next refresh, and a stale answer is dropped.
    std::optional<PendingRequest> pending_;
    // One outstanding read per kind. They are separate slots for the same
    // reason the document and the reports are: a Live tick asks two questions
    // at once, and neither may cancel the other's answer. Re-asking a kind
    // replaces its slot, which is how a stale reply is dropped.
    std::vector<PendingRequest> reads_;

    std::string digest_;
    // N-8: what the root rendering document actually is, and which file it is.
    // A legacy rendering root has nowhere to put a `layout`, and a panel that
    // keeps one has to be able to say that in those words -- naming the file --
    // rather than letting the user read a refused write as a bug.
    std::string root_schema_;
    std::string source_reference_;
    nlohmann::json nodes_ = nlohmann::json::array();
    nlohmann::json definitions_ = nlohmann::json::object();
    // UX-13b, decision 4 (engine CT-5): get_graph_instances'
    // definition_instances, as it came.
    nlohmann::json definition_instances_ = nlohmann::json::object();
    // N-6: the root scope's own placement, as the document carries it. A
    // definition's travels inside its body, so only this one needs a field.
    nlohmann::json layout_ = nlohmann::json::object();
    bool definitions_reported_ = false;
    bool context_received_ = false;

    std::vector<GraphCanvasUiNode> ui_nodes_;
    std::vector<std::string> audio_assets_;

    bool catalog_requested_ = false;
    bool catalog_received_ = false;
    // op -> catalog entry, flattened across modules.
    nlohmann::json operations_ = nlohmann::json::object();
    // semantic type id -> type schema, flattened the same way. The sockets are
    // drawn from these: the traits, and a module's own display declaration.
    nlohmann::json types_ = nlohmann::json::object();
    // UX-5b (EL-4): the catalog's engine_shaders and material_pass_contracts
    // as they arrived, and each engine shader's pass-input declarations in
    // the Frame Plan's terms, by reference.
    nlohmann::json engine_shaders_ = nlohmann::json::array();
    nlohmann::json material_pass_contracts_ = nlohmann::json::array();
    std::map<std::string, FramePlanPassInputs, std::less<>> engine_shader_inputs_;
    // Known once a catalog (EL-4) or get_engine_shaders (EL-4b) carried it;
    // asked once a connection; wanted before the catalog said whether it
    // carries the list itself.
    bool engine_shaders_known_ = false;
    bool engine_shaders_asked_ = false;
    bool engine_shaders_wanted_ = false;
    // UX-5d (EL-4c): when a list the engine answered "building" is asked
    // for again.
    std::optional<std::chrono::steady_clock::time_point> engine_shaders_again_at_;
    std::uint64_t engine_shaders_revision_ = 0;
    void takeEngineShaders(const nlohmann::json &shaders);
    // EL-5s: get_shader_interface's answers by stem; the questions in the
    // air (request id -> stem), outside the one-per-kind read slots since
    // several stems may be asked at once; when a "building" stem is asked
    // again; the stems a refresh (decision 4) waits on, with the ports the
    // canvas had for them; a Shader picked again (decision 2) that waits for
    // its interface.
    std::map<std::string, GraphCanvasShaderInterface, std::less<>> shader_interfaces_;
    std::map<std::uint64_t, std::string> shader_interface_reads_;
    std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> shader_interface_again_at_;
    std::map<std::string, std::vector<std::string>, std::less<>> shader_interface_refreshing_;
    struct PendingComputeShader {
        std::string scope;
        std::string node;
        std::string reference;
    };
    std::optional<PendingComputeShader> pending_compute_shader_;
    std::vector<GraphCanvasPortChange> compute_port_changes_;
    std::uint64_t shader_interface_revision_ = 0;
    std::vector<std::string> project_ray_tracing_choices_;
    bool takeShaderInterfaceReply(std::uint64_t id, const std::string *json, const std::string &failure);
    void comparePortsAfterRefresh(const std::string &reference);
    // passInputs' lookup in the plan alone.
    [[nodiscard]] const FramePlanPassInputs *passInputsFromPlan(
        std::string_view scope, std::string_view node,
        const std::function<const FramePlanPassInputs *(std::string_view)> &find) const;

    std::string values_instance_;
    bool values_received_ = false;
    bool values_published_ = false;
    nlohmann::json values_ = nlohmann::json::array();

    // AG-3b. The node values overlay: the same question in both scopes, with
    // the scope deciding which instance it is asked about.
    bool live_ = false;
    // The instance the reply in hand answers for. The root scope's question is
    // the empty one, so this is compared against nodeValuesInstance() rather
    // than against the watched instance.
    std::string node_values_instance_;
    bool node_values_received_ = false;
    bool node_values_published_ = false;
    nlohmann::json node_values_ = nlohmann::json::array();

    bool status_received_ = false;
    double status_time_ = 0;
    bool animation_available_ = false;
    nlohmann::json animation_sinks_ = nlohmann::json::array();

    // The Timeline's ring buffer. Nothing in the engine keeps a history, so a
    // strip over time is exactly what this session saw pass.
    std::string timeline_node_;
    std::vector<std::string> timeline_ports_;
    std::vector<GraphCanvasTimelineSample> timeline_;
    // One scrub at a time: the second set_time of a drag would race the first
    // render_frame, and the value read back would belong to neither. The
    // render is what ends it, so its id is the one that has to be watched for.
    bool scrubbing_ = false;
    std::string scrub_blocked_reason_;
    std::uint64_t scrub_render_ = 0;

    InspectorNotice failure_;
    // What the last edit is doing or did. Kept apart from failure_ so a failed
    // report never reads as a failed edit, and the other way round.
    InspectorNotice edit_notice_;
    std::string ticket_;
    std::uint64_t content_revision_ = 0;
    std::uint64_t report_revision_ = 0;
    std::uint64_t placement_revision_ = 0;

    struct HistoryEntry {
        nlohmann::json forward;
        nlohmann::json inverse;
        // GL-1d, decision 3: what the forward list sends beside its ops
        // (`material_route_default`), or null. An Undo sends the inverse
        // alone: the route default is the project's, not the list's.
        nlohmann::json params;
    };
    enum class HistoryAction { Edit, Undo, Redo };
    struct PendingHistory {
        HistoryAction action;
        HistoryEntry entry;
    };
    std::vector<HistoryEntry> undo_;
    std::vector<HistoryEntry> redo_;
    std::optional<PendingHistory> pending_history_;
    // Q-13. A placement refused because another panel's commit moved the
    // document is written again, once, against the document read next. The
    // placement is the canvas's own (the document only keeps it), so the same
    // op list is what it would build again; any other gesture is dropped as
    // before, because it was built from the nodes the old revision held.
    std::optional<nlohmann::json> layout_retry_;
    bool layout_retrying_ = false;
    // Q-18. Whether the edit in flight is a placement only (see
    // planCommitRevision()), and whether a commit that was not is still
    // waiting for the document read after it -- which then reads the frame
    // plan whatever the read shows, a read that looks like a placement only
    // included.
    bool pending_placement_only_ = false;
    bool plan_after_commit_ = false;
    std::uint64_t plan_commit_revision_ = 0;
    // GL-1b, decision 2: the line a committed edit's restart_required asked
    // for, until the owner takes it (takeEngineRestart).
    std::optional<std::string> engine_restart_;
    // History belongs to exactly one committed document. A refresh showing
    // another writer's revision invalidates it instead of overwriting them.
    std::string history_digest_;
    static constexpr std::size_t historyCapacity = 100;
    // LP-4c, decision 7: each op is inverted against the document the ops
    // before it produced, which a copy of the document is walked through.
    nlohmann::json inverseOps(const nlohmann::json &ops) const;
    bool submitHistory(HistoryAction action, HistoryEntry entry);
    // GL-1d, decision 3: an engine that predates edit_graph_document's
    // material_route_default refuses the whole call over the field. The
    // list goes again without it (the engine then names the unserved
    // route), and later additions no longer offer it. False when the
    // refusal is not that one.
    bool retryWithoutRouteDefault(const std::string &message);
    bool route_default_unknown_ = false;
    void commitHistory(const nlohmann::json &response);
    void clearHistory();

    // The current scope and the path taken to reach it. The first entry is
    // always the root scope.
    std::vector<std::string> breadcrumb_{std::string{}};
    // LP-4c, decision 5: the loop the current body was entered through, and
    // which branch of it each loop opens (the branch combo's choice).
    std::string entered_loop_;
    std::map<std::string, std::size_t, std::less<>> loop_branch_;

    // LP-4c: get_frame_plan's loops[], by loop id. Read whenever a document
    // with a root graph.loop arrives.
    nlohmann::json loop_reports_ = nlohmann::json::object();
    bool loop_reports_received_ = false;
    // LP-4f1: read off the same get_frame_plan -- the view families its
    // passes run in (with their axes), each node's family and axes by name,
    // and the render targets `mip_levels_of` / `layers_of` can name.
    std::vector<GraphCanvasViewFamily> view_families_;
    std::map<std::string, GraphCanvasViewFamily, std::less<>> plan_node_views_;
    std::vector<std::string> plan_targets_;
    // LP-4f3 (VF-2): per secondary view family, the size the engine holds
    // its passes to, as the plan's family_extent on the family's nodes says.
    std::map<std::string, FramePlanFamilyExtent, std::less<>> family_extents_;
    void readPlanViews(const nlohmann::json &plan);
    // Q-19: every pass's pass_inputs off the same get_frame_plan, by the
    // pass's name, and the name of each pass a loop expanded, by its
    // loop_origin -- a body node's sockets are read off one of its copies.
    std::map<std::string, FramePlanPassInputs, std::less<>> plan_pass_inputs_;
    struct PlanLoopCopy {
        FramePlanLoopOrigin origin;
        std::string name;
    };
    std::vector<PlanLoopCopy> plan_loop_copies_;
    void readPlanPassInputs(const nlohmann::json &plan);
    // A new pass has no pipeline until the engine publishes the generation
    // that holds it, so the plan read right after the edit may not list it.
    // The plan is read again on the clock, a bounded number of times after
    // each document read, while a render node's pass is not in it.
    int plan_retry_budget_ = 0;
    bool plan_retry_wanted_ = false;
    static constexpr int planRetryLimit = 40;
    // PS-1s, decision 4: after rerun_once, the plan is read again on the
    // clock until a pass it scheduled says it ran later than it had (by name:
    // the frame it had last run in, 0 for never), at most this many times.
    int once_follow_reads_ = 0;
    std::map<std::string, std::uint64_t, std::less<>> once_follow_from_;
    static constexpr int onceFollowLimit = 40;
    // UX-12c, decision 1: followDrawnMaterials's reads left.
    int drawn_follow_reads_ = 0;
    static constexpr int drawnFollowLimit = 12;
    // The node the rerun_once in flight was about ("" for every once node).
    std::string once_rerun_node_;
    // Decision 2: what the next commit says in place of "Graph change saved."
    // (one sentence: the notice shows its first).
    std::string saved_note_;
    void receiveRerunOnce(const nlohmann::json &response);
    void receiveRerunOnceRefusal(const std::string &message);
    // GL-1: how many more times a plan that failed, while none has arrived
    // yet, is asked for -- two minutes of polls.
    int plan_wait_budget_ = 0;
    static constexpr int planWaitLimit = 480;
    [[nodiscard]] bool planMissesRenderNode() const;
    // The render.image type a numbered port reading `source_node.source_port`
    // in `scope` is declared with: what the source says it is, followed
    // through a body's graph.input to what feeds the loop or the instance.
    [[nodiscard]] nlohmann::json imageTypeOf(std::string_view scope, std::string_view node,
                                             std::string_view port, int depth) const;
    // Decision 2's list for a source that may not exist yet (the zone's new
    // graph.input): its `type`, and the `base` a new port's name comes from.
    [[nodiscard]] nlohmann::json socketOps(std::string_view scope, const std::string &node,
                                           const std::string &socket,
                                           const std::string &source_node,
                                           const std::string &source_port,
                                           const std::string &base, const nlohmann::json &type,
                                           std::string *refusal) const;
    // Q-19, decision 4: a definition a committed edit renamed, so a trail
    // that stood in it follows it to its new name.
    std::map<std::string, std::string> committed_renames_;
    [[nodiscard]] const std::vector<FramePlanViewAxis> *familyAxes(std::string_view family) const;
    void requestLoopReports();
    // LP-4e: a draw_loop's report holds its first page of materials. The
    // rest are asked for a page at a time, one loop after another, until each
    // holds its `total` (or a page brings nothing new).
    std::string loop_page_node_;
    std::set<std::string, std::less<>> loop_pages_done_;
    void requestNextLoopPage();
    void receiveLoopPage(const nlohmann::json &plan);
    const nlohmann::json *loopReport(std::string_view id) const;
    // UX-5a: outputResource in one scope (a loop body's definition, or the
    // root), and a loop's output in the root.
    [[nodiscard]] std::string scopeOutputResource(std::string_view scope, std::string_view node,
                                                  std::string_view port) const;
    [[nodiscard]] std::string loopOutputResource(const std::string &loop_id,
                                                 std::string_view port) const;
    // --- GL-1: what the frame plan holds besides the graph's own nodes.
    std::vector<GraphCanvasPlanPass> plan_passes_;
    std::string plan_graph_;
    void readPlanPasses(const nlohmann::json &plan);
    // --- GL-1e: the plan's buffers (resources[].kind), and the authoring
    // context read after a plan with a rendering-section pass.
    std::set<std::string, std::less<>> plan_buffers_;
    nlohmann::json render_authoring_ = nlohmann::json::object();
    std::uint64_t authoring_revision_ = 0;
    void requestRenderAuthoring();
    // UX-2: the context was asked for once for its render_target_vocabulary
    // (askRenderTargetVocabulary), on this connection.
    bool vocabulary_asked_ = false;
    // UX-12e, decision 1: the document digest the context was last asked
    // for by a parameter field (askRenderAuthoringForChoices).
    std::string authoring_choices_digest_;
    // UX-2: the shaders a render pass's `shader` is picked from, handed over
    // by the canvas's owner (the Pass Editor's list: the project's and the
    // engine's).
    std::vector<std::string> vertex_shader_choices_;
    std::vector<std::string> fragment_shader_choices_;
    // UX-3: the project's compute shaders (shaders/*.comp), handed over by
    // the owner, for a new render.compute.
    std::vector<std::string> compute_shader_choices_;
    // UX-3: the ops of a new render.snapshot_copy's images, and its inputs.
    void snapshotCopySeedOps(std::string_view in_scope, const std::string &scope_token,
                             const std::string &id, const std::string &after,
                             nlohmann::json &ops, nlohmann::json &inputs) const;
    // --- GL-2b: the plan's lower layers, by the frame plan node names each
    // element carries (from_nodes, a barrier's ends, a scope's nodes).
    struct PlanBarrier {
        std::string from;
        std::string to;
        std::string resource;
        std::string kind;
    };
    struct PlanResource {
        std::string name;
        std::string format;
        std::vector<std::string> from_nodes;
    };
    struct PlanAliasGroup {
        std::string id;
        std::vector<std::string> resources;
        std::vector<std::string> from_nodes;
    };
    struct PlanScope {
        std::string id;
        std::string kind;
        std::vector<std::string> nodes;
    };
    struct PlanDecision {
        std::string id;
        std::string subject;
        std::string selected;
        std::vector<std::string> from_nodes;
    };
    struct PlanCandidate {
        std::string candidate;
        std::string endpoint;
        bool feasible = false;
        bool selected = false;
    };
    // Whether the engine publishes origin at all (GL-2a): its plan always
    // carries diagnostics and deactivated_graph_nodes. An older one's is
    // read by GL-1's name match (graphOwnerOf).
    bool plan_origin_aware_ = false;
    std::vector<PlanBarrier> plan_barriers_;
    std::vector<PlanResource> plan_resources_;
    std::vector<PlanAliasGroup> plan_alias_groups_;
    std::vector<PlanScope> plan_scopes_;
    std::vector<PlanDecision> plan_decisions_;
    std::vector<PlanCandidate> plan_candidates_;
    // execution_plan.nodes[].resource_uses: node -> resource -> access.
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>>
        plan_access_;
    std::vector<GraphCanvasPlanDiagnostic> plan_diagnostics_;
    std::vector<GraphCanvasDeactivatedNode> deactivated_nodes_;
    // GL-3a: the plan's fold_decisions (absent while no group is verified).
    std::vector<GraphCanvasFoldDecision> plan_fold_decisions_;
    void readPlanLayers(const nlohmann::json &plan);
    // The endpoint most of the plan's passes run on.
    [[nodiscard]] std::string graphBackend() const;
    // get_graph_templates, asked once a plan names a feature: feature
    // reference -> the template's document (nodes, definitions, outputs).
    bool templates_requested_ = false;
    nlohmann::json templates_ = nlohmann::json::object();
    // Decision 5: the domains the root canvas shows (session state).
    std::set<std::string, std::less<>> domains_shown_{"render", "pose", "value", "audio", "ui"};
    // Decision 3. A folded view is a document Studio builds and draws in
    // place of the real one, read only: the real one waits in the stash,
    // and every reply is read against it (FoldedSuspend) before the view
    // is built again from the trail.
    struct FoldedStash {
        nlohmann::json nodes;
        nlohmann::json definitions;
        nlohmann::json layout;
        nlohmann::json loop_reports;
        bool loop_reports_received = false;
        std::vector<std::string> breadcrumb;
        std::string entered_loop;
        std::map<std::string, std::size_t, std::less<>> loop_branch;
    };
    std::optional<FoldedStash> folded_stash_;
    std::vector<GraphCanvasFoldedStep> folded_trail_;
    bool folded_refused_ = false;
    struct FoldedSuspend;
    friend struct FoldedSuspend;
    [[nodiscard]] const nlohmann::json &realNodes() const noexcept {
        return folded_stash_ ? folded_stash_->nodes : nodes_;
    }
    // The canvas node a pass of the plan stands under at the root: the
    // graph node that made it, or its folded node. Empty for an anchor.
    [[nodiscard]] std::string graphOwnerOf(const GraphCanvasPlanPass &pass) const;
    [[nodiscard]] std::string foldedOwnerOf(const GraphCanvasPlanPass &pass) const;
    [[nodiscard]] std::vector<GraphCanvasFoldedNode> rootFoldedNodes() const;
    [[nodiscard]] std::vector<GraphCanvasFoldedNode> featurePassNodes(std::string_view feature) const;
    // GL-5a, decision 1: the folded node's owner, from the authoring context.
    void markFoldedOwner(GraphCanvasFoldedNode &node) const;
    [[nodiscard]] const GraphCanvasPlanPass *planPass(std::string_view name) const;
    // The plan's passes a node of `scope` made: a root node's, or a body
    // node's copies (as loopElementRead finds them).
    [[nodiscard]] std::vector<const GraphCanvasPlanPass *>
    planPassesMadeBy(std::string_view scope, std::string_view node) const;
    // The document a step shows; false when the plan (or the templates) no
    // longer holds what it names.
    [[nodiscard]] bool foldedDocument(const GraphCanvasFoldedStep &step, nlohmann::json &nodes,
                                      nlohmann::json &definitions,
                                      nlohmann::json &reports) const;
    // Show the trail's last step (stashing the real document first).
    bool showFoldedTop();
    void restoreFoldedStash();
    // Decision 4: an edit inside a folded view is refused here, before
    // anything is built into a request; a placement only is dropped quietly.
    bool refuseFoldedEdit(const nlohmann::json &ops);

    // Decision 9: what the last refusal pointed at.
    std::vector<GraphCanvasRefusalMark> refusal_marks_;
    // `scope`: where the refused edit was made (UX-12a: a node the engine
    // names by its parameter is marked there).
    void markRefusal(const nlohmann::json &error, const std::string &reason,
                     const std::string &scope = {});
    // UX-12a, decision 4.
    std::optional<GraphCanvasParameterRefusal> parameter_refusal_;
    // UX-13b, decision 3.
    std::optional<GraphCanvasScalarTargetFix> scalar_target_fix_;
    // The branch a loop runs for one element: the report's answer when there
    // is one, else the first branch whose integer condition holds.
    std::optional<std::size_t> branchForElement(std::string_view loop,
                                                std::int64_t element) const;
    // The body a descent into the loop lands in: the chosen branch's, else
    // the first branch that has one. Empty when no branch has a body.
    std::string loopEntryBody(const nlohmann::json &node) const;
    // One gesture on a loop's parameters: the whole object, one key changed.
    bool setLoopParameters(const std::string &id, nlohmann::json parameters);
    [[nodiscard]] nlohmann::json loopParameters(std::string_view id) const;

    void request(RequestKind kind, std::string method, nlohmann::json params);
    void read(RequestKind kind, std::string method, nlohmann::json params);
    // Take the pending read this id belongs to, if any. A reply whose slot has
    // already been replaced by a newer question of the same kind is dropped.
    std::optional<PendingRequest> takeRead(std::uint64_t id);
    void clearContext();
    void requestValues();
    // The pair of reads the observation overlay runs on: the node values for
    // the scope on screen, and the status the Timeline's axis and its
    // animation band come from.
    void requestObservation();
    void clearObservation();
    // Point the overlay at an instance without asking anything yet: descending
    // into one changes both the watch and the scope, and asking in between
    // would send a question about the scope being left.
    void setWatchedInstance(std::string instance);
    // The scope on screen changed: the overlay in hand is about the one that
    // was left, and the question has to be asked again.
    void enteredScope();
    void appendTimelineSample(std::uint64_t frame);
    // The `instance` the current scope's node values have to be asked for: the
    // empty question for the root document, the watched instance for a
    // definition body.
    [[nodiscard]] std::string nodeValuesInstance() const;
    // The outputs object get_graph_node_values reported for one node of the
    // scope on screen, or null.
    const nlohmann::json *nodeOutputs(std::string_view scope,
                                      std::string_view id) const;
    const nlohmann::json *scopeNodes(std::string_view scope) const;
    const nlohmann::json *operation(std::string_view op) const;
    // The type leaving one port of a definition, followed through whatever the
    // definition's outputs map names. Bounded: a definition whose output is
    // another instance's output can nest, and a malformed chain must not spin.
    std::string definitionOutputType(std::string_view definition,
                                     std::string_view port, int depth) const;
    const nlohmann::json *findNode(std::string_view scope,
                                   std::string_view id) const;
    // Q-11. The id of the scope's first value.input reading scene_time, or
    // empty. A placed audio.playback reads its time from it.
    [[nodiscard]] std::string sceneTimeNode(std::string_view scope) const;
    // One gesture is one op list, one ticket and one CAS: two edits in the same
    // frame cannot both land.
    bool submitOps(nlohmann::json ops);
    // GL-1f (RS-4): the list with the writers' `outputs` kept in step -- a
    // forwarded output (a material pass's colour or depth, a shadow_depth's
    // depth, a snapshot_copy's destination) a set_input, an add_node or a
    // body's outputs reads is listed first (replace_node before the op); one
    // the list stopped reading, and nothing else reads, is taken out after.
    [[nodiscard]] nlohmann::json withWriterOutputs(nlohmann::json ops) const;
    // The type of the edge into a node's input: what the node on the other
    // end says it is, through the writers that hand on what they wrote.
    // Null when nothing says.
    [[nodiscard]] nlohmann::json forwardedImageType(std::string_view scope, std::string_view node,
                                                    std::string_view input, int depth) const;
    // The scope token the engine understands: the root document is "root", a
    // definition body is its own name.
    [[nodiscard]] std::string scopeToken() const;
    [[nodiscard]] static std::string scopeToken(std::string_view scope);

  public:
    // A reconnect keeps the Undo / Redo history (OT-1): the next document
    // read keeps it only when its digest is the one the history was built on.
    void setConnected(bool connected);
    bool connected() const noexcept { return connected_; }
    // OT-1: another project is opened. Its document is not the one the
    // history is about, even if its digest happened to match.
    void forgetHistory();

    void refresh();
    void requestCatalog();
    // Which root instance the values overlay follows. Idempotent: reselecting
    // the same instance must not throw away what is already shown.
    void watchInstance(std::string instance);
    const std::string &watchedInstance() const noexcept {
        return values_instance_;
    }
    // Nothing here holds a ticket, so polling only picks up the one read the
    // canvas still owes: the catalog it could not ask for while disconnected.
    void poll();
    // True while a poll would actually ask the engine something: the catalog
    // has still to be fetched, Live is on, or an edit ticket is outstanding.
    // False the rest of the time -- and then nothing has to wake up for it.
    [[nodiscard]] bool pollWanted() const noexcept {
        if (!connected_) return false;
        return !catalog_requested_ || live_ || (!pending_ && !ticket_.empty()) ||
               plan_retry_wanted_ || once_follow_reads_ > 0 || drawn_follow_reads_ > 0;
    }

    std::vector<InspectorRpcRequest> takeRpcRequests();
    void receiveRpcResult(std::uint64_t id, std::string_view json);
    void receiveRpcFailure(std::uint64_t id, std::string message);

    std::vector<GraphCanvasScope> scopes() const;
    const std::string &currentScope() const noexcept {
        return breadcrumb_.back();
    }
    const std::vector<std::string> &breadcrumb() const noexcept {
        return breadcrumb_;
    }
    // Selecting a scope from the combo restarts the trail: the root scope, and
    // the definition under it when one was chosen.
    bool selectScope(std::string_view scope);
    // Descend into the definition a root-scope graph.instance names.
    bool enterInstance(std::string_view node_id);
    // Whether that descent would land anywhere, without taking it.
    bool canEnterInstance(std::string_view node_id) const;
    bool leaveScope();

    std::vector<GraphCanvasNode> nodes() const { return nodes(currentScope()); }
    std::vector<GraphCanvasEdge> edges() const { return edges(currentScope()); }
    std::vector<GraphCanvasNode> nodes(std::string_view scope) const;
    std::vector<GraphCanvasEdge> edges(std::string_view scope) const;
    // (scope, node id) for every node the document holds, in every scope. The
    // scene prunes the positions it kept for nodes that are no longer there.
    std::vector<std::pair<std::string, std::string>> allNodeKeys() const;

    // Root instances that own a scene object, which is what get_graph_values
    // can answer for.
    std::vector<std::string> valueInstances() const;
    bool valuesPublished() const noexcept {
        return values_received_ && values_published_;
    }
    bool valuesReceived() const noexcept { return values_received_; }

    // --- AG-3b: observation.

    // Live follows the running engine: while it is on, every poll asks the
    // node values and the status again. Off, the overlay is read when the
    // document moves and when the scope or the watched instance changes --
    // which is what it did before this switch existed.
    void setLive(bool live);
    [[nodiscard]] bool live() const noexcept { return live_; }

    [[nodiscard]] bool nodeValuesReceived() const noexcept {
        return node_values_received_;
    }
    [[nodiscard]] bool nodeValuesPublished() const noexcept {
        return node_values_received_ && node_values_published_;
    }
    // What the status said, for the Timeline's axis.
    [[nodiscard]] bool statusReceived() const noexcept { return status_received_; }
    [[nodiscard]] double statusTime() const noexcept { return status_time_; }
    [[nodiscard]] bool animationAvailable() const noexcept {
        return animation_available_;
    }
    // The sinks whose owner is the owner the watched instance names. An
    // instance with no owner, or a status with no matching sink, answers with
    // nothing: a band over the wrong object's animation is worse than no band.
    [[nodiscard]] std::vector<GraphCanvasAnimationSink> animationSinks() const;

    // Which node the Timeline follows. Its output ports are the traces, up to
    // four of them; naming a different node starts the buffer again, because a
    // strip whose lines changed meaning halfway along says nothing.
    void setTimelineNode(std::string id);
    [[nodiscard]] const std::string &timelineNode() const noexcept {
        return timeline_node_;
    }
    [[nodiscard]] const std::vector<std::string> &timelinePorts() const noexcept {
        return timeline_ports_;
    }
    [[nodiscard]] const std::vector<GraphCanvasTimelineSample> &
    timelineSamples() const noexcept {
        return timeline_;
    }
    void clearTimeline();
    // How many readings the strip keeps. At the Live rate (one poll, 250 ms)
    // this is ten minutes of scene time.
    static constexpr std::size_t timelineCapacity = 2400;
    // At most four lines: a strip with more of them is a wall, and the node
    // that needed five wanted a graph of its own.
    static constexpr std::size_t timelineTraceLimit = 4;

    // Scrub: O-1's reading of "go to a time" -- set the clock, draw one frame,
    // read the values back. Two writes and the pair of reads, in that order,
    // and never two scrubs at once.
    bool scrub(double time);
    [[nodiscard]] bool scrubbing() const noexcept { return scrubbing_; }
    // PM-3a (design_play_mode 12.6): while the game plays its clock moves on
    // its own, so a scrub is refused and `reason` says why; empty lets it be.
    void setScrubBlockedReason(std::string reason) { scrub_blocked_reason_ = std::move(reason); }
    [[nodiscard]] const std::string &scrubBlockedReason() const noexcept {
        return scrub_blocked_reason_;
    }

    // --- Q-8: the UI node a `ui.value` / `ui.visible` sink writes.

    // The parameter an operation names a UI node in ("node" for the two ui.*
    // sinks), or empty for every other operation.
    [[nodiscard]] static std::string_view uiNodeParameter(std::string_view op) noexcept;
    // What the Studio read from the project's UI document. Held here so the
    // palette can seed a new sink with a node that exists.
    void setUiNodes(std::vector<GraphCanvasUiNode> nodes) { ui_nodes_ = std::move(nodes); }
    [[nodiscard]] const std::vector<GraphCanvasUiNode> &uiNodes() const noexcept {
        return ui_nodes_;
    }
    // The paths the `node` field of this operation offers: for `ui.value` the
    // gauges (the only widget whose value can be written), for `ui.visible`
    // every node. Empty for an operation with no UI node parameter.
    [[nodiscard]] std::vector<std::string> uiNodeChoices(std::string_view op) const;

    // --- Q-10: the three parameters of `audio.playback` that are not ports.

    [[nodiscard]] static bool isAudioPlayback(std::string_view op) noexcept;
    // The buses a voice plays on. The engine takes exactly these two
    // ("unknown audio bus: ... (a voice plays on se or bgm)") and names them
    // nowhere a client can read, so the list is held here, beside the
    // engine's default first.
    [[nodiscard]] static const std::vector<std::string> &audioBusChoices();
    // How the integrated time map wraps, the engine's default first.
    [[nodiscard]] static const std::vector<std::string> &audioWrapChoices();
    // The project's sounds, read by the widget. The palette seeds a new node
    // with the first; the `asset` field lists them all.
    void setAudioAssets(std::vector<std::string> assets) { audio_assets_ = std::move(assets); }
    [[nodiscard]] const std::vector<std::string> &audioAssets() const noexcept {
        return audio_assets_;
    }

    bool catalogReceived() const noexcept { return catalog_received_; }
    // The catalog's schema for one semantic type, or null when the catalog
    // never carried it. The socket resolver reads its traits and its optional
    // display declaration through here.
    const nlohmann::json *typeSchema(std::string_view id) const;
    // N-6. The placement the document holds for one scope, in the shape
    // `set_layout` writes: `{"nodes": {...}, "reroutes": [...], "ties": [...]}`,
    // or an empty object when the document says nothing about it. This is the
    // canvas's source of truth for where things are; the session table under it
    // only answers for a node the document has never placed.
    [[nodiscard]] nlohmann::json scopeLayout(std::string_view scope) const;
    [[nodiscard]] nlohmann::json scopeLayout() const {
        return scopeLayout(currentScope());
    }
    // One `set_layout`, naming the scope rather than taking the current one:
    // a debounced write can land after the canvas has already moved on, and it
    // must still be about the scope the gesture happened in.
    bool setLayout(const std::string &scope, const nlohmann::json &layout);

    bool definitionsReported() const noexcept { return definitions_reported_; }
    bool contextReceived() const noexcept { return context_received_; }
    const std::string &digest() const noexcept { return digest_; }
    // N-8. Empty until a document has arrived. "pelican.graph" is the root that
    // can carry a placement; anything else is a legacy rendering root.
    const std::string &rootSchema() const noexcept { return root_schema_; }
    const std::string &sourceReference() const noexcept {
        return source_reference_;
    }
    bool graphRoot() const noexcept { return root_schema_ == "pelican.graph"; }

    // One line for the panel. An error the user has to see outranks the two
    // "this view is incomplete, and here is why" notices, which outrank the
    // report of an unpublished graph.
    InspectorNotice notice() const;

    auto contentRevision() const noexcept { return content_revision_; }
    auto reportRevision() const noexcept { return report_revision_; }
    // Q-5. A read that differs from the one before only in where things are
    // placed -- the reply to a node drag, a moved control point, the Frame
    // Plan's member -- moves this one instead of contentRevision(): nothing
    // the canvas draws a node from has changed, so the values reported against
    // those nodes stay, and a canvas already drawing that placement has no
    // reason to throw its items away and draw them again.
    auto placementRevision() const noexcept { return placement_revision_; }
    // Q-18. How many of this tab's edits have committed that can change the
    // frame plan: every edit but a placement (an op list of set_layout only,
    // which the Frame Plan's own node positions are written as too). The
    // owner compares it before and after a reply to tell the other panels
    // that show the plan to read it again.
    auto planCommitRevision() const noexcept { return plan_commit_revision_; }
    // GL-1b, decision 2. An edit committed with `restart_required: true` (the
    // engine could not apply it live -- RS-1's `restart_reason` says why):
    // the document is written, and the engine has to start again to run it.
    // The model neither reads the document nor the plan after such a commit;
    // this hands the owner, once, the line to restart with (the same
    // restart the Frame Plan's G-4 asks for). Empty when none is asked.
    [[nodiscard]] std::optional<std::string> takeEngineRestart() {
        return std::exchange(engine_restart_, std::nullopt);
    }
    // The restart was declined: the engine that committed the edit is still
    // running. Say why, and read what it has now.
    void engineRestartDeclined(std::string message);
    // GL-1b, decision 3. Added to a refusal an engine without RS-1 gives a
    // graph edit it could only apply by restarting.
    static constexpr std::string_view restartNotOfferedHint =
        " \xE2\x80\x94 this change needs an engine restart, which the engine does not "
        "offer for graph edits yet";
    bool busy() const noexcept { return pending_.has_value() || !ticket_.empty(); }
    bool canRefresh() const noexcept { return connected_ && !busy(); }
    // An edit needs a digest to compare against: without one there is nothing
    // to hold the document still between the read and the write.
    bool canEdit() const noexcept { return canRefresh() && !digest_.empty(); }
    bool canUndo() const noexcept {
        return canEdit() && !undo_.empty() && digest_ == history_digest_;
    }
    bool canRedo() const noexcept {
        return canEdit() && !redo_.empty() && digest_ == history_digest_;
    }
    bool undo();
    bool redo();

    // --- Editing. Each call is one gesture on the canvas and, when it returns
    // true, exactly one edit_graph_document carrying one op list. Every op
    // names the scope the canvas is showing, so a definition body is edited in
    // place rather than through its instances.

    // The authored parameters object of a node in the current scope. The pill
    // editor reads the whole of it and sends it back with one key replaced,
    // because set_node_parameters replaces rather than merges.
    nlohmann::json nodeParameters(std::string_view id) const;
    // Whether the canvas should let this drag land: a port that declares no
    // accepted type takes anything, and so does a producer whose type the
    // catalog never named. Two declared types have to be the same one.
    bool canConnect(std::string_view from_node, std::string_view from_port,
                    std::string_view to_node, std::string_view to_port) const;

    bool connectInput(const std::string &node, const std::string &port,
                      const std::string &source_node, const std::string &source_port);
    bool disconnectInput(const std::string &node, const std::string &port);
    // One key of one node's parameters. For an instance port this is the
    // stored value the port returns to; elsewhere it is the node's own setting.
    bool setNodeParameter(const std::string &node, const std::string &key,
                          const nlohmann::json &value);
    bool removeNode(const std::string &node);
    bool setNodeLabel(const std::string &node, const std::string &label);
    // Add one catalog operation to the current scope. Required inputs are
    // seeded with their own value.constant in the same op list, because a node
    // whose required input is unconnected is not a document the engine loads.
    bool addNode(const std::string &op, const std::string &definition = {});
    // The ops addNode would send, so the same list can be read back in a test
    // and replayed over the RPC.
    nlohmann::json addNodeOps(const std::string &op, const std::string &definition = {}) const;

    // LP-4d1: the same gestures, naming the scope they happen in rather than
    // taking the one on screen. The root canvas draws a loop's body inside
    // its zone, so a node there is edited in its definition while the canvas
    // shows the root; every op these send carries `scope: <definition>`.
    // The forms above are these with the current scope.
    [[nodiscard]] nlohmann::json nodeParameters(std::string_view scope,
                                                std::string_view id) const;
    [[nodiscard]] bool canConnect(std::string_view scope, std::string_view from_node,
                                  std::string_view from_port, std::string_view to_node,
                                  std::string_view to_port) const;
    // UX-1: the same judgement over a scope's nodes already read -- a line
    // being drawn asks it of every socket on the canvas at once.
    [[nodiscard]] static bool canConnectAmong(const std::vector<GraphCanvasNode> &drawn,
                                              std::string_view from_node,
                                              std::string_view from_port,
                                              std::string_view to_node,
                                              std::string_view to_port);
    bool connectInput(std::string_view scope, const std::string &node,
                      const std::string &port, const std::string &source_node,
                      const std::string &source_port);
    bool disconnectInput(std::string_view scope, const std::string &node,
                         const std::string &port);
    bool setNodeParameter(std::string_view scope, const std::string &node,
                          const std::string &key, const nlohmann::json &value);
    bool removeNode(std::string_view scope, const std::string &node);
    bool setNodeLabel(std::string_view scope, const std::string &node,
                      const std::string &label);
    // UX-3, decision 1: `seed` is what the palette chose beside the op --
    // a render.target_ref's {"name"}, a render.snapshot_copy's
    // {"snapshot_after"}, a render.buffer's / render.attribute's {"element",
    // "domain", "count"} -- and is empty for every other one.
    bool addNodeIn(std::string_view scope, const std::string &op,
                   const std::string &definition = {},
                   const nlohmann::json &seed = nlohmann::json::object());
    // GL-1d, decision 3: the params the palette's addition sends beside its
    // ops: `material_route_default: "forward"` for a Forward opaque material
    // pass in a pipeline with no pass for deferred materials (the Frame
    // Plan's judgment, frameplanmodel's materialRouteOffersForwardDefault),
    // else an empty object.
    [[nodiscard]] nlohmann::json addNodeParams(std::string_view scope, const std::string &op,
                                               const std::string &definition = {}) const;
    // Whether a material pass of the last frame plan, or one the root
    // document holds, takes the deferred_geometry route.
    [[nodiscard]] bool pipelineHasDeferredMaterialPass() const;
    [[nodiscard]] nlohmann::json addNodeOpsIn(
        std::string_view scope, const std::string &op, const std::string &definition = {},
        const nlohmann::json &seed = nlohmann::json::object()) const;
    [[nodiscard]] std::vector<GraphCanvasCatalogEntry> paletteOperationsIn(
        std::string_view scope, std::string_view text = {}, std::string_view domain = {}) const;
    [[nodiscard]] std::vector<std::string> paletteDomainsIn(std::string_view scope) const;
    [[nodiscard]] std::string suggestNodeId(std::string_view scope, std::string_view op) const;

    // What the palette offers here, filtered by a case-insensitive substring of
    // the operation name and, when non-empty, by domain.
    std::vector<GraphCanvasCatalogEntry> paletteOperations(
        std::string_view text = {}, std::string_view domain = {}) const;
    std::vector<std::string> paletteDomains() const;
    // An id no node in this scope is using, derived from the operation name.
    std::string suggestNodeId(std::string_view op) const;
    // The stored values an instance of this definition has to start with: one
    // per graph.input, its declared default or zero.
    nlohmann::json definitionDefaults(std::string_view definition) const;

    const InspectorNotice &editNotice() const noexcept { return edit_notice_; }

    // --- LP-4c: graph.loop.

    // A root graph.loop read into the Loop fields' terms, or nothing.
    [[nodiscard]] std::optional<GraphCanvasLoop> loop(std::string_view id) const;
    [[nodiscard]] bool loopReportsReceived() const noexcept {
        return loop_reports_received_;
    }
    // A definition name no definition is using, from `base`.
    [[nodiscard]] std::string suggestDefinitionName(std::string_view base = "loop_body") const;
    // Decision 2. The ops "Add loop" sends: over one element, one branch
    // running `body`, and every body output declared as `p`. With
    // `new_definition` set, the body is a new, empty definition of that name,
    // added in the same list.
    [[nodiscard]] nlohmann::json addLoopOps(const std::string &body,
                                            const std::string &new_definition = {}) const;
    bool addLoop(const std::string &body, const std::string &new_definition = {});

    // Decision 3: the Loop fields. Each is one gesture: one set_node_parameters
    // carrying the whole parameters object with one member changed (or, for
    // the outputs, one replace_node).
    bool setLoopCount(const std::string &id, std::int64_t count);
    bool setLoopCountExpression(const std::string &id, const std::string &expression);
    // LP-4f1, decision 1: over one view axis of one family
    // (`over: {view_axis: {family, axis}}`); the rest of the parameters stay.
    bool setLoopViewAxis(const std::string &id, const std::string &family,
                         const std::string &axis);
    // GL-1d, decision 2: over the draws of the material pass that reads the
    // loop's `draws` (`over: {draw_items: {contract}}`, tier draw_loop, the
    // phase's order); what a draw_loop cannot take goes. The contract is that
    // pass's; empty when no root material pass reads the loop, and then the
    // loop cannot run over draw items.
    [[nodiscard]] std::string loopDrawItemsContract(std::string_view id) const;
    bool setLoopDrawItems(const std::string &id);
    // The view families a pass of the last frame plan runs in, with their
    // axes: what the Loop fields' View axis offers. Families without axes
    // ($main, planar, an author's) are not in it.
    [[nodiscard]] const std::vector<GraphCanvasViewFamily> &viewFamilies() const noexcept {
        return view_families_;
    }
    // The render targets of the last frame plan, for the count
    // expression's mip_levels_of / layers_of.
    [[nodiscard]] const std::vector<std::string> &planTargets() const noexcept {
        return plan_targets_;
    }
    // LP-4f2. The family a loop over a view axis runs the body `scope` in
    // (its over.view_axis.family); empty for any other scope.
    [[nodiscard]] std::string viewAxisFamilyOf(std::string_view scope) const;
    // LC-1s, decision 1: `scope` is the body of a loop of the root document
    // (a branch's body included), so a pass made there is copied per element.
    [[nodiscard]] bool isLoopBodyScope(std::string_view scope) const;
    // LC-1s, decision 2: whether the pass `node` of `scope` reads the loop
    // element, from the frame plan's reads_loop_index on the passes it made
    // (a body node: the copies whose origin is it; a root node: its passes).
    [[nodiscard]] GraphCanvasLoopElementRead loopElementRead(std::string_view scope,
                                                             std::string_view node) const;
    // The size a new image drawn in `family` is made at, as render.image
    // parameters, from the frame plan's family_extent / family_layers on the
    // family's nodes (LP-4f3, VF-2): {width, height[, layers]} for a fixed
    // size, {extent_scale[, layers]} for one relative to the screen. Nothing
    // when the plan carries no family_extent for it (the image is made as
    // before, and a mismatch is the engine's to name:
    // render.view_family_extent_mismatch@1).
    [[nodiscard]] std::optional<nlohmann::json> viewFamilyImageExtent(
        std::string_view family) const;

    // --- LP-4f1, decision 2: a render node's View field.

    // The operations that take view_select (a pass, a task, a copy).
    [[nodiscard]] static bool takesViewSelect(std::string_view op) noexcept;
    // What the field shows for a node, or nothing when it is not shown: an
    // operation that takes no view_select, or no axes to show. A node in
    // the body of a loop over a view axis gets the loop's family; a root
    // node the family the frame plan lists it in; otherwise the family it
    // names, when the frame plan lists that one.
    [[nodiscard]] std::optional<GraphCanvasViewField> viewField(std::string_view scope,
                                                                std::string_view node) const;
    // One gesture: the node's view_select replaced by `view_select` (an
    // object of axis -> integer or {"$expr": ...}; empty takes the member
    // out), and, with `family` set and the node naming no view_family, that
    // family written beside it.
    bool setViewSelect(std::string_view scope, const std::string &node,
                       const nlohmann::json &view_select, const std::string &family = {});
    bool setLoopDescending(const std::string &id, bool descending);
    bool setLoopLimit(const std::string &id, std::optional<std::int64_t> limit);
    // "" (automatic), "expand" or "draw_loop".
    bool setLoopTier(const std::string &id, const std::string &tier);
    // "" (none), "element_in" or "element_not_in".
    bool setLoopWhere(const std::string &id, const std::string &kind,
                      const std::vector<std::int64_t> &values);
    bool setLoopCarry(const std::string &id,
                      const std::vector<std::pair<std::string, std::string>> &carry);
    bool setLoopOutputs(const std::string &id, const std::vector<std::string> &outputs);
    // Where the copies land: one of loopAnchors() as canonical_anchor, or ""
    // for none (the end of the pass family). Either way an `insert` goes.
    bool setLoopAnchor(const std::string &id, const std::string &anchor);
    // The canonical anchors a loop can be placed at, in the frame's order.
    // A loop with none expands after the scene passes, before the first
    // anchor (LP-1e): Studio does not give one on its own.
    [[nodiscard]] static const std::vector<std::string> &loopAnchors();

    // Decision 4: the branch table. The last branch is the else, which is
    // never removed, moved or given a condition.
    bool addLoopBranch(const std::string &id);
    bool removeLoopBranch(const std::string &id, std::size_t branch);
    bool moveLoopBranch(const std::string &id, std::size_t branch, int delta);
    // "element", "element_in" or "element_not_in" with its integers.
    bool setLoopBranchWhen(const std::string &id, std::size_t branch,
                           const std::string &when, const std::vector<std::int64_t> &values);
    // An empty optional is `(nothing)`, the `null` body.
    bool setLoopBranchBody(const std::string &id, std::size_t branch,
                           const std::optional<std::string> &body);
    // "New definition...": a definition with the loop's boundary (its first
    // body's graph.input nodes and outputs) under `name`, and the branch
    // running it, in one list.
    bool newLoopBranchDefinition(const std::string &id, std::size_t branch,
                                 const std::string &name);
    // The definitions a branch of this loop may run: those whose boundary is
    // the loop's (the same graph.input ids and the same outputs).
    [[nodiscard]] std::vector<std::string> loopBodyChoices(std::string_view id) const;

    // One gesture's op list as built elsewhere -- the zone gestures of LP-4d
    // put a loop input and its body's graph.input in one list -- sent with
    // its inverse like every other gesture.
    bool submitEdit(nlohmann::json ops);

    // LP-0c's three definition ops, for the undo they need (decision 7).
    // Q-19, decision 4: neither is sent when it would be refused for a
    // reason Studio can see; the edit notice then says why, in one line.
    bool renameDefinition(const std::string &from, const std::string &to);
    bool removeDefinition(const std::string &name);
    // Why `to` cannot be `from`'s new name -- empty, a slash, "root" (the
    // root document's scope token), or a definition already there -- as one
    // line; empty when it can. The same name is no rename, and says nothing.
    [[nodiscard]] std::string definitionNameRefusal(std::string_view from,
                                                    std::string_view to) const;
    // The root nodes that run a definition (instances, and loops a branch of
    // which runs it), each once, in document order.
    [[nodiscard]] std::vector<std::string> definitionReferrers(std::string_view definition) const;
    // "Definition <name> is used by <node>, <node>: ..." when something runs
    // it; empty when it can go.
    [[nodiscard]] std::string definitionRemoveRefusal(std::string_view name) const;

    // --- Q-19: a render node's input sockets, and lines into them.

    // The pass_inputs of the pass the node made, from the last frame plan:
    // a root node's pass by its id, a body node's through one of the copies a
    // loop running the body expanded (loop_origin), or an instance's
    // (`<instance>/<id>`). Null when the plan lists none.
    [[nodiscard]] const FramePlanPassInputs *passInputs(std::string_view scope,
                                                        std::string_view node) const;
    // The ports of a render node that read a pass input by number: its
    // `input_index` -> the port. (The engine refuses gaps and duplicates.)
    [[nodiscard]] static std::map<std::int64_t, std::string>
    numberedPorts(const nlohmann::json &parameters);
    // A name for a new numbered port of `node`, proposed from `base` (the
    // source's port, or a graph.input's id): no port or input of the node
    // has it.
    [[nodiscard]] std::string suggestInputPortName(std::string_view scope, std::string_view node,
                                                   std::string_view base) const;
    // Decision 2. `source_node.source_port` dropped on the socket named
    // `socket` (GraphCanvasInputSocket::key()) of `node`: a new numbered port
    // at that binding and the edge into it, in one list, or the edge moved
    // onto the port already there. Empty -- with `refusal` saying why in one
    // line when there is a reason to say -- when it cannot be sent: a
    // skipped binding, a full band, a buffer binding.
    [[nodiscard]] nlohmann::json socketConnectOps(std::string_view scope, const std::string &node,
                                                  const std::string &socket,
                                                  const std::string &source_node,
                                                  const std::string &source_port,
                                                  std::string *refusal = nullptr) const;
    // Sends it, or, refused, puts the line in the edit notice.
    bool connectSocket(std::string_view scope, const std::string &node, const std::string &socket,
                       const std::string &source_node, const std::string &source_port);
    // The numbered port taken out -- its edge and its declaration -- and the
    // ports after it moved down one input_index, in one list. Empty when
    // `port` is not a numbered port of the node. disconnectInput() sends this
    // for such a port: a numbered port without its edge is not a document.
    [[nodiscard]] nlohmann::json removeInputPortOps(std::string_view scope, const std::string &node,
                                                    const std::string &port) const;
    // Decision 3, from outside a zone: a root output dropped on a socket of
    // a body node -- the loop input, its graph.input in every body, and the
    // numbered port reading it, in one list.
    bool addZoneInputToSocket(const std::string &loop, const std::string &source_node,
                              const std::string &source_port, const std::string &body,
                              const std::string &target_node, const std::string &socket);

    // Decision 5. The loop the current scope was entered through (empty when
    // it was not), its branch the descent opened, and a switch to another
    // branch's body while inside.
    [[nodiscard]] const std::string &enteredLoop() const noexcept { return entered_loop_; }
    [[nodiscard]] std::size_t loopBranch(std::string_view id) const;
    bool selectLoopBranch(std::size_t branch);
    // How many root nodes (instances and loop branches) name a definition:
    // more than one is the "shared" mark.
    [[nodiscard]] std::size_t definitionUseCount(std::string_view definition) const;
    // LP-4d1: the body a root loop's zone shows -- the branch its tab chose,
    // else the first branch that has one (the body a descent opens). Empty
    // when no branch has a body. `showLoopBranch` is the tab: session state,
    // never written to the document. False for a branch with no body.
    [[nodiscard]] std::string loopZoneBody(std::string_view loop) const;
    bool showLoopBranch(std::string_view loop, std::size_t branch);
    [[nodiscard]] std::vector<GraphCanvasBodyOutput> bodyOutputs(std::string_view definition) const;

    // Decision 6. The other loops' `p[]` outputs a root loop's input can read
    // one element of, as (loop, port).
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    perIterationSources(std::string_view receiver) const;
    bool connectPerIteration(const std::string &node, const std::string &port,
                             const std::string &source_node, const std::string &source_port,
                             const std::string &expression = "element");

    // --- LP-4d2: lines across a loop zone's edge.

    // The name a new loop input or body output is given, proposed from the
    // port whose line made it (dots and slashes become `_`, a `[]` goes):
    // an input's is no input of the loop and no node of any body its
    // branches run; an output's is no output of any of those bodies and
    // nothing the loop declares.
    [[nodiscard]] std::string suggestLoopInputName(std::string_view loop,
                                                   std::string_view base) const;
    [[nodiscard]] std::string suggestLoopOutputName(std::string_view loop,
                                                    std::string_view base) const;
    // Decision 1. A root node's output drawn into a root loop's zone: a new
    // loop input it feeds (root `set_input`), and a graph.input of that id in
    // every definition the branches run (each once), in one list. With a
    // target -- a node's input in `body`, one of those definitions -- that
    // input reads the new graph.input in the same list. Empty when it cannot
    // be sent.
    [[nodiscard]] nlohmann::json zoneInputOps(std::string_view loop,
                                              const std::string &source_node,
                                              const std::string &source_port,
                                              const std::string &body = {},
                                              const std::string &target_node = {},
                                              const std::string &target_port = {}) const;
    bool addZoneInput(const std::string &loop, const std::string &source_node,
                      const std::string &source_port, const std::string &body = {},
                      const std::string &target_node = {}, const std::string &target_port = {});
    // Decision 2. A body node's output drawn to the zone's right edge: the
    // output in every definition the branches run (DO-1's set_outputs, each
    // body's whole outputs), and `p` declared on the loop (replace_node), in
    // one list. Every other body has to give the same output from a node of
    // the same id and port -- a set_outputs endpoint names a node, so an
    // output cannot be left unconnected there -- and when one cannot, the
    // list is empty and `refusal` says which body, in one line.
    [[nodiscard]] nlohmann::json zoneOutputOps(std::string_view loop, const std::string &body,
                                               const std::string &node, const std::string &port,
                                               std::string *refusal = nullptr) const;
    // Sends it, or, refused, puts the line in the edit notice and sends
    // nothing.
    bool addZoneOutput(const std::string &loop, const std::string &body,
                       const std::string &node, const std::string &port);
    // Decision 3. One pair more (or less) in the loop's `carry`: the payload
    // the Loop fields' carry rows send.
    bool addLoopCarry(const std::string &loop, const std::string &from, const std::string &to);
    bool removeLoopCarry(const std::string &loop, const std::string &from, const std::string &to);
    // Decision 6. Show the body node a loop's copy came from on the root
    // canvas: the root scope, with the loop's tab on the branch that element
    // ran. Answers the definition the node stands in, or empty -- with
    // nothing changed -- when the loop, the branch or the node is not there.
    std::string showLoopOrigin(std::string_view loop, std::int64_t element, std::string_view node);
    // UX-6a: the same definition, asked without showing anything.
    [[nodiscard]] std::string loopOriginScope(std::string_view loop, std::int64_t element,
                                              std::string_view node) const;

    // Decision 9. What the last refused edit pointed at; cleared by the next
    // edit that commits.
    [[nodiscard]] const std::vector<GraphCanvasRefusalMark> &refusalMarks() const noexcept {
        return refusal_marks_;
    }
    // Decision 10. Open the body node a loop copy came from: the scope of
    // the branch that element ran, entered through the loop. False when the
    // loop, the branch or the node is not in the document.
    bool enterLoopOrigin(std::string_view loop, std::int64_t element, std::string_view node);

    // --- LP-4e: the draw_loop stage, render.material_pass and draw.*.

    // The material contracts a render.material_pass is given (the palette's
    // combo), and a material's shader contracts (a draw_loop branch's
    // `contract`), in the order the combos list them.
    [[nodiscard]] static const std::vector<std::string> &materialPassContracts();
    [[nodiscard]] static const std::vector<std::string> &materialShaderContracts();
    // render.material_pass's attachment sockets, in location order: one per
    // material_outputs output when it declares them, else `color` for a
    // forward contract and the five G-buffer ones for a deferred one. Empty
    // for a contract the table does not know. (graphmodule.cpp's
    // renderMaterialPassColorPorts, restated: Studio does not link the graph
    // module's registry.)
    [[nodiscard]] static std::vector<std::string>
    materialPassColorSockets(const nlohmann::json &parameters);
    // A draw_loop body's nodes are draw.* only: the palette offers them in a
    // definition a draw_loop runs, or one that already holds a draw step.
    [[nodiscard]] bool drawBodyScope(std::string_view scope) const;

    // Decision 3: the draw_loop's own fields. `where` tests tags; an empty
    // filter removes it.
    bool setLoopWhereTags(const std::string &id, const GraphCanvasTagFilter &tags);
    // A branch's condition as a draw_loop reads it (AND of what is set). An
    // empty condition is refused: only the last row is the else.
    bool setLoopBranchDrawWhen(const std::string &id, std::size_t branch,
                               const GraphCanvasDrawWhen &when);
    // Every material a draw_loop's report named, all pages read. Empty
    // without a report.
    [[nodiscard]] std::vector<std::string> loopMaterials(std::string_view id) const;

    // Decision 4: a draw step's parameters, each one gesture.
    bool setDrawVariant(const std::string &node, const std::string &variant);
    // render_state: cull / depth_test / depth_write / blend / depth_compare,
    // an absent key keeping the material's own; defines one per entry. Both
    // empty is not an override, and is refused.
    bool setDrawOverride(const std::string &node, const nlohmann::json &render_state,
                         const std::vector<std::string> &defines);
    // LP-4d1: a draw step inside a draw_loop's zone, named by its body.
    bool setDrawVariant(std::string_view scope, const std::string &node,
                        const std::string &variant);
    bool setDrawOverride(std::string_view scope, const std::string &node,
                         const nlohmann::json &render_state,
                         const std::vector<std::string> &defines);
    [[nodiscard]] std::string suggestDrawVariant(std::string_view scope) const;
    bool setMaterialPassContract(const std::string &node, const std::string &contract);
    // The variant a new draw.variant is given: the material_variant of the
    // pass the scope's loop draws in, when it names one, else "variant".
    [[nodiscard]] std::string suggestDrawVariant() const;

    // --- GL-1: features and the rendering section as folded nodes.

    // The last frame plan's passes, in its order, and its graph's name.
    [[nodiscard]] const std::vector<GraphCanvasPlanPass> &planPasses() const noexcept {
        return plan_passes_;
    }
    [[nodiscard]] const std::string &planGraph() const noexcept { return plan_graph_; }
    // Decision 1. The folded nodes the canvas draws: at the root, one per
    // feature and one per other pass no graph node made (not the anchors),
    // in the plan's order -- none while the Render domain is hidden; inside
    // a feature's fold, its passes one by one; elsewhere none.
    [[nodiscard]] std::vector<GraphCanvasFoldedNode> foldedNodes() const;
    // Decision 2. The lines the plan's reads and writes imply between the
    // folded nodes, and between them and the root's graph nodes (a
    // render.target_ref or render.image of the resource's name, else the
    // render node whose pass reads or writes it).
    [[nodiscard]] std::vector<GraphCanvasDerivedEdge> derivedEdges() const;
    // The root canvas node a pass of the plan is drawn under (decision 6):
    // the graph node that made it, or the folded node it is in. Empty when
    // the plan does not list it (or it is an anchor).
    [[nodiscard]] std::string canvasNodeOfPass(std::string_view pass) const;
    // UX-5d, decision 3: whether a resource a refusal's message names is
    // written, in the plan, by a pass or task a graph node made -- not by a
    // feature's task or a pass of the rendering section. The message's words
    // are matched against what the plan's passes write.
    [[nodiscard]] bool graphWritesResourceNamedIn(std::string_view message) const;

    // --- GL-1e: a rendering-section pass's inputs, edited from the Graph.

    // get_render_authoring_context as it last arrived: read after every plan
    // that holds a pass of the rendering section (the root's own, F-6).
    // Empty until one arrives; the revision moves with each.
    [[nodiscard]] const nlohmann::json &renderAuthoringContext() const noexcept {
        return render_authoring_;
    }
    [[nodiscard]] std::uint64_t authoringRevision() const noexcept { return authoring_revision_; }
    // GL-5a, decision 1: whether the context lists `pass` of the plan's graph
    // as one the root's rendering section declares (editable_root_pass).
    [[nodiscard]] bool renderingSectionPass(std::string_view pass) const;
    // The last plan in the Frame Plan's terms, as far as a pass's input
    // sockets need it: its graph, each pass with its pass_inputs, and which
    // resources are buffers.
    [[nodiscard]] FramePlanModel passInputPlan() const;
    // The resource a root node's output hands on: a render.image (or
    // render.buffer, render.attribute) its own id, a render.target_ref the
    // target it names, a render node's output the resource node feeding its
    // input of the same name. Empty for anything else.
    // UX-5a: a graph.loop's output is never the loop's input of the same
    // name. UX-5c: its `p` is `<loop>.<output>` (RS-13: the engine reads the
    // last element's value); its `p[]` is no resource (empty).
    [[nodiscard]] std::string outputResource(std::string_view node, std::string_view port) const;
    // A pass's input edit is sent by the canvas's owner (edit_preset_pass);
    // what became of it is said on the line a graph edit's is.
    void setEditNotice(InspectorNotice notice);

    // --- UX-2: a node's values chosen rather than typed.

    // The whole parameters object of one node, sent as one
    // set_node_parameters (one Undo step). Keys the caller did not touch
    // travel as they were. False when nothing changed or no edit can go out.
    bool setNodeParameters(std::string_view scope, const std::string &node,
                           const nlohmann::json &parameters);

    // --- AB-1s: a sound's analysis baked into a table (engine AB-1).

    // Decision 3: what Use in a shader placed -- the persistent image the
    // table starts (initial.asset), the vec4 buffer of its layout, the two
    // render.scalar sinks that fill it (rows_per_feature, sample_count,
    // hop_seconds from the layout; scene_time from the scope's clock), and,
    // when `pass` was given, the pass ports they were connected to (empty
    // when its shader declares no binding for one; `note` says which).
    struct SampleTableShaderPlacement {
        std::string image;
        std::string buffer;
        std::string values;
        std::string time;
        std::string image_port;
        std::string buffer_port;
        std::string note;
    };
    // The one edit: the nodes, and the pass's two ports and lines. Empty
    // when the table's asset is empty or the scope cannot take nodes.
    // UX-13a, decision 1: the layout's port is chosen by the name the shader
    // reads it as (pelican_resource_<port>), never by a binding number: the
    // reflected pelican_resource_ buffers, then `shader_ports`, the names the
    // shader's text reads through pelican_resource_ports.glsl
    // (shaderResourcePortNames), which no reflection shows before the port
    // is there. The one named <stem>_layout first, else the first no port
    // takes yet.
    [[nodiscard]] nlohmann::json sampleTableShaderOps(std::string_view scope, const std::string &asset,
                                                      std::uint32_t rows_per_feature,
                                                      std::uint32_t sample_count, double hop_seconds,
                                                      const std::string &pass,
                                                      SampleTableShaderPlacement *placed,
                                                      const std::vector<std::string> &shader_ports = {}) const;
    bool addSampleTableShader(std::string_view scope, const std::string &asset,
                              std::uint32_t rows_per_feature, std::uint32_t sample_count,
                              double hop_seconds, const std::string &pass,
                              SampleTableShaderPlacement *placed,
                              const std::vector<std::string> &shader_ports = {});
    // Decision 4: the table a node reads -- a value.sample_table's asset or
    // a render.image's initial.asset -- without its project://; empty for
    // any other node.
    [[nodiscard]] std::string sampleTableAssetOf(std::string_view scope, std::string_view node) const;
    // The root's value.sample_table nodes reading `asset`.
    [[nodiscard]] std::vector<std::string> sampleTableReaders(std::string_view asset) const;
    // get_render_authoring_context's render_target_vocabulary: the image
    // formats and usage flags the engine's parser accepts. Known once a
    // context carrying it arrived; an engine without it leaves both empty.
    [[nodiscard]] bool renderTargetVocabularyKnown() const;
    [[nodiscard]] std::vector<std::string> renderTargetFormats() const;
    [[nodiscard]] std::vector<std::string> renderTargetUsages() const;
    // Ask for the context once on this connection when none has arrived
    // yet (the plan asks for it only when the rendering section has a pass
    // of its own); a context without the vocabulary is not asked again. The
    // request goes out with the next dispatch.
    void askRenderTargetVocabulary();
    // The catalog's image types, in its order: the legacy one after the rest.
    [[nodiscard]] std::vector<GraphCanvasImageType> imageTypes() const;
    [[nodiscard]] std::optional<GraphCanvasImageType> imageType(std::string_view semantic) const;
    // "pelican.render.color_signal@1" -> "Color signal".
    [[nodiscard]] static std::string semanticLabel(std::string_view semantic);
    // D16_UNORM, D24_UNORM_S8_UINT, D32_SFLOAT: a depth format.
    [[nodiscard]] static bool depthFormat(std::string_view format);
    // The formats a render.image with these parameters is offered: the
    // engine's depth formats for a depth type, the others for the rest.
    // Empty without the vocabulary.
    [[nodiscard]] std::vector<std::string> imageFormatChoices(const nlohmann::json &parameters) const;
    // `parameters` with the image's type made `semantic`: the arguments it
    // shares with the old type kept where the new one takes their value, a
    // required one given its first value; and, when depth changes sides, the
    // format and the attachment usage flag moved with it.
    [[nodiscard]] nlohmann::json imageParametersWithSemantic(const nlohmann::json &parameters,
                                                             std::string_view semantic) const;
    // The Pass Editor's shader list (fullscreenShaderChoices), set by the
    // owner, who knows the project.
    void setShaderChoices(std::vector<std::string> vertex, std::vector<std::string> fragment);
    [[nodiscard]] const std::vector<std::string> &vertexShaderChoices() const noexcept {
        return vertex_shader_choices_;
    }
    [[nodiscard]] const std::vector<std::string> &fragmentShaderChoices() const noexcept {
        return fragment_shader_choices_;
    }
    // Decision 4: what a node's value pill offers to pick, or nothing when
    // the value is typed: a render.image's format and type, a render pass's
    // fragment shader.
    [[nodiscard]] std::vector<GraphCanvasValueChoice>
    valueChoices(std::string_view scope, std::string_view node, std::string_view port) const;

    // --- UX-3: the palette's other render nodes, a render node's place, its
    // ports' settings, and the engine's lists (EL-1) where names were typed.

    // Decision 1. The targets a render.target_ref can name: the authoring
    // context's render_targets, less the graph's own images (a render.image
    // is read by its node). Empty before a context arrives.
    [[nodiscard]] std::vector<GraphCanvasDeclaredTarget> declaredRenderTargets() const;
    // The image type a reference to `declaration` carries.
    [[nodiscard]] static nlohmann::json targetRefType(const nlohmann::json &declaration);
    // The compute shaders a new render.compute is offered (set by the owner,
    // who knows the project); none, and the palette does not offer one.
    void setComputeShaderChoices(std::vector<std::string> choices);
    [[nodiscard]] const std::vector<std::string> &computeShaderChoices() const noexcept {
        return compute_shader_choices_;
    }
    // A render.raster is offered with the engine's shaders the Pass Editor
    // starts from (engine://fullscreen, engine://passthrough), when the
    // owner's shader list has them.
    [[nodiscard]] bool rasterTemplateAvailable() const;
    // render.buffer / render.attribute elements and attribute domains, in the
    // order the combos list them, and the bytes one element takes.
    [[nodiscard]] static const std::vector<std::string> &bufferElements();
    [[nodiscard]] static const std::vector<std::string> &attributeDomains();
    [[nodiscard]] static std::uint64_t bufferElementSize(std::string_view element);

    // Decision 2. The plan's passes a node can be placed before or after
    // (no anchor, no loop copy, not `except`), in the plan's order; the
    // families (pass sets) of the authoring context's graphs.
    [[nodiscard]] std::vector<std::string> placePasses(std::string_view except = {}) const;
    [[nodiscard]] std::vector<std::string> passFamilies() const;
    // The render nodes that show the Place field, and the ones that can take
    // a place (a compute task runs after every pass: graph.insert_on_compute).
    [[nodiscard]] static bool showsPlace(std::string_view op) noexcept;
    [[nodiscard]] static bool takesPlace(std::string_view op) noexcept;
    [[nodiscard]] GraphCanvasPlace place(std::string_view scope, std::string_view node) const;
    // One set_node_parameters: `place` written over the node's insert /
    // canonical_anchor / pass_set, every other key as it was.
    bool setPlace(std::string_view scope, const std::string &node, const GraphCanvasPlace &place);

    // Decision 3. A port of a render node's `ports`, or nothing when the
    // node has no such port.
    [[nodiscard]] std::optional<GraphCanvasPortSettings>
    portSettings(std::string_view scope, std::string_view node, std::string_view port) const;
    // One set_node_parameters with the port's history / sampling /
    // subresource / location as `settings` says (a default leaves the key
    // out); what the port cannot take is not written.
    bool setPortSettings(std::string_view scope, const std::string &node, const std::string &port,
                         const GraphCanvasPortSettings &settings);

    // UX-7, decision 3. A render.raster's depth attachment: the image its
    // `attachment: "depth"` port is connected to ("" for none), and the
    // depth images of the scope it can take (render.image nodes of a depth
    // format or type). setRasterDepth names one of those, "" for none, or
    // kNewRasterDepth for a new D32_SFLOAT image the size of the pass's
    // colour image: one edit (the image, and the pass replaced with its
    // port, edge and output). A pass given a depth tests and writes it
    // unless its raster_state says otherwise; one left without loses its
    // depth keys (the engine refuses depth state with no attachment).
    static constexpr std::string_view kNewRasterDepth = "+new";
    [[nodiscard]] std::string rasterDepth(std::string_view scope, std::string_view node) const;
    [[nodiscard]] std::vector<std::string> rasterDepthChoices(std::string_view scope) const;
    bool setRasterDepth(std::string_view scope, const std::string &node, const std::string &depth);

    // UX-7, decision 5 / UX-12a, decision 2. A parameter an op cannot be
    // placed without (graph.render_node_parameters_missing@1, RS-9): one
    // `required` entry of the catalog's `parameters` (engine CT-3). The
    // palette asks for them in a small dialog and hands them over as the
    // seed's `parameters` (and an edge's `inputs`).
    struct RequiredParameter {
        std::string name;
        // The table's kind (number, integer, bool, string, enum, asset,
        // json...), or what Studio knows the string names: "buffer" (a
        // render.scalar's target: a buffer's name, picked or new), "input"
        // (value.input's name: one of valueInputNamesFor()).
        std::string kind;
        // The table's entry as the catalog gives it.
        nlohmann::json schema = nlohmann::json::object();
        // `alternatives`: one of these instead does; one that is an input
        // port of the op (audio.volume's `voice`) is met by an edge too.
        std::vector<std::string> alternatives;
        std::vector<std::string> edge_alternatives;
    };
    // The clocks the engine hands a value graph (graphinputs.cpp).
    [[nodiscard]] static std::vector<std::string> valueInputNames();
    // The buffers a render.scalar in `scope` can write: the render.buffer
    // nodes of the root and of the scope, by id.
    [[nodiscard]] std::vector<std::string> bufferChoices(std::string_view scope) const;

    // Decision 4. The name Rename proposes for a definition: `<runner>_body`
    // after the first node that runs it, free of every other definition's
    // name; the definition's own name when nothing runs it.
    [[nodiscard]] std::string proposeDefinitionName(std::string_view definition) const;

    // Decision 5. The engine's materials, their tags and the variants any
    // of them has (sorted, each once). All empty when the engine did not list
    // them (an engine before EL-1, or none running): the fields then stay
    // what they were.
    [[nodiscard]] std::vector<GraphCanvasMaterial> materials() const;
    [[nodiscard]] std::vector<std::string> materialTags() const;
    [[nodiscard]] std::vector<std::string> materialVariants() const;
    // A root render.material_pass's material_variant; empty takes it out.
    bool setMaterialPassVariant(const std::string &node, const std::string &variant);

    // UX-12c, decision 3 (engine MP-1). The names a material pass binds as
    // `kind` ("screen_inputs" or "surface_resources"), with the semantic
    // type the target it reads must carry, as the catalog's
    // material_pass_contracts list them (every contract lists the same; each
    // name once, in the catalog's order): {name, source_type}. Empty before
    // the catalog arrives or from an engine that predates MP-1.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    materialPassInputs(std::string_view kind) const;
    // UX-12c, decision 1: a plan read before the first frame after a publish
    // does not say what the material passes drew. The plan is read again on
    // the clock (one read at a time, a bounded number of them) until one of
    // its passes says.
    void followDrawnMaterials();
    // The semantic type id of the render target `target` names, as Studio
    // knows it: a root render.image's or render.target_ref's type, else the
    // authoring context's declaration; empty when none says.
    [[nodiscard]] std::string targetSemantic(std::string_view target) const;

    // --- UX-5b: the Graph's fullscreen pass used as the Pass Editor's is.

    // Decision 1. The catalog's engine_shaders (EL-4), in its order; empty
    // before the catalog arrives or from an engine without the list.
    [[nodiscard]] std::vector<GraphCanvasEngineShader> engineShaders() const;
    // EL-4b: the list is the engine's answer to get_engine_shaders, which
    // takes it seconds; it is asked for only when a shader list is opened
    // (never on connecting), once a connection. An engine whose catalog
    // carries engine_shaders (EL-4) is not asked. The request goes out with
    // the next dispatch; until it is answered the lists are the fixed ones.
    void askEngineShaders();
    [[nodiscard]] std::uint64_t engineShadersRevision() const noexcept {
        return engine_shaders_revision_;
    }
    // UX-5d: whether the engine's list has arrived (or its catalog carried it).
    [[nodiscard]] bool engineShadersKnown() const noexcept { return engine_shaders_known_; }
    [[nodiscard]] std::optional<GraphCanvasEngineShader>
    engineShader(std::string_view reference) const;
    // UX-5d, decision 2: the engine's ray tracing pipelines -- a shader stem
    // the list gives a raygen stage (and a miss and a closesthit), with the
    // resource ports its stages declare (port, descriptor type), in the
    // list's order. Empty until get_engine_shaders is answered.
    [[nodiscard]] std::vector<GraphCanvasRayTracingPipeline> rayTracingPipelines() const;

    // --- EL-5s: a project's compute / ray tracing shader placed from the
    // palette takes its ports from the shader's declarations
    // (get_shader_interface, EL-5).

    // The project's ray tracing stems (shaders/<name>.rgen with its .rmiss
    // and .rchit, as project://shaders/<name>), set by the owner, who knows
    // the project. The palette offers them beside the compute shaders.
    void setProjectRayTracingChoices(std::vector<std::string> choices);
    [[nodiscard]] const std::vector<std::string> &projectRayTracingChoices() const noexcept {
        return project_ray_tracing_choices_;
    }
    // Asks get_shader_interface for a stem (`refresh`: build it again even
    // when the files hash the same -- an include changed). One question per
    // stem in the air; "building" is asked again half a second later, from
    // poll(). Goes out with the next dispatch.
    void askShaderInterface(const std::string &reference, bool refresh = false);
    // What the engine last answered for the stem; nothing before an answer.
    // A refresh in the air keeps the previous answer until the new one.
    [[nodiscard]] std::optional<GraphCanvasShaderInterface>
    shaderInterface(std::string_view reference) const;
    [[nodiscard]] std::uint64_t shaderInterfaceRevision() const noexcept {
        return shader_interface_revision_;
    }
    // A question about the stem is in the air or waits to be asked again.
    [[nodiscard]] bool shaderInterfaceWaiting(std::string_view reference) const;
    // The project stem a render.compute runs: its `shader`, or its ray
    // tracing raygen; empty for an engine:// shader or none.
    [[nodiscard]] static std::string computeShaderStem(const nlohmann::json &parameters);
    // Decision 1 / 2. The ops that make `node` (a new id: add it; an existing
    // render.compute: replace it) run `reference` with the ports its
    // interface declares: a storage image is written into an image of its own
    // (the format the shader declares, else R16G16B16A16_SFLOAT; STORAGE and
    // SAMPLED), connected; a sampled image is an empty socket until a line is
    // dropped on it (the engine refuses a declared port no edge feeds); an
    // existing port of the same name keeps its line, a port the shader no
    // longer declares loses its readers' lines. dispatch.groups_from the
    // first written image (local_size_z 1), else one group; a ray tracing set
    // traces rays_from it. Empty when the interface is not ready or cannot
    // run (a ray tracing set that writes no image).
    [[nodiscard]] nlohmann::json computeShaderOps(std::string_view scope, const std::string &node,
                                                  const std::string &reference) const;
    // Decision 2. The node's Shader picked again: computeShaderOps when the
    // interface is known, else it is asked for and the edit is sent from
    // poll() once it is ready.
    bool setComputeShader(std::string_view scope, const std::string &node, const std::string &reference);
    // The sampled image ports of a compute node's shader that the node does
    // not declare yet: the empty sockets a line can be dropped on.
    [[nodiscard]] std::vector<std::string> computeOpenSockets(std::string_view scope,
                                                              std::string_view node) const;
    // Decision 3. A reflection error's sentence for the node fields: the
    // message's first line, with kReflectionDefinesHint when the message
    // reads like a shader that needs a define (an undeclared identifier,
    // an #error).
    static constexpr std::string_view kReflectionDefinesHint =
        "the shader may need defines the reflection does not pass";
    [[nodiscard]] static std::string reflectionErrorSentence(std::string_view reference,
                                                             std::string_view message);
    // Decision 4. Every compute node's project stem asked again with
    // refresh; an answer whose ports differ from what the canvas had is a
    // change (computePortChanges) and a notice line.
    void refreshComputeShaderInterfaces();
    [[nodiscard]] const std::vector<GraphCanvasPortChange> &computePortChanges() const noexcept {
        return compute_port_changes_;
    }
    // "Update ports": decision 2 for every changed node, in one edit.
    bool updateComputePorts();

    // Decision 4. render_target_vocabulary.format_aspects: "color", "depth",
    // "depth_stencil" or "stencil"; empty for a format the engine did not
    // list (or an engine before it).
    [[nodiscard]] std::string formatAspect(std::string_view format) const;
    // Not a colour format, by the engine's aspects; by the name's first
    // letters (depthFormat) only when the engine did not list them.
    [[nodiscard]] bool isDepthFormat(std::string_view format) const;
    // The material pass socket a root render.image is drawn into, with the
    // formats its contract takes there; nothing for any other image, a pass
    // that declares its own material_outputs, or before the catalog.
    [[nodiscard]] std::optional<GraphCanvasMaterialPassImage>
    materialPassImage(std::string_view scope, std::string_view image) const;
    // The formats the image's Format combo offers: its material pass
    // socket's, else imageFormatChoices.
    [[nodiscard]] std::vector<std::string> imageFormatChoicesFor(std::string_view scope,
                                                                 std::string_view node) const;
    // One edit (one Undo step): the image's format, and -- for a material
    // pass's image -- the formats of its other sockets' images moved to the
    // contract's combination that holds the new one. False when nothing
    // changed or no edit can go out.
    bool setImageFormat(std::string_view scope, const std::string &node, const std::string &format);

    // --- PS-1s (engine PS-1): passes that run once, images that are kept.

    // Decision 1. A pass's or task's `schedule`: "per_frame" (also when the
    // key is absent), "once", or -- a render.compute only -- "per_view".
    // Empty for a node that has none to choose.
    [[nodiscard]] std::string nodeSchedule(std::string_view scope, std::string_view node) const;
    // The images the pass writes (a port with access write / read_write or an
    // attachment, followed through the passes that hand them on) that are
    // render.image nodes of `scope` without `lifetime: "persistent"`.
    [[nodiscard]] std::vector<std::string> transientImagesWrittenBy(std::string_view scope,
                                                                    std::string_view node) const;
    // One edit (one Undo step): the schedule ("per_frame" leaves the key
    // out), and -- decision 2 -- for "once" every transient image it writes
    // made Persistent, which the saved notice then says in one line.
    bool setNodeSchedule(std::string_view scope, const std::string &node,
                         const std::string &schedule);
    // The loop's "Run the body once": true when every pass and task of its
    // bodies runs once, false when one does not; empty for bodies with none.
    [[nodiscard]] std::optional<bool> loopBodyOnce(std::string_view loop) const;
    // One edit over every pass and task of the loop's bodies, as
    // setNodeSchedule for each.
    bool setLoopBodyOnce(const std::string &loop, bool once);
    // Decision 3. What the plan says of a once node's passes; empty when the
    // document does not run it once.
    [[nodiscard]] std::optional<GraphCanvasOnceRun> onceRun(std::string_view scope,
                                                            std::string_view node) const;
    // Its band: `once · ran gen N`, `once · not run yet`; empty for a node
    // that runs every frame.
    [[nodiscard]] std::string onceBand(std::string_view scope, std::string_view node) const;
    // Decision 2: a render.image with `lifetime: "persistent"` (its band
    // says `persistent`).
    [[nodiscard]] bool persistentImage(std::string_view scope, std::string_view node) const;
    // IA-1s, decision 1: the asset a render.image starts from
    // (`initial: {asset}`), empty for none.
    [[nodiscard]] std::string imageInitialAsset(std::string_view scope, std::string_view node) const;
    // IA-1s, decision 5: every initial asset the document's images start
    // from, in the root and in each definition body, as written; sorted, once
    // each. UX-13a, decision 3: and every table a value.sample_table reads
    // (without its project://), which the engine reads again on a reload
    // when it changed (IA-3).
    [[nodiscard]] std::vector<std::string> initialAssets() const;
    // Decision 4. rerun_once {node} (empty: every once node); a node of a
    // loop's body asks for its loop, whose copies the plan has. The plan is
    // read again until the scheduled passes say they ran.
    bool rerunOnce(std::string_view scope, const std::string &node);
    // Whether the plan has a once pass at all (Recompute all once passes).
    [[nodiscard]] bool planHasOncePasses() const;

    // Decision 2. The images a fullscreen template's inputs are filled
    // from, in `scope`, `node` itself left out: each image by its id, with
    // the end of the pass that writes it (its output of the socket's name)
    // when one does, else the image's own `resource`.
    [[nodiscard]] std::vector<GraphCanvasTemplateSource>
    templateSources(std::string_view scope, std::string_view node) const;
    // One edit: the pass draws with `fragment`, asks for the scene lights
    // and `push_constants` when they are set (never turns them off), and
    // reads `connect` in order as numbered inputs after the ones it has.
    bool setPassShaderTemplate(std::string_view scope, const std::string &node,
                               const std::string &fragment, bool uses_light_data,
                               std::string_view push_constants,
                               const std::vector<GraphCanvasTemplateSource> &connect);

    // --- GL-2b: the lower layers on the root canvas (design_graph_layers
    // 3.2, 4 rules 1 and 5).

    // Whether the last plan came from an engine that publishes `origin`.
    [[nodiscard]] bool planOriginAware() const noexcept { return plan_origin_aware_; }
    // Decision 4: where Open in Graph lands for one frame plan node.
    [[nodiscard]] GraphCanvasPlanTarget planNodeTarget(std::string_view plan_node) const;
    // The frame plan nodes drawn under one root canvas node, in plan order.
    [[nodiscard]] std::vector<std::string> planNodesOf(std::string_view canvas_node) const;
    // Decision 2: bands and the Layers section are shown on the real root,
    // while the domain filter shows one domain only (3.3).
    [[nodiscard]] bool layersShown() const;
    // The band under a root node: `×3 copies · 2 barriers · aliased`, the
    // backend when it is not the graph's, `deactivated: <reason> (<feature>)`.
    // Only what differs from the default; empty for nothing to say.
    [[nodiscard]] std::string layerBand(std::string_view canvas_node) const;
    // A root graph node the composition switched off, or null.
    [[nodiscard]] const GraphCanvasDeactivatedNode *deactivatedNode(std::string_view node) const;
    // Decision 3: a root node's Layers section, L1 to L3 in that order.
    [[nodiscard]] std::vector<GraphCanvasLayerRow> layerRows(std::string_view canvas_node) const;
    // What concerns no node: the decisions whose from_nodes is empty and the
    // backend candidates -- the Layers section with nothing picked.
    [[nodiscard]] std::vector<GraphCanvasLayerRow> graphLayerRows() const;
    [[nodiscard]] const std::vector<GraphCanvasPlanDiagnostic> &planDiagnostics() const noexcept {
        return plan_diagnostics_;
    }
    // UX-6b, decision 4: what the plan's unread_output and unused_binding
    // diagnostics say of a root node's passes, once each (a loop's copies
    // say it once). Empty where the bands are not shown.
    [[nodiscard]] std::vector<GraphCanvasPlanNote> planNotes(std::string_view canvas_node) const;
    // Whether a diagnostic is one of those two, which the bands and the
    // details carry rather than the diagnostics line.
    [[nodiscard]] static bool nodeNoteDiagnostic(const GraphCanvasPlanDiagnostic &diagnostic);
    // GL-3a, decision 6: what the verified fold groups decided; the band of
    // the node each lands on says `isolate (fold <group>)`.
    [[nodiscard]] const std::vector<GraphCanvasFoldDecision> &planFoldDecisions() const noexcept {
        return plan_fold_decisions_;
    }
    // The band's words for one root node: the fold decisions that landed on
    // it (a node decision on a pass it made; a resource decision on the
    // node that hands the resource on).
    [[nodiscard]] std::vector<std::string> foldDecisionParts(std::string_view canvas_node) const;

    // Decision 3: into a folded node (a feature's fold or a material pass),
    // read only; leaveScope() comes back out one step at a time.
    bool enterFolded(std::string_view id);
    [[nodiscard]] bool foldedView() const noexcept { return folded_stash_.has_value(); }
    [[nodiscard]] const std::vector<GraphCanvasFoldedStep> &foldedTrail() const noexcept {
        return folded_trail_;
    }
    // Inside a feature's fold whose feature has a template: its decomposed
    // version, read only (the template's nodes and definitions).
    [[nodiscard]] bool canShowDecomposed() const;
    bool showDecomposed();
    // Back to the real document's root, whatever the view.
    void leaveFolded();
    // What the breadcrumb says: "root", the folded steps, then the scopes
    // entered inside the view.
    [[nodiscard]] std::vector<std::string> displayTrail() const;
    // A key for the view on screen that tells a folded view from the root.
    [[nodiscard]] std::string viewKey() const;
    // Decision 4: an edit was tried inside the folded view (cleared on
    // leaving it), and what "Expand to edit…" runs from here.
    [[nodiscard]] bool foldedEditRefused() const noexcept { return folded_refused_; }
    [[nodiscard]] GraphCanvasFoldedExpand foldedExpand() const;
    static constexpr std::string_view foldedEditNotice =
        "This is the engine's folded implementation: expand it to edit.";

    // Decision 5: the domain filter of the root canvas. `render` (with
    // draw.*), `pose`, `value` (with event.*), `audio`, `ui`.
    [[nodiscard]] static const std::vector<std::string> &domainNames();
    // The domain an operation belongs to; empty for graph.* and anything
    // else, which is always shown.
    [[nodiscard]] static std::string opDomain(std::string_view op);
    [[nodiscard]] bool domainShown(std::string_view domain) const;
    // False when nothing changed.
    bool setDomainShown(const std::string &domain, bool shown);
    // Whether the canvas draws this node of `scope`: a root node of a hidden
    // domain is not drawn; a graph.instance and a graph.loop always are, and
    // so is every node of a definition or a folded view.
    [[nodiscard]] bool nodeShown(std::string_view scope, const GraphCanvasNode &node) const;
    // The cut ends of the root's lines to hidden nodes, one per socket and
    // domain.
    [[nodiscard]] std::vector<GraphCanvasDomainStub> domainStubs() const;

    // --- UX-8: a curve onto a shader's value, the curve's keys, the sound
    // nodes' placing (graphcanvasvalues.cpp).

    // Decision 4: the interpolations the engine's value.curve takes
    // (valuegraph's curve keys: linear, step, cubic), the default first.
    [[nodiscard]] static const std::vector<std::string> &curveInterpolations();
    // The keys of a value.curve, as its `keys` holds them; empty for a node
    // that is no curve (or keys the engine would not read).
    [[nodiscard]] std::vector<GraphCanvasCurveKey> curveKeys(std::string_view scope,
                                                             std::string_view node) const;
    // Why the engine would refuse these keys -- none, or times that do not
    // rise -- in one line; empty when it would take them.
    [[nodiscard]] static std::string curveKeysRefusal(const std::vector<GraphCanvasCurveKey> &keys);
    [[nodiscard]] static nlohmann::json curveKeysJson(const std::vector<GraphCanvasCurveKey> &keys);
    // Key `index` moved to (time, value), its time held strictly between its
    // neighbours' (a drag past one stops just short of it).
    [[nodiscard]] static std::vector<GraphCanvasCurveKey>
    curveKeysWithMoved(std::vector<GraphCanvasCurveKey> keys, std::size_t index, double time,
                       double value);
    // A key after `index` (halfway to the next, or one past the last), with
    // the value the curve has there.
    [[nodiscard]] static std::vector<GraphCanvasCurveKey>
    curveKeysWithAdded(std::vector<GraphCanvasCurveKey> keys, std::size_t index);
    // One gesture: the curve's keys replaced (one set_node_parameters, one
    // Undo step). Refused keys are not sent; the edit notice says why.
    bool setCurveKeys(std::string_view scope, const std::string &node,
                      const std::vector<GraphCanvasCurveKey> &keys);

    // Decision 5: value.playback's fields. The wraps the engine takes, the
    // default first; the numbers it reads beside `elapsed` and their
    // defaults (CurvePlaybackSettings).
    [[nodiscard]] static const std::vector<std::string> &playbackWraps();
    [[nodiscard]] static const std::vector<std::pair<std::string, double>> &playbackNumbers();
    // An audio.playback carries the same map but no duration.
    [[nodiscard]] static bool isPlayback(std::string_view op) noexcept;
    // The number a field shows: the document's, else the default.
    [[nodiscard]] double playbackNumber(std::string_view scope, std::string_view node,
                                        std::string_view key) const;
    // An edge drives it instead (its field is read only then).
    [[nodiscard]] bool playbackDriven(std::string_view scope, std::string_view node,
                                      std::string_view key) const;
    // One set_node_parameters with `key` written (a default writes it too:
    // what the field shows is what the document says).
    bool setPlaybackField(std::string_view scope, const std::string &node, const std::string &key,
                          const nlohmann::json &value);

    // Decision 2 (UX-13a, decision 1): a pass's named buffer ports (kind
    // buffer) in the order the engine binds a graph pass's buffer reads: by
    // the name of the buffer each reads (the render.buffer's id; the graph's
    // lowering keeps a pass's reads in a set), after the images -- read i at
    // the pass's buffer_binding_base + i. Two ports reading one buffer are one
    // read. A port with no line (`resource` empty) is none the engine binds;
    // those follow, by name.
    struct BufferPortRead {
        std::string port;
        std::string resource;
        bool operator==(const BufferPortRead &) const = default;
    };
    [[nodiscard]] std::vector<BufferPortRead> bufferPortReads(std::string_view scope,
                                                              std::string_view node) const;
    // Their ports alone, in that order.
    [[nodiscard]] std::vector<std::string> bufferPorts(std::string_view scope,
                                                       std::string_view node) const;
    // The element of the buffer a node hands on (a render.buffer's or
    // render.attribute's own), or empty.
    [[nodiscard]] std::string bufferElementOf(std::string_view scope, std::string_view node) const;
    // A named buffer port `port` on the pass, reading `source_node.source_port`,
    // and the edge into it, in one list -- or the edge moved onto the port
    // already there. `element` is always written (the engine reads it).
    // Empty, with `refusal` saying why, when the source hands on no buffer.
    [[nodiscard]] nlohmann::json bufferPortOps(std::string_view scope, const std::string &node,
                                               const std::string &port,
                                               const std::string &source_node,
                                               const std::string &source_port,
                                               std::string *refusal = nullptr) const;
    bool addBufferPort(std::string_view scope, const std::string &node, const std::string &port,
                       const std::string &source_node, const std::string &source_port);
    // A port name made of a shader's declaration ("Level" stays, "level
    // buffer" becomes level_buffer); "buffer" for none.
    [[nodiscard]] static std::string bufferPortName(std::string_view declaration);
    // What the engine reads a resource port through (shaderresourceinterface's
    // shaderResourcePortVariableName): a buffer the shader declares itself is
    // a port's only under that name.
    static constexpr std::string_view kResourcePortVariablePrefix = "pelican_resource_";
    // The buffers a "+ Buffer port" can read in `scope`: render.buffer and
    // render.attribute ids.
    [[nodiscard]] std::vector<std::string> bufferSources(std::string_view scope) const;

    // Decision 7: the voices a new audio.volume or audio.pitch can drive,
    // the audio.playback nodes of the scope.
    [[nodiscard]] std::vector<std::string> voiceChoices(std::string_view scope) const;

    // Decision 6: why the Timeline cannot read values now, in one line;
    // empty when it can (the root reads without an instance, with Live on).
    [[nodiscard]] std::string timelineReason() const;

    // Decision 10: a new, empty definition (no nodes, no outputs) by name.
    [[nodiscard]] nlohmann::json newDefinitionOps(const std::string &name) const;
    bool addDefinition(const std::string &name);

    // Decision 12 (b): render.scalar's value -- the edge's one number, or 2
    // to 4 numbers it writes itself -- and where it lands in its buffer.
    // `components` is 1 for the edge's scalar.
    [[nodiscard]] std::vector<double> scalarValue(std::string_view scope,
                                                  std::string_view node) const;
    // The sinks writing `target` in `scope`, in the order they tile the
    // buffer (offset, then id), and the offsets that tile it without a gap.
    [[nodiscard]] std::vector<std::pair<std::string, std::int64_t>>
    scalarLayout(std::string_view scope, const std::string &target,
                 const std::map<std::string, std::size_t> &components = {}) const;
    // One edit: the sink's components (1 for the edge, else the numbers),
    // and every sink on the same buffer given the offset that packs it.
    bool setScalarValue(std::string_view scope, const std::string &node,
                        const std::vector<double> &value);
    // And its target: the old buffer's sinks and the new one's packed again.
    bool setScalarTarget(std::string_view scope, const std::string &node,
                         const std::string &target);
    // What a render.scalar can write: the graph's buffers (bufferChoices)
    // and the frame plan's other buffers (a feature's runtime buffer, as
    // bloom.intensity).
    [[nodiscard]] std::vector<std::string> scalarTargets(std::string_view scope) const;
    // UX-13b, decision 2 (engine CT-5): what the picker of a render.scalar's
    // target offers -- the graph's buffers it can write once the same edit
    // makes them so (float elements, no value line on them, a size the edit
    // can set), each with what the edit changes, then the frame plan's
    // other buffers as they are. `node` is the sink (none: a new one) and
    // `components` the floats it writes.
    [[nodiscard]] std::vector<GraphCanvasScalarTarget>
    scalarTargetChoices(std::string_view scope, std::string_view node = {},
                        std::size_t components = 1) const;
    // "<buffer>", or "<buffer> (will be set to <fix>)".
    [[nodiscard]] static std::string scalarTargetLabel(const GraphCanvasScalarTarget &target);
    // The set_node_parameters that make the graph's buffer `target` one the
    // render.scalar sinks writing it can write: host_source runtime_f32 and
    // `bytes` large (a render.attribute's count); none when it already is,
    // when it is not the graph's, or when no edit makes it so.
    [[nodiscard]] nlohmann::json scalarTargetFixOps(std::string_view scope, const std::string &target,
                                                    std::uint64_t bytes) const;
    // The bytes the render.scalar sinks of `scope` write into `target`,
    // `except` one of them, a sink's own count of floats taken from
    // `components` when it is there.
    [[nodiscard]] std::uint64_t scalarTargetBytes(std::string_view scope, const std::string &target,
                                                  std::string_view except = {},
                                                  const std::map<std::string, std::size_t> &components = {}) const;
    // UX-13b, decision 3: the last edit refused as
    // graph.render_scalar_target_incompatible@1; reset by the next commit.
    [[nodiscard]] const std::optional<GraphCanvasScalarTargetFix> &scalarTargetFix() const noexcept {
        return scalar_target_fix_;
    }
    // "Fix: make <target> runtime_f32, <size> bytes".
    [[nodiscard]] static std::string scalarTargetFixLabel(const GraphCanvasScalarTargetFix &fix);
    // The Fix of a graph buffer: one set_node_parameters on it.
    bool fixScalarTarget();
    // UX-13b, decision 4 (engine CT-5): whether every root node that runs
    // `definition` is an instance with an owner (or none runs it) --
    // get_graph_instances' definition_instances. True when the engine does
    // not say.
    [[nodiscard]] bool definitionOwnedInstancesOnly(std::string_view definition) const;
    // UX-13b, decision 1: get_graph_catalog as the choices of a parameter
    // read it -- {"modules": [{"types": [...], "operations": [...]}]}.
    [[nodiscard]] nlohmann::json catalogChoices() const;

    // Decision 13 (a) (engine CT-1) / UX-12a, decision 2 (engine CT-3): the
    // parameters the palette asks for -- the `required` entries of the
    // catalog's `parameters`, less the ones the palette seeds itself (a
    // curve's keys, a voice's sound, a UI sink's node, a render node's row
    // beside the op, a draw step's); none before the catalog -- and the
    // names value.input takes, from the catalog's `input_names`.
    [[nodiscard]] std::vector<RequiredParameter> requiredParametersFor(std::string_view op) const;
    [[nodiscard]] std::vector<std::string> valueInputNamesFor() const;

    // --- UX-12a (engine CT-3): a node's fields from the catalog's table.

    // The op's `parameters` ([{name, kind, required?, alternatives?,
    // default?, enum?, min?, max?, items?, fields?, expression?,
    // description?}]); an empty array when the catalog gives none.
    [[nodiscard]] nlohmann::json operationParameters(std::string_view op) const;
    // `parameters_complete`: false when the op's parser reads more keys than
    // the table lists (render.image, the passes, render.compute).
    [[nodiscard]] bool operationParametersComplete(std::string_view op) const;
    // UX-12e, decision 5 (engine CT-4): where a node of the op may stand --
    // "anywhere", or "owned_instance" (only inside a definition an object's
    // instance runs: pose.*, value.morph_weight). "anywhere" when the
    // catalog does not say.
    [[nodiscard]] std::string operationPlacement(std::string_view op) const;
    // UX-12e, decision 1: get_render_authoring_context, asked for once per
    // reading of the document by the fields whose choices it lists (its
    // passes and targets move with the document), not once per field.
    void askRenderAuthoringForChoices();
    // One top-level entry by name, or nullopt.
    [[nodiscard]] std::optional<nlohmann::json> parameterSchema(std::string_view op,
                                                                std::string_view name) const;
    // The input ports the catalog declares for the op.
    [[nodiscard]] std::vector<std::string> operationInputPorts(std::string_view op) const;
    // Decision 4: the last edit refused as graph.render_node_parameter_invalid@1
    // -- the node, its parameter as the engine names it (`size`,
    // `keys[0].interpolation`), and the sentence; reset by the next commit.
    [[nodiscard]] const std::optional<GraphCanvasParameterRefusal> &parameterRefusal() const noexcept {
        return parameter_refusal_;
    }
};

// UX-8, decision 9: sound files copied into a project's assets/audio. Each
// path is copied under its own name, `_2`, `_3`... when that name is taken.
// Answers the references the copies are named by
// (`project://assets/audio/<name>`) and, in `error`, why one was not copied
// (not a .wav or .ogg, unreadable). Nothing is overwritten.
struct GraphCanvasAudioImport {
    std::vector<std::string> references;
    std::string error;
};
GraphCanvasAudioImport importProjectAudio(const std::filesystem::path &project_root,
                                          const std::vector<std::filesystem::path> &files);

// IA-1s, decision 1: the images a render.image can start from -- the
// project's .ktx2 and .png files (any case), named as `initial.asset` names
// them: relative to the project root ("assets/images/ltc.ktx2"). Dot
// directories are skipped. Sorted; `error` says why there are none.
struct GraphCanvasImageAssetsRead {
    std::vector<std::string> assets;
    std::string error;
};
GraphCanvasImageAssetsRead readProjectImageAssets(const std::filesystem::path &project_root);

// The width and height a KTX2 or PNG file declares in its header; nullopt for
// another file or one that cannot be read.
struct GraphCanvasImageExtent {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool operator==(const GraphCanvasImageExtent &) const = default;
};
std::optional<GraphCanvasImageExtent> readImageAssetExtent(const std::filesystem::path &file);

// IA-1s, decision 4: an image file copied into the project's assets/images,
// under its own name or `_2`, `_3`... when that is taken (nothing is
// overwritten). A .ktx2 or .png is `reference` itself; a .hdr is copied and
// `convert` says what to run: `pelican_cli image ktx2 --hdr <hdr> --out
// <ktx2>`, the .ktx2 being `reference` (a free name too). `error` says why
// nothing was copied.
struct GraphCanvasImageImport {
    std::string reference;
    struct Conversion {
        std::filesystem::path hdr;
        std::filesystem::path ktx2;
    };
    std::optional<Conversion> convert;
    std::string error;
};
GraphCanvasImageImport importProjectImage(const std::filesystem::path &project_root,
                                          const std::filesystem::path &file);

} // namespace PelicanStudio
