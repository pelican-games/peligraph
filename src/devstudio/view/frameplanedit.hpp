#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
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

// One compute or ray-tracing task of get_compute_task_authoring.
struct FramePlanTaskAuthoring {
    struct Task {
        std::string name;
        std::string kind;
        nlohmann::json declaration = nlohmann::json::object();
        std::string digest;
        bool editable = false;
        std::string read_only_reason;
    };
    bool received = false;
    std::string source_digest;
    std::map<std::string, Task> tasks;

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

} // namespace PelicanStudio
