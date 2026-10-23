#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace PelicanStudio {

struct FramePlanAuthoring;

// GL-5a, decisions 2 and 4. The edit_preset_pass one change of a pass's
// declaration makes, built in one place: the Pass Editor's Save (which the
// Frame Plan's Edit pass opens) and the Graph's detail column beside a folded
// pass of the rendering section both call presetPassEditParams, so the same
// change sends the same params from either.

// Which declaration is written and the digests that name it.
struct PresetPassEditSubject {
    // Where the pass is declared. The root's own pass (F-6) is named by
    // base_source_digest alone; a preset's or a feature's pass carries that
    // document's digest too.
    enum class Document { root, preset, feature };

    std::string source_digest;
    std::string graph;
    std::string pass;
    Document document = Document::root;
    // base_preset_digest / base_feature_digest (unused for the root).
    std::string document_digest;
};

// The declaration before and after the forms, and what travels beside it.
struct PresetPassEditDraft {
    // The editable declaration the forms were loaded from.
    nlohmann::json original = nlohmann::json::object();
    // The declaration the forms give back. A fullscreen pass's is built from
    // the forms alone (name, type, input, output, shader, the interface
    // fields, clear_colors); any other type's is the original with the
    // forms' keys written over it.
    nlohmann::json draft = nlohmann::json::object();
    // ray_query, sent with every edit.
    nlohmann::json ray_query = false;
    // A preset's material pass: material_routing as the form has it, sent
    // when it differs from the original's. Left empty for the root's and a
    // feature's pass, whose edit writes no routing.
    std::optional<nlohmann::json> routing;
    nlohmann::json original_routing = nullptr;
    // LP-4b: view_select when the View field changed it (null removes it).
    std::optional<nlohmann::json> view_select;
};

// edit_preset_pass's params. A fullscreen pass sends its input, output and
// shader whole (the original's other output and shader fields kept), the
// interface fields and clear_colors only when they changed; any other type
// sends only the keys the draft changed (material_settings for a material
// pass's ten settings, null for one taken away).
nlohmann::json presetPassEditParams(const PresetPassEditSubject &subject,
                                    const PresetPassEditDraft &edit);

// ray_query as the Pass Editor loads it from a declaration and sends it back
// unchanged: false, or the acceleration structure it names ("scene" for a
// bare true).
nlohmann::json presetPassRayQuery(const nlohmann::json &declaration);

// The subject of a pass the authoring context lists, or nullopt when it
// lists none, or one that is not editable.
std::optional<PresetPassEditSubject> presetPassEditSubject(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass);

} // namespace PelicanStudio
