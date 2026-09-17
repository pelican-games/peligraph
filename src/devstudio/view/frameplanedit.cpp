#include "frameplanedit.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

Json objectOr(const Json &value, const char *key) {
    if (!value.is_object()) return Json::object();
    const auto found = value.find(key);
    return found != value.end() && found->is_object() ? *found : Json::object();
}

std::string textOr(const Json &value, const char *key) {
    if (!value.is_object()) return {};
    const auto found = value.find(key);
    return found != value.end() && found->is_string()
               ? found->get<std::string>()
               : std::string{};
}

bool auxiliaryPassType(std::string_view type) {
    return type == "debug_draw" || type == "debug_text" || type == "gizmo" ||
           type == "shadow_depth" || type == "velocity" || type == "picking";
}

FramePlanEditRequest refuse(std::string reason) {
    FramePlanEditRequest request;
    request.refusal = std::move(reason);
    return request;
}

// The `input` array of one pass declaration, as a list of names. A declaration
// that never had one reads as empty rather than as broken: adding the first
// input is exactly how a pass acquires the member.
std::vector<std::string> declaredInputs(const Json &declaration) {
    std::vector<std::string> inputs;
    const auto found = declaration.find("input");
    if (found == declaration.end()) return inputs;
    if (found->is_string()) {
        inputs.push_back(found->get<std::string>());
        return inputs;
    }
    if (!found->is_array()) return inputs;
    for (const auto &entry : *found)
        if (entry.is_string()) inputs.push_back(entry.get<std::string>());
    return inputs;
}

std::vector<std::string> declaredReads(const Json &declaration) {
    std::vector<std::string> reads;
    const auto found = declaration.find("reads");
    if (found == declaration.end() || found->is_null()) return reads;
    if (found->is_string()) {
        reads.push_back(found->get<std::string>());
        return reads;
    }
    if (!found->is_array()) return reads;
    for (const auto &entry : *found)
        if (entry.is_string()) reads.push_back(entry.get<std::string>());
    return reads;
}

// Add or drop, and say what happened. The canvas refuses a gesture that would
// change nothing rather than sending an edit that commits the same document:
// a silent no-op looks exactly like a broken drag.
struct ResourceChange {
    std::vector<std::string> values;
    std::vector<std::string> already;
    std::vector<std::string> absent;
    bool changed = false;
};

ResourceChange applyResources(std::vector<std::string> current,
                              const std::vector<std::string> &resources,
                              bool connect) {
    ResourceChange result;
    for (const auto &resource : resources) {
        const auto found = std::ranges::find(current, resource);
        if (connect) {
            if (found != current.end()) {
                result.already.push_back(resource);
                continue;
            }
            current.push_back(resource);
            result.changed = true;
        } else {
            if (found == current.end()) {
                result.absent.push_back(resource);
                continue;
            }
            current.erase(found);
            result.changed = true;
        }
    }
    result.values = std::move(current);
    return result;
}

std::string join(const std::vector<std::string> &values) {
    std::string text;
    for (const auto &value : values) {
        if (!text.empty()) text += ", ";
        text += value;
    }
    return text;
}

// Why this pass cannot be edited, in one sentence that names it. Empty means it
// can. The order matters: a feature that said why outranks the generic answer.
std::string passRefusal(const FramePlanAuthoring &authoring,
                        const FramePlanAuthoringPass &pass) {
    if (pass.preset || pass.feature) return {};
    if (!pass.feature_reason.empty())
        return pass.name + ": " + pass.feature_reason;
    if (pass.provenance_source == "feature")
        return pass.name +
               " comes from a feature: materialize the feature into the "
               "project before editing it here.";
    if (!authoring.preset_forked)
        return pass.name +
               " comes from a shared engine preset: fork the preset into the "
               "project before editing it here.";
    return pass.name +
           " has no editable pass declaration in the project preset or in a "
           "project feature.";
}

// The digest member that names the document this pass really lives in.
void addDigests(Json &params, const FramePlanAuthoring &authoring,
                const FramePlanAuthoringPass &pass) {
    params["base_source_digest"] = authoring.source_digest;
    if (pass.feature) params["base_feature_digest"] = pass.feature_digest;
    else params["base_preset_digest"] = authoring.preset_digest;
}

// Everything edit_preset_pass will accept for this pass type, taken verbatim
// from the declaration the engine reported. edit_preset_pass REPLACES each
// field it is given, so a payload that carried only the field the gesture
// changed would be correct -- and a payload that carried a field it invented
// would silently overwrite the author's. The whole declaration travels because
// that is what makes "one gesture, one RPC, nothing else moved" checkable.
void addDeclarationFields(Json &params, const Json &declaration,
                          std::string_view type) {
    if (type == "material") {
        if (declaration.contains("output")) params["output"] = declaration.at("output");
        return;
    }
    for (const auto *key : {"output", "shader"})
        if (declaration.contains(key)) params[key] = declaration.at(key);
    if (type != "raster") return;
    for (const auto *key : {"draw", "raster_state"})
        if (declaration.contains(key)) params[key] = declaration.at(key);
}

} // namespace

const FramePlanAuthoringPass *FramePlanAuthoring::find(
    const std::string &graph, const std::string &pass) const {
    const auto found = passes.find(std::pair{graph, pass});
    return found == passes.end() ? nullptr : &found->second;
}

std::optional<std::pair<int, std::string>> FramePlanAuthoring::anchorAfter(
    const std::string &graph, const std::string &pass) const {
    const auto candidates = anchors.find(graph);
    if (candidates == anchors.end()) return std::nullopt;
    const std::string token = "after:" + pass;
    const auto found = std::ranges::find(candidates->second, token,
                                         &std::pair<int, std::string>::second);
    if (found == candidates->second.end()) return std::nullopt;
    return *found;
}

FramePlanAuthoring framePlanAuthoringFromContext(const Json &context) {
    FramePlanAuthoring authoring;
    if (!context.is_object()) return authoring;
    authoring.source_digest = textOr(objectOr(context, "source_digest"), "hex");
    const auto preset = objectOr(context, "pipeline_preset");
    authoring.preset_digest = textOr(objectOr(preset, "source_digest"), "hex");
    authoring.preset_forked =
        textOr(preset, "reference").starts_with("project://");
    const auto graphs = context.find("graphs");
    if (graphs == context.end() || !graphs->is_array()) return authoring;
    for (const auto &graph : *graphs) {
        if (!graph.is_object()) continue;
        const auto name = textOr(graph, "name");
        if (name.empty()) continue;
        const auto passes = graph.find("passes");
        if (passes != graph.end() && passes->is_array()) {
            for (const auto &pass : *passes) {
                if (!pass.is_object()) continue;
                FramePlanAuthoringPass row;
                row.graph = name;
                row.name = textOr(pass, "name");
                if (row.name.empty()) continue;
                row.declaration_index =
                    pass.value("declaration_index", std::size_t{0});
                row.declaration = objectOr(pass, "declaration");
                row.preset = pass.value("editable_preset_pass", false);
                row.feature = pass.value("editable_feature_pass", false);
                row.feature_reason = textOr(pass, "feature_edit_reason");
                row.feature_digest =
                    textOr(objectOr(pass, "feature_source_digest"), "hex");
                if (row.feature)
                    row.editable_declaration = objectOr(pass, "feature_declaration");
                else if (row.preset)
                    row.editable_declaration = objectOr(pass, "preset_declaration");
                const auto provenance = objectOr(pass, "provenance");
                row.provenance_source = textOr(provenance, "source");
                row.provider_reference = textOr(provenance, "provider_reference");
                row.managed = pass.value("managed", false);
                authoring.order[name].push_back(row.name);
                authoring.passes.insert_or_assign(std::pair{name, row.name},
                                                  std::move(row));
            }
        }
        const auto candidates = graph.find("anchor_candidates");
        if (candidates == graph.end() || !candidates->is_array()) continue;
        for (const auto &anchor : *candidates) {
            if (!anchor.is_object() || !anchor.contains("position") ||
                !anchor.at("position").is_number_integer())
                continue;
            const auto insert = textOr(anchor, "insert");
            if (insert.empty()) continue;
            authoring.anchors[name].emplace_back(
                anchor.at("position").get<int>(), insert);
        }
    }
    authoring.received = true;
    return authoring;
}

const FramePlanTaskAuthoring::Task *FramePlanTaskAuthoring::find(
    const std::string &name) const {
    const auto found = tasks.find(name);
    return found == tasks.end() ? nullptr : &found->second;
}

FramePlanTaskAuthoring framePlanTaskAuthoringFromCatalog(const Json &catalog) {
    FramePlanTaskAuthoring authoring;
    if (!catalog.is_object()) return authoring;
    authoring.source_digest = textOr(objectOr(catalog, "source_digest"), "hex");
    const auto tasks = catalog.find("tasks");
    if (tasks == catalog.end() || !tasks->is_array()) return authoring;
    for (const auto &task : *tasks) {
        if (!task.is_object()) continue;
        FramePlanTaskAuthoring::Task row;
        row.name = textOr(task, "name");
        if (row.name.empty()) continue;
        row.kind = textOr(task, "kind");
        row.declaration = objectOr(task, "declaration");
        row.digest = textOr(objectOr(task, "source_digest"), "hex");
        row.editable = task.value("editable", false);
        row.read_only_reason = textOr(task, "read_only_reason");
        authoring.tasks.insert_or_assign(row.name, std::move(row));
    }
    authoring.received = true;
    return authoring;
}

FramePlanEditRequest framePlanPassResourceRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass, const std::vector<std::string> &resources,
    bool connect) {
    if (!authoring.received)
        return refuse("The render authoring context has not arrived yet.");
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr)
        return refuse(pass + " is not a pass of " + graph +
                      " in the current authoring context.");
    if (const auto reason = passRefusal(authoring, *row); !reason.empty())
        return refuse(reason);
    const auto &declaration = row->editable_declaration;
    const auto type = textOr(declaration, "type");
    if (auxiliaryPassType(type))
        return refuse(pass + " is a " + type +
                      " pass: the engine gives it no editable input list.");
    if (type != "fullscreen" && type != "raster" && type != "material")
        return refuse(pass + " is a " + (type.empty() ? "typeless" : type) +
                      " pass and has no editable input list.");
    const auto change =
        applyResources(declaredInputs(declaration), resources, connect);
    if (!change.changed)
        return refuse(connect ? join(change.already) + " already reaches " + pass
                              : join(change.absent) + " is not an input of " + pass);

    FramePlanEditRequest request;
    request.method = "edit_preset_pass";
    Json params = Json::object();
    addDigests(params, authoring, *row);
    params["graph"] = graph;
    params["pass_name"] = pass;
    params["input"] = change.values;
    addDeclarationFields(params, declaration, type);
    if (type == "fullscreen" &&
        (!params.contains("output") || !params.contains("shader")))
        return refuse(pass +
                      " has no complete output and shader declaration to send "
                      "back: edit it in the Pass Editor.");
    request.params = std::move(params);
    return request;
}

FramePlanEditRequest framePlanPassShaderRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass, const std::string &stage,
    const std::string &reference) {
    if (!authoring.received)
        return refuse("The render authoring context has not arrived yet.");
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr)
        return refuse(pass + " is not a pass of " + graph +
                      " in the current authoring context.");
    if (const auto reason = passRefusal(authoring, *row); !reason.empty())
        return refuse(reason);
    if (reference.empty())
        return refuse(pass + ": a shader reference must not be empty.");
    const auto &declaration = row->editable_declaration;
    const auto type = textOr(declaration, "type");
    if (type == "material")
        return refuse(pass +
                      " is a material pass: its shaders come from the material "
                      "contract, not from the pass.");
    auto shader = objectOr(declaration, "shader");
    if (!shader.contains(stage))
        return refuse(pass + " declares no " + stage + " shader to replace.");
    if (shader.at(stage) == reference)
        return refuse(pass + " already uses " + reference + " for " + stage);
    shader[stage] = reference;

    FramePlanEditRequest request;
    request.method = "edit_preset_pass";
    Json params = Json::object();
    addDigests(params, authoring, *row);
    params["graph"] = graph;
    params["pass_name"] = pass;
    if (auxiliaryPassType(type)) {
        // The auxiliary form takes output and shader only, and refuses input.
        params["shader"] = std::move(shader);
        request.params = std::move(params);
        return request;
    }
    params["input"] = declaredInputs(declaration);
    addDeclarationFields(params, declaration, type);
    params["shader"] = std::move(shader);
    if (type == "fullscreen" && !params.contains("output"))
        return refuse(pass +
                      " has no complete output declaration to send back: edit "
                      "it in the Pass Editor.");
    request.params = std::move(params);
    return request;
}

FramePlanEditRequest framePlanPassRemoveRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass) {
    if (!authoring.received)
        return refuse("The render authoring context has not arrived yet.");
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr)
        return refuse(pass + " is not a pass of " + graph +
                      " in the current authoring context.");
    if (!row->managed || row->provider_reference.empty()) {
        if (row->preset)
            return refuse(pass +
                          " is declared by the project rendering preset: this "
                          "version cannot delete preset passes.");
        if (row->feature || row->provenance_source == "feature")
            return refuse(pass +
                          " belongs to a feature: this version cannot delete "
                          "feature passes, even after materializing them.");
        return refuse(pass +
                      " is not an authored fragment: only passes this project "
                      "added can be removed here.");
    }
    FramePlanEditRequest request;
    request.method = "remove_authored_pass";
    request.params = Json{{"base_source_digest", authoring.source_digest},
                          {"fragment_reference", row->provider_reference}};
    return request;
}

FramePlanEditRequest framePlanTaskResourceRequest(
    const FramePlanTaskAuthoring &authoring, const std::string &task,
    const std::vector<std::string> &resources, bool connect) {
    if (!authoring.received)
        return refuse("The compute task catalog has not arrived yet.");
    const auto *row = authoring.find(task);
    if (row == nullptr)
        return refuse(task + " is not a task in the current catalog.");
    if (!row->editable)
        return refuse(task + ": " +
                      (row->read_only_reason.empty()
                           ? std::string{"this task cannot be edited here."}
                           : row->read_only_reason));
    const auto change =
        applyResources(declaredReads(row->declaration), resources, connect);
    if (!change.changed)
        return refuse(connect ? join(change.already) + " is already read by " + task
                              : join(change.absent) + " is not read by " + task);
    auto declaration = row->declaration;
    declaration["reads"] = change.values;
    FramePlanEditRequest request;
    request.method = "edit_compute_task";
    request.params = Json{{"base_source_digest", authoring.source_digest},
                          {"base_task_source_digest", row->digest},
                          {"task_name", task},
                          {"task", std::move(declaration)}};
    return request;
}

FramePlanEditRequest framePlanTaskDispatchRequest(
    const FramePlanTaskAuthoring &authoring, const std::string &task,
    const std::string &dispatch_json) {
    if (!authoring.received)
        return refuse("The compute task catalog has not arrived yet.");
    const auto *row = authoring.find(task);
    if (row == nullptr)
        return refuse(task + " is not a task in the current catalog.");
    if (!row->editable)
        return refuse(task + ": " +
                      (row->read_only_reason.empty()
                           ? std::string{"this task cannot be edited here."}
                           : row->read_only_reason));
    auto dispatch = Json::parse(dispatch_json, nullptr, false);
    if (dispatch.is_discarded() || !dispatch.is_object())
        return refuse(task + ": the dispatch must be a JSON object.");
    if (row->declaration.value("dispatch", Json::object()) == dispatch)
        return refuse(task + " already dispatches that way.");
    auto declaration = row->declaration;
    declaration["dispatch"] = std::move(dispatch);
    FramePlanEditRequest request;
    request.method = "edit_compute_task";
    request.params = Json{{"base_source_digest", authoring.source_digest},
                          {"base_task_source_digest", row->digest},
                          {"task_name", task},
                          {"task", std::move(declaration)}};
    return request;
}

FramePlanEditRequest framePlanTaskRemoveRequest(
    const FramePlanTaskAuthoring &authoring, const std::string &task) {
    if (!authoring.received)
        return refuse("The compute task catalog has not arrived yet.");
    const auto *row = authoring.find(task);
    if (row == nullptr)
        return refuse(task + " is not a task in the current catalog.");
    if (!row->editable)
        return refuse(task + ": " +
                      (row->read_only_reason.empty()
                           ? std::string{"this task cannot be removed here."}
                           : row->read_only_reason));
    FramePlanEditRequest request;
    request.method = "remove_compute_task";
    request.params = Json{{"base_source_digest", authoring.source_digest},
                          {"base_task_source_digest", row->digest},
                          {"task_name", task}};
    return request;
}

std::vector<FramePlanInlineField> framePlanInlineFields(
    const FramePlanAuthoring &authoring, const FramePlanTaskAuthoring &tasks,
    const std::string &graph, const std::string &pass,
    const std::string &authored_task) {
    std::vector<FramePlanInlineField> fields;
    if (!authored_task.empty()) {
        const auto *row = tasks.find(authored_task);
        if (row == nullptr || !row->editable) return fields;
        const auto dispatch = objectOr(row->declaration, "dispatch");
        if (dispatch.empty()) return fields;
        fields.push_back({"dispatch", "dispatch", dispatch.dump()});
        return fields;
    }
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr || !passRefusal(authoring, *row).empty()) return fields;
    const auto type = textOr(row->editable_declaration, "type");
    if (type == "material") return fields;
    const auto shader = objectOr(row->editable_declaration, "shader");
    for (const auto *stage : {"vertex", "fragment"}) {
        const auto found = shader.find(stage);
        if (found == shader.end() || !found->is_string()) continue;
        fields.push_back({std::string{"shader."} + stage, stage,
                          found->get<std::string>()});
    }
    return fields;
}

} // namespace PelicanStudio
