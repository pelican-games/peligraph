#include "presetpassedit.hpp"

#include "frameplanedit.hpp"
#include "fullscreenpasssettings.hpp"
#include "fullscreenshadertemplate.hpp"
#include "passforms/materialsettingsform.hpp"

namespace PelicanStudio {

using Json = nlohmann::json;

Json presetPassEditParams(const PresetPassEditSubject &subject, const PresetPassEditDraft &edit) {
    const auto &original = edit.original;
    const auto &draft = edit.draft;
    Json params{{"base_source_digest", subject.source_digest},
                {"graph", subject.graph},
                {"pass_name", subject.pass},
                {"ray_query", edit.ray_query}};
    // F-6: the root's own pass is named by base_source_digest alone.
    if (subject.document != PresetPassEditSubject::Document::root)
        params[subject.document == PresetPassEditSubject::Document::feature
                   ? "base_feature_digest"
                   : "base_preset_digest"] = subject.document_digest;
    const auto type = original.value("type", std::string{});
    // A pass that is not fullscreen keeps its declaration: only what the
    // draft changed is sent.
    if (!type.empty() && type != "fullscreen") {
        if (type == "material") {
            auto settings = Json::object();
            for (const auto &key : MaterialSettingsForm::keys()) {
                if (draft.contains(key)) {
                    if (!original.contains(key) || draft.at(key) != original.at(key))
                        settings[key] = draft.at(key);
                } else if (original.contains(key)) {
                    settings[key] = nullptr;
                }
            }
            if (!settings.empty()) params["material_settings"] = std::move(settings);
            if (edit.routing && *edit.routing != edit.original_routing)
                params["material_routing"] = *edit.routing;
        }
        for (const auto *key : {"input", "output", "shader", "draw", "raster_state"}) {
            if (draft.contains(key) && (!original.contains(key) || draft.at(key) != original.at(key)))
                params[key] = draft.at(key);
        }
        if (type == "raster" && original.contains("raster_state") && !draft.contains("raster_state"))
            params["raster_state"] = nullptr;
        if ((type == "velocity" || type == "picking") &&
            original.value("shader", Json::object()).contains("skinned_vertex") &&
            !draft.value("shader", Json::object()).contains("skinned_vertex"))
            params["shader"]["skinned_vertex"] = nullptr;
    } else {
        auto output = original.value("output", Json::object());
        output["color"] = draft.at("output").at("color");
        output["depth"] = draft.at("output").at("depth");
        auto shader = original.value("shader", Json::object());
        // VI-3b: off engine://passthrough, its define means nothing.
        if (shader.value("fragment", std::string{}) == EnginePassthroughFragment &&
            draft.at("shader").value("fragment", std::string{}) != EnginePassthroughFragment)
            shader = framePlanShaderWithoutPassthroughInput(std::move(shader));
        shader["vertex"] = draft.at("shader").at("vertex");
        shader["fragment"] = draft.at("shader").at("fragment");
        // UX-4a: the defines are the form's; moving off engine://passthrough
        // still drops its input define (VI-3b).
        if (const auto defines = draft.at("shader").find("defines");
            defines != draft.at("shader").end())
            shader["defines"] = *defines;
        else
            shader.erase("defines");
        if (shader.value("fragment", std::string{}) != EnginePassthroughFragment &&
            original.value("shader", Json::object()).value("fragment", std::string{}) ==
                EnginePassthroughFragment)
            shader = framePlanShaderWithoutPassthroughInput(std::move(shader));
        params["input"] = draft.at("input");
        params["output"] = std::move(output);
        params["shader"] = std::move(shader);
        // The interface fields and clear_colors, each only when it changed.
        fullscreenInterfaceEditParams(original, draft, params);
        const auto clear = draft.value("clear_colors", Json(nullptr));
        if (clear != original.value("clear_colors", Json(nullptr))) params["clear_colors"] = clear;
    }
    if (edit.view_select) params["view_select"] = *edit.view_select;
    return params;
}

Json presetPassRayQuery(const Json &declaration) {
    const auto query = declaration.is_object() ? declaration.value("ray_query", Json(false)) : Json(false);
    if (query.is_object())
        return Json{{"acceleration_structure",
                     query.value("acceleration_structure", std::string{"scene"})}};
    if (query.is_boolean() && query.get<bool>())
        return Json{{"acceleration_structure", "scene"}};
    return false;
}

std::optional<PresetPassEditSubject> presetPassEditSubject(const FramePlanAuthoring &authoring,
                                                           const std::string &graph,
                                                           const std::string &pass) {
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr || !(row->root || row->preset || row->feature)) return std::nullopt;
    PresetPassEditSubject subject{.source_digest = authoring.source_digest, .graph = graph, .pass = pass};
    if (row->root) {
        // F-6: base_source_digest alone.
    } else if (row->feature) {
        subject.document = PresetPassEditSubject::Document::feature;
        subject.document_digest = row->feature_digest;
    } else if (row->preset) {
        subject.document = PresetPassEditSubject::Document::preset;
        subject.document_digest = authoring.preset_digest;
    }
    return subject;
}

} // namespace PelicanStudio
