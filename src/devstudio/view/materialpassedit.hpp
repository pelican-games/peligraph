#pragma once

#include "frameplanedit.hpp"

#include <string>
#include <vector>

namespace PelicanStudio {

// G-1. "Add material pass..." on the Frame Plan: a material contract decides
// how many color outputs the pass has, what each one holds and which format it
// must be, and the form offers to make the targets that are still missing. As
// in frameplanedit, nothing here sends anything: these functions turn what the
// form holds into the two requests the engine already accepts --
// edit_render_targets (F-1) for the new targets, then add_authored_pass for the
// pass -- or into the sentence that says why there is no request.

// One output of a material pass: a color attachment, or the depth one.
struct MaterialPassSlot {
    // What an object name and a test call it: "albedo", "normal", "material",
    // "world_position", "emissive", "color", "depth".
    std::string id;
    // What a person reads beside the slot.
    std::string role;
    // The name "Create missing targets" gives the target it makes.
    std::string default_name;
    // The format the contract requires of this attachment, and the declaration
    // the target is made with. A G-buffer's albedo and emissive are "scene"
    // targets: their format follows the pipeline's HDR switch exactly as the
    // contract's own table does, so they stay right when the switch changes.
    std::string format;
    std::string format_class;
    std::vector<std::string> usage;
    bool depth = false;
};

struct MaterialPassContractChoice {
    std::string contract;
    std::string label;
};

// The contracts the form offers, in the order it offers them.
const std::vector<MaterialPassContractChoice> &materialPassContracts();

// The contract's outputs: its color attachments in location order, then the
// depth attachment. `hdr` picks the SDR or HDR column of a G-buffer's table.
std::vector<MaterialPassSlot> materialPassSlots(const std::string &contract,
                                                bool hdr);

// Whether the running pipeline renders scene color in HDR: "hdr" is among the
// features it enabled. A context that does not say is read as HDR.
bool materialPassHdr(const FramePlanAuthoring &authoring);

// A pass name nothing in `graph` uses yet, from the contract's own name.
std::string materialPassDefaultName(const FramePlanAuthoring &authoring,
                                    const std::string &graph,
                                    const std::string &contract);

// What the form holds.
struct MaterialPassFormValues {
    std::string graph;
    // An insert token of the context's anchor_candidates.
    std::string insert;
    std::string name;
    std::string contract = "forward_opaque_v1";
    // One target name per slot of materialPassSlots(contract), empty where
    // nothing is chosen yet.
    std::vector<std::string> targets;
    // G-4: Save also sets the project's material_route_default to "forward",
    // in the same change as the pass. Only meaningful while
    // materialPassOffersForwardDefault holds.
    bool forward_default = false;
};

// G-4: whether some material pass of the pipeline takes the deferred_geometry
// route: a deferred_geometry_v1 pass, or a legacy G-buffer pass (which accepts
// every material with a G-buffer shader contract).
bool materialPassPipelineHasDeferredPass(const FramePlanAuthoring &authoring);

// G-4: the form offers "draw this project's materials forward" for a Forward
// opaque pass in a pipeline that has no pass for deferred materials. Without
// it, the engine's built-in materials and every automatic material have no
// pass to be drawn by, and the engine refuses the pass.
bool materialPassOffersForwardDefault(const FramePlanAuthoring &authoring,
                                      const std::string &contract);

// G-4: the notice for a material_route_unserved@1 refusal: which materials
// have no pass, on which route, and what to do about it. Empty for any other
// error.
std::string materialRouteUnservedNotice(const nlohmann::json &error);

// "Create missing targets": every empty slot gets the slot's default name, or
// the default with _2, _3, ... when that name is declared already or another
// slot holds it. Slots that are filled keep what they hold.
std::vector<std::string> materialPassFillMissing(
    const std::vector<std::string> &declared, const std::string &contract,
    std::vector<std::string> targets);

// T-1: what the form's slots hold when it opens or the contract changes, so
// Save works without a click. A slot whose default name is declared already
// with the format the slot needs (its `format`, or no format and its
// `format_class`) reuses that target; every other slot gets a new name as
// Create missing targets would give it.
std::vector<std::string> materialPassInitialTargets(
    const FramePlanAuthoring &authoring, const std::string &contract);

// The slots whose target no render target in the context declares: the ones
// Save makes first.
std::vector<std::size_t> materialPassNewSlots(
    const FramePlanAuthoring &authoring, const MaterialPassFormValues &values);

// Why Save cannot go ahead, in one sentence, or empty when it can.
std::string materialPassFormRefusal(const FramePlanAuthoring &authoring,
                                    const MaterialPassFormValues &values);

// Save's first request: one edit_render_targets whose add ops make every new
// slot's target with the format the contract requires. Neither sendable nor
// refused when nothing is missing -- then the pass goes alone.
FramePlanEditRequest materialPassTargetsRequest(
    const FramePlanAuthoring &authoring, const MaterialPassFormValues &values);

// Save's second request: add_authored_pass with the material pass. Refused
// while a target it names is not declared yet, so it is built from the context
// that arrives after the targets were made.
FramePlanEditRequest materialPassAddRequest(const FramePlanAuthoring &authoring,
                                            const MaterialPassFormValues &values);

} // namespace PelicanStudio
