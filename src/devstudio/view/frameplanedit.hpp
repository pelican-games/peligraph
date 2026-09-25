#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PelicanStudio {

// N-8. The Frame Plan canvas is an editing surface now, and every gesture on it
// finishes as one of the authoring RPCs that already exist. Nothing here sends
// anything: these functions turn "the user dropped this resource on that pass"
// into the exact request the engine already accepts, or into the one line that
// says why it will not be sent. Keeping that decision here is what lets a test
// read the payload without driving a dialog.

// One request the canvas wants to make, or one refusal. A refusal is not an
// error: it is the sentence shown under the toolbar, and it always names the
// pass or task it is about, because a canvas gesture has no other label.
struct FramePlanEditRequest {
    std::string method;
    nlohmann::json params;
    std::string refusal;
    // Q-12: the status line, when it is not the refusal's first sentence.
    // The engine's two-sentence reason for a graph node's task is the line
    // as a whole; the refusal (the tooltip) names the task and the way there.
    std::string line;

    [[nodiscard]] bool sendable() const noexcept {
        return refusal.empty() && !method.empty();
    }
};

// One pass row of get_render_authoring_context, joined on (graph, pass name).
struct FramePlanAuthoringPass {
    std::string graph;
    std::string name;
    std::size_t declaration_index = 0;
    // What the engine composed, and what the document the engine would write
    // actually holds. They differ: the composed one carries defaults.
    nlohmann::json declaration = nlohmann::json::object();
    nlohmann::json editable_declaration = nlohmann::json::object();
    bool preset = false;
    bool feature = false;
    // F-6: declared by a direct root itself; edit_preset_pass then takes no
    // document digest but the root's own.
    bool root = false;
    std::string feature_reason;
    std::string feature_digest;
    std::string provenance_source;
    std::string provider_reference;
    bool managed = false;
};

// F-1. One row of get_render_authoring_context.render_targets[]: a composed
// render target, where it was declared, and whether edit_render_targets may
// change it. `authored` is the fragment's own declaration -- the one a replace
// starts from -- and is empty for anything the editor does not own.
struct FramePlanRenderTarget {
    std::string name;
    nlohmann::json declaration = nlohmann::json::object();
    nlohmann::json authored = nlohmann::json::object();
    bool editable = false;
    // "authoring" | "root" | "preset" | "feature:<reference>" | "engine" |
    // "composed"
    std::string source;
};

struct FramePlanAuthoring {
    bool received = false;
    std::string source_digest;
    std::string preset_digest;
    // The project forked the preset: without this, editing a preset pass is
    // refused by the engine, and the canvas should say so before sending.
    bool preset_forked = false;
    // F-4: the root declares its passes itself (`config_kind` "direct", as a
    // Blank pipeline project does). There is no preset to fork; F-6: the passes
    // the root declares are edited in the root (FramePlanAuthoringPass::root).
    bool direct = false;
    std::string source_reference;
    std::map<std::pair<std::string, std::string>, FramePlanAuthoringPass> passes;
    // graph -> the pass names in declaration order, and the insert tokens the
    // engine offers. "Add pass after this node" is a position in these.
    std::map<std::string, std::vector<std::string>> order;
    std::map<std::string, std::vector<std::pair<int, std::string>>> anchors;
    // F-1: the render targets, and the vocabulary the engine's parser accepts
    // for them. The form offers exactly these values.
    std::map<std::string, FramePlanRenderTarget> render_targets;
    std::vector<std::string> render_target_formats;
    std::vector<std::string> render_target_usages;
    std::string preset_name;
    // G-1: the features the running pipeline enabled ("hdr" decides a
    // G-buffer's formats). Unknown when the context predates the member.
    std::vector<std::string> enabled_features;
    bool enabled_features_known = false;

    [[nodiscard]] const FramePlanRenderTarget *findRenderTarget(
        const std::string &name) const;

    [[nodiscard]] const FramePlanAuthoringPass *find(
        const std::string &graph, const std::string &pass) const;
    // The insert token that puts a new pass immediately after `pass`, and the
    // anchor position that names it. Empty when the graph has no such anchor.
    [[nodiscard]] std::optional<std::pair<int, std::string>> anchorAfter(
        const std::string &graph, const std::string &pass) const;
};

FramePlanAuthoring framePlanAuthoringFromContext(const nlohmann::json &context);

// Q-12 (MG-3b): where the render.compute node that made a task is authored.
// `scope` is the token the graph edit ops take ("root", or the definition
// whose body holds the node), `node` the id written in that scope, and
// `instance` the instance path that placed it ("" in the root scope, no
// trailing "/").
struct FramePlanGraphNodeRef {
    std::string scope;
    std::string node;
    std::string instance;

    friend bool operator==(const FramePlanGraphNodeRef &,
                           const FramePlanGraphNodeRef &) = default;
};

// One compute or ray-tracing task of get_compute_task_authoring.
struct FramePlanTaskAuthoring {
    struct Task {
        std::string name;
        std::string kind;
        nlohmann::json declaration = nlohmann::json::object();
        std::string digest;
        bool editable = false;
        std::string read_only_reason;
        // Q-10: where the declaration lives, and the feature that provides
        // it (empty for anything no feature provides).
        std::string source_reference;
        std::string provider_feature;
        // Q-12: MG-3b's origin ("" for a catalog that predates it), and the
        // node that made the task when the origin is "graph_node".
        std::string origin;
        std::optional<FramePlanGraphNodeRef> graph_node;
    };
    bool received = false;
    std::string source_digest;
    std::map<std::string, Task> tasks;
    // Q-10: the rendering document's own reference, from the authoring
    // context. The catalog does not carry it; the canvas fills it in.
    std::string root_reference;

    [[nodiscard]] const Task *find(const std::string &name) const;
};

FramePlanTaskAuthoring framePlanTaskAuthoringFromCatalog(
    const nlohmann::json &catalog);

// --- The gestures.

// Connect (or disconnect) resources on a pass: the whole current declaration
// goes back with `input` changed and nothing else touched, because
// edit_preset_pass replaces the fields it is given rather than merging them.
FramePlanEditRequest framePlanPassResourceRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass, const std::vector<std::string> &resources,
    bool connect);

// One shader stem of an editable pass -- what the in-place band edits. The
// other fields still travel, for the same reason.
FramePlanEditRequest framePlanPassShaderRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass, const std::string &stage,
    const std::string &reference);

// Q-14. Make a pass-through shader: a fullscreen pass's input list and its
// fragment shader, changed together in one edit_preset_pass, because either
// one alone is refused by the engine's input contract. `inputs` is the whole
// new list; every other field travels as the declaration holds it.
FramePlanEditRequest framePlanPassInputsAndShaderRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass, const std::vector<std::string> &inputs,
    const std::string &fragment);

// Delete a node. Only an authored fragment can go; everything else is refused
// by name, because the engine has no delete for it and inventing one here
// would be a second way to author.
FramePlanEditRequest framePlanPassRemoveRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass);

FramePlanEditRequest framePlanTaskResourceRequest(
    const FramePlanTaskAuthoring &authoring, const std::string &task,
    const std::vector<std::string> &resources, bool connect);

FramePlanEditRequest framePlanTaskDispatchRequest(
    const FramePlanTaskAuthoring &authoring, const std::string &task,
    const std::string &dispatch_json);

FramePlanEditRequest framePlanTaskRemoveRequest(
    const FramePlanTaskAuthoring &authoring, const std::string &task);

// --- Q-10: a task at the rendering document's root.

// MG-2b: rendering is a pelican.graph root, and that root's `rendering`
// section cannot declare compute_tasks (the lowering derives them from
// render.compute nodes). A task the catalog attributes to the root document
// itself -- no feature provides it -- therefore has no declaration Studio can
// write back, and the engine answers edit_compute_task / remove_compute_task
// for it with this code.
inline constexpr std::string_view FramePlanRootTaskCode = "compute_task_not_editable";

// The catalog row's source is the root document (the `source_reference` of
// get_render_authoring_context or get_render_features) and no feature
// provides it. An unknown root reference answers false: nothing is claimed
// about a task whose place Studio has not been told.
[[nodiscard]] bool framePlanTaskAtRoot(std::string_view source_reference,
                                       std::string_view provider_feature,
                                       std::string_view root_reference);

// What is said instead: one sentence for the status line (what to do), then
// one that carries the code.
[[nodiscard]] std::string framePlanRootTaskRefusal(std::string_view task);

// --- Q-12: a task a render.compute node made (MG-3b).

// get_compute_task_authoring says where each task came from: `origin` is
// "feature" | "graph_node" | "preset" | "root". A node's task is listed
// against the root document even when the root names a preset, so the Q-10
// test above would call it a root task; the origin is read first.
inline constexpr std::string_view FramePlanGraphNodeTaskReason =
    "This task is made by a render.compute node in the graph. Edit the node "
    "in the Graph tab.";

// The row's origin, or "" for a row that carries none (an engine before
// MG-3b).
[[nodiscard]] std::string framePlanTaskOrigin(const nlohmann::json &row);
// The node that made the task: only for origin "graph_node", and only when
// the row names a scope and a node (a missing instance reads as "").
[[nodiscard]] std::optional<FramePlanGraphNodeRef> framePlanTaskGraphNode(
    const nlohmann::json &row);

// Why a task cannot be edited here, decided by origin first:
//   graph_node -- the node's task, whatever else the row says;
//   root       -- origin "root", or no origin and the Q-10 test holds;
//   read_only  -- the row says it is not editable;
//   none       -- it can be edited.
enum class FramePlanTaskLock { none, graph_node, root, read_only };
[[nodiscard]] FramePlanTaskLock framePlanTaskLock(
    std::string_view origin, bool editable, std::string_view source_reference,
    std::string_view provider_feature, std::string_view root_reference);

// The line a node's task gets: the engine's read_only_reason as it wrote it,
// or FramePlanGraphNodeTaskReason when the row carries none.
[[nodiscard]] std::string framePlanGraphNodeTaskReason(
    std::string_view read_only_reason);

// Q-10. What the Task Editor sends for Apply / Create task. The decision lives
// here so a test can read it without the dialog: a new task still goes out as
// add_compute_task into the project:// feature it names, whatever the catalog
// holds, and an edit of a root task is refused before anything is sent.
struct ComputeTaskEditorSubmit {
    bool creating = false;
    std::string destination_reference;
    std::string base_source_digest;
    // Editing: the catalog row the editor shows.
    std::string task_name;
    std::string task_source_digest;
    std::string source_reference;
    std::string provider_feature;
    bool editable = false;
    std::string root_reference;
    // Q-12: the row's origin and read_only_reason. A node's task is refused
    // with the engine's reason before the Q-10 root test is asked.
    std::string origin;
    std::string read_only_reason;
};

FramePlanEditRequest computeTaskEditorRequest(const ComputeTaskEditorSubmit &submit,
                                              const nlohmann::json &task);

// --- The in-place bands a node offers.

struct FramePlanInlineField {
    // The field id travels on the item so a test can name the band without
    // reading its text: "shader.vertex", "shader.fragment", "dispatch".
    std::string field;
    std::string label;
    std::string value;
};

// What the given node lets the user retype on the canvas. A pass that cannot
// be edited offers nothing: a band that refuses on Enter would be a button
// that looks like an invitation.
std::vector<FramePlanInlineField> framePlanInlineFields(
    const FramePlanAuthoring &authoring, const FramePlanTaskAuthoring &tasks,
    const std::string &graph, const std::string &pass,
    const std::string &authored_task);

// --- F-1: render targets.

// What the New render target form holds. The defaults are the answer to "I
// only typed a name": a scene color target the size of the screen that a pass
// can draw into and another can sample.
struct RenderTargetFormValues {
    std::string name;
    // "scene" | "display" | "data" | "explicit"
    std::string kind = "scene";
    std::string format = "R16G16B16A16_SFLOAT";
    bool fixed_size = false;
    double extent_scale = 1.0;
    std::uint32_t width = 256;
    std::uint32_t height = 256;
    std::vector<std::string> usage{"COLOR_ATTACHMENT", "SAMPLED"};
    bool history = false;
    std::optional<std::array<double, 4>> clear_color;
};

// The form filled from a declaration, for the replace path and the read-only
// view of a target the editor does not own.
RenderTargetFormValues renderTargetFormValues(const nlohmann::json &declaration);

// The declaration the form sends. `base` is the fragment's current one on a
// replace, so the fields the form does not show (mip_levels, dimension, ...)
// travel unchanged. A fixed size keeps `extent_scale: 1` beside width/height:
// the engine's parser requires the member even when the fixed extent wins.
nlohmann::json renderTargetDeclaration(
    const RenderTargetFormValues &values,
    const nlohmann::json &base = nlohmann::json::object());

FramePlanEditRequest framePlanRenderTargetAddRequest(
    const FramePlanAuthoring &authoring, const RenderTargetFormValues &values);
FramePlanEditRequest framePlanRenderTargetReplaceRequest(
    const FramePlanAuthoring &authoring, const std::string &name,
    const RenderTargetFormValues &values);
// A target the editor does not own makes no RPC: the refusal names where it
// was declared, because that is the only place it can be deleted.
FramePlanEditRequest framePlanRenderTargetRemoveRequest(
    const FramePlanAuthoring &authoring, const std::string &name);

// Where a target came from, in one line a person reads ("the screen output the
// engine provides", "a target of preset hybrid_v1").
std::string framePlanRenderTargetOrigin(const std::string &source,
                                        const std::string &name,
                                        const std::string &preset_name = {});

// The notice line for an edit_render_targets rejection, chosen by its code.
// A referenced target lists the passes and tasks still using it.
std::string framePlanRenderTargetRejection(
    const nlohmann::json &error, const std::string &preset_name = {});

// G-6: the line an engine refusal of a render edit gets where it is seen (not
// only in a tooltip): which graph variant refused it when one did, the code,
// then the engine's message --
// "The preview graph refused it: render_pipeline_preflight_failed: ...".
// `error` is the rejection's `error` object.
std::string renderAuthoringRefusalLine(const nlohmann::json &error);

} // namespace PelicanStudio
