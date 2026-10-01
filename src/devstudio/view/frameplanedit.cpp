#include "frameplanedit.hpp"

#include "fullscreenshadertemplate.hpp"
#include "inspectormodel.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <set>
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
    if (pass.preset || pass.feature || pass.root) return {};
    if (!pass.feature_reason.empty())
        return pass.name + ": " + pass.feature_reason;
    if (pass.provenance_source == "feature")
        return pass.name +
               " comes from a feature: materialize the feature into the "
               "project before editing it here.";
    // F-4: a direct root selects no preset, so there is nothing to fork. F-6:
    // the passes it declares are edited in it (pass.root, above); what is left
    // here is a declaration the engine did not offer as editable.
    if (authoring.direct && pass.provenance_source == "project")
        return pass.name + " is declared in the project's own rendering " +
               "config" +
               (authoring.source_reference.empty()
                    ? std::string{}
                    : " (" + authoring.source_reference + ")") +
               ", which selects no preset, and the engine offers no editable "
               "declaration of it. Edit that file by hand.";
    if (authoring.direct)
        return pass.name + " is added by the engine; it has no declaration "
                           "in the project to edit.";
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
    // F-6: the root's own pass -- base_source_digest already names that file.
    if (pass.root) return;
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
    authoring.direct = textOr(context, "config_kind") == "direct";
    authoring.source_reference = textOr(context, "source_reference");
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
                row.root = pass.value("editable_root_pass", false);
                if (row.feature)
                    row.editable_declaration = objectOr(pass, "feature_declaration");
                else if (row.preset)
                    row.editable_declaration = objectOr(pass, "preset_declaration");
                else if (row.root)
                    row.editable_declaration = objectOr(pass, "root_declaration");
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
    authoring.preset_name = textOr(preset, "name");
    // G-1: the features the running pipeline enabled, when the context says.
    if (const auto features = context.find("enabled_feature_names");
        features != context.end() && features->is_array()) {
        authoring.enabled_features_known = true;
        for (const auto &feature : *features)
            if (feature.is_string())
                authoring.enabled_features.push_back(feature.get<std::string>());
    }
    if (const auto targets = context.find("render_targets");
        targets != context.end() && targets->is_array()) {
        for (const auto &target : *targets) {
            FramePlanRenderTarget row;
            row.name = textOr(target, "name");
            if (row.name.empty()) continue;
            row.declaration = objectOr(target, "declaration");
            row.authored = objectOr(target, "authored_declaration");
            row.editable = target.value("editable", false);
            row.source = textOr(target, "source");
            authoring.render_targets.insert_or_assign(row.name, std::move(row));
        }
    }
    const auto vocabulary = objectOr(context, "render_target_vocabulary");
    for (const auto &[field, into] :
         {std::pair{"formats", &authoring.render_target_formats},
          std::pair{"usage", &authoring.render_target_usages}}) {
        const auto values = vocabulary.find(field);
        if (values == vocabulary.end() || !values->is_array()) continue;
        for (const auto &value : *values)
            if (value.is_string()) into->push_back(value.get<std::string>());
    }
    authoring.received = true;
    return authoring;
}

const FramePlanRenderTarget *FramePlanAuthoring::findRenderTarget(
    const std::string &name) const {
    const auto found = render_targets.find(name);
    return found == render_targets.end() ? nullptr : &found->second;
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
        row.source_reference = textOr(task, "source_reference");
        row.provider_feature = textOr(task, "provider_feature");
        row.origin = framePlanTaskOrigin(task);
        row.graph_node = framePlanTaskGraphNode(task);
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

FramePlanEditRequest framePlanPassInputsAndShaderRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass, const std::vector<std::string> &inputs,
    const std::string &fragment) {
    if (!authoring.received)
        return refuse("The render authoring context has not arrived yet.");
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr)
        return refuse(pass + " is not a pass of " + graph +
                      " in the current authoring context.");
    if (const auto reason = passRefusal(authoring, *row); !reason.empty())
        return refuse(reason);
    if (fragment.empty())
        return refuse(pass + ": a shader reference must not be empty.");
    const auto &declaration = row->editable_declaration;
    if (textOr(declaration, "type") != "fullscreen")
        return refuse(pass + " is not a fullscreen pass: its shader does not "
                             "read the pass's input list.");
    auto shader = objectOr(declaration, "shader");
    if (!declaration.contains("output") || !shader.contains("vertex"))
        return refuse(pass +
                      " has no complete output and shader declaration to send "
                      "back: edit it in the Pass Editor.");
    shader["fragment"] = fragment;

    FramePlanEditRequest request;
    request.method = "edit_preset_pass";
    Json params = Json::object();
    addDigests(params, authoring, *row);
    params["graph"] = graph;
    params["pass_name"] = pass;
    params["input"] = inputs;
    addDeclarationFields(params, declaration, "fullscreen");
    params["shader"] = std::move(shader);
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

// --- VI-3a: sockets from the shader's declarations, and connecting by number.

namespace {

constexpr std::string_view HistorySuffix = "@history";

struct PassInput {
    std::size_t index = 0; // position in `input`
    std::string resource;  // without "@history"
    bool history = false;
};

// The pass's inputs split the way the engine numbers them: texture inputs
// (history ones included) count from binding 0, buffer inputs from the
// buffer binding base.
struct SplitInputs {
    std::vector<PassInput> textures;
    std::vector<PassInput> buffers;
};

std::pair<std::string, bool> bareResource(const std::string &input) {
    if (input.size() > HistorySuffix.size() &&
        input.ends_with(HistorySuffix))
        return {input.substr(0, input.size() - HistorySuffix.size()), true};
    return {input, false};
}

bool bufferResource(const FramePlanModel &model, const std::string &name) {
    const auto found =
        std::ranges::find(model.resources, name, &FramePlanResource::name);
    return found != model.resources.end() && found->kind == "buffer";
}

SplitInputs splitInputs(const FramePlanModel &model,
                        const std::vector<std::string> &inputs) {
    SplitInputs split;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        auto [resource, history] = bareResource(inputs[index]);
        const bool buffer = !history && bufferResource(model, resource);
        (buffer ? split.buffers : split.textures)
            .push_back(PassInput{index, std::move(resource), history});
    }
    return split;
}

// Without the authoring context the frame plan's reads stand in: the planner
// appends a pass's texture inputs, then its buffer inputs, in `input` order.
std::vector<std::string> readsAsInputs(const FramePlanModel &model,
                                       const FramePlanNode &node) {
    std::vector<std::string> inputs;
    for (const auto &read : node.reads)
        if (!bufferResource(model, read)) inputs.push_back(read);
    for (const auto &read : node.history_reads)
        inputs.push_back(read + std::string{HistorySuffix});
    for (const auto &read : node.reads)
        if (bufferResource(model, read)) inputs.push_back(read);
    return inputs;
}

std::string connectedLabel(const std::string &resource, bool history) {
    return history ? resource + " (history)" : resource;
}

const FramePlanNode *findNode(const FramePlanModel &model,
                              const std::string &pass) {
    const auto found =
        std::ranges::find(model.nodes, pass, &FramePlanNode::name);
    return found == model.nodes.end() ? nullptr : &*found;
}

std::string bindingRange(std::size_t from, std::size_t to_exclusive) {
    return to_exclusive - from == 1
               ? std::to_string(from)
               : std::to_string(from) + ".." + std::to_string(to_exclusive - 1);
}

FramePlanEditRequest refuseLine(std::string line) {
    FramePlanEditRequest request;
    request.refusal = line;
    request.line = std::move(line);
    return request;
}

// Every check framePlanPassResourceRequest makes before it sends, and then
// the same payload with `inputs` as the whole new list.
FramePlanEditRequest passInputListRequest(const FramePlanAuthoring &authoring,
                                          const std::string &graph,
                                          const std::string &pass,
                                          std::vector<std::string> inputs,
                                          std::string notice = {}) {
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
    if (type != "fullscreen" && type != "raster")
        return refuse(pass + " is a " + (type.empty() ? "typeless" : type) +
                      " pass and has no input list its shader declares.");
    FramePlanEditRequest request;
    request.method = "edit_preset_pass";
    Json params = Json::object();
    addDigests(params, authoring, *row);
    params["graph"] = graph;
    params["pass_name"] = pass;
    params["input"] = std::move(inputs);
    addDeclarationFields(params, declaration, type);
    if (type == "fullscreen" &&
        (!params.contains("output") || !params.contains("shader")))
        return refuse(pass +
                      " has no complete output and shader declaration to send "
                      "back: edit it in the Pass Editor.");
    request.params = std::move(params);
    request.notice = std::move(notice);
    return request;
}

// The pass's own `input`, as edit_preset_pass will replace it.
std::optional<std::vector<std::string>> editableInputs(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass) {
    const auto *row = authoring.find(graph, pass);
    if (!authoring.received || row == nullptr) return std::nullopt;
    return declaredInputs(row->editable_declaration);
}

} // namespace

bool framePlanNodeDeclaresInputs(const FramePlanNode &node) {
    return node.pass_inputs && node.pass_inputs->declared() &&
           (!node.pass_inputs->declarations.empty() ||
            node.pass_inputs->variadic_range);
}

std::string FramePlanInputSocket::key() const {
    if (kind == Kind::variadic_drop) return "in:variadic";
    if (kind == Kind::undeclared || !binding) return {};
    return "in@" + std::to_string(*binding);
}

std::optional<std::vector<std::string>> framePlanPassInputList(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass) {
    const auto *row = authoring.find(graph, pass);
    if (!authoring.received || row == nullptr) return std::nullopt;
    // What the engine composed and runs; the document's own list when the
    // context left the composed one out.
    if (row->declaration.contains("input"))
        return declaredInputs(row->declaration);
    return declaredInputs(row->editable_declaration);
}

FramePlanInputSockets framePlanInputSockets(
    const FramePlanModel &model, const FramePlanNode &node,
    const std::optional<std::vector<std::string>> &inputs,
    std::optional<std::size_t> shown) {
    FramePlanInputSockets result;
    if (!framePlanNodeDeclaresInputs(node)) return result;
    const auto &declared = *node.pass_inputs;
    result.declared = true;
    result.variadic = declared.variadic_range;
    const auto split =
        splitInputs(model, inputs ? *inputs : readsAsInputs(model, node));

    std::set<std::size_t> refused_bindings;
    if (declared.reload_refused)
        for (const auto &declaration : declared.reload_refused->declarations)
            refused_bindings.insert(declaration.binding);
    const auto mark = [&](FramePlanInputSocket &socket) {
        socket.removed = declared.reload_refused && socket.connected() &&
                         socket.binding &&
                         !refused_bindings.contains(*socket.binding);
    };
    const auto connect = [](FramePlanInputSocket &socket,
                            const PassInput &input) {
        socket.resource = input.resource;
        socket.history = input.history;
        socket.input_index = input.index;
    };

    std::set<std::size_t> taken_textures;
    std::set<std::size_t> taken_buffers;
    bool variadic_drawn = false;
    for (const auto &declaration : declared.declarations) {
        if (declaration.variadic) {
            if (variadic_drawn || !declared.variadic_range) continue;
            variadic_drawn = true;
            const auto [first, max] = *declared.variadic_range;
            for (std::size_t slot = 0; slot < max; ++slot) {
                const std::size_t ordinal = first + slot;
                taken_textures.insert(ordinal);
                if (ordinal >= split.textures.size()) continue;
                FramePlanInputSocket socket;
                socket.kind = FramePlanInputSocket::Kind::variadic;
                socket.binding = ordinal;
                connect(socket, split.textures[ordinal]);
                socket.label = "← " +
                               connectedLabel(socket.resource, socket.history);
                // VI-3b: the member engine://passthrough shows.
                if (shown && *shown == slot) {
                    socket.shown = true;
                    socket.label += " (shown)";
                }
                mark(socket);
                result.sockets.push_back(std::move(socket));
            }
            FramePlanInputSocket drop;
            drop.kind = FramePlanInputSocket::Kind::variadic_drop;
            drop.label = "+ input";
            result.sockets.push_back(std::move(drop));
            continue;
        }
        FramePlanInputSocket socket;
        socket.kind = FramePlanInputSocket::Kind::declared;
        socket.binding = declaration.binding;
        socket.declaration = declaration.name;
        socket.buffer = declaration.buffer();
        if (socket.buffer) {
            if (declaration.binding >= declared.buffer_binding_base) {
                const std::size_t ordinal =
                    declaration.binding - declared.buffer_binding_base;
                taken_buffers.insert(ordinal);
                if (ordinal < split.buffers.size())
                    connect(socket, split.buffers[ordinal]);
            }
        } else {
            taken_textures.insert(declaration.binding);
            if (declaration.binding < split.textures.size())
                connect(socket, split.textures[declaration.binding]);
        }
        socket.label =
            socket.connected()
                ? declaration.name + " ← " +
                      connectedLabel(socket.resource, socket.history)
                : declaration.name;
        mark(socket);
        result.sockets.push_back(std::move(socket));
    }
    // What the pass supplies and no declaration takes keeps a row of its own.
    const auto extras = [&](const std::vector<PassInput> &list,
                            const std::set<std::size_t> &taken, bool buffer) {
        for (std::size_t ordinal = 0; ordinal < list.size(); ++ordinal) {
            if (taken.contains(ordinal)) continue;
            FramePlanInputSocket socket;
            socket.kind = FramePlanInputSocket::Kind::undeclared;
            socket.buffer = buffer;
            connect(socket, list[ordinal]);
            socket.label = connectedLabel(socket.resource, socket.history);
            result.sockets.push_back(std::move(socket));
        }
    };
    extras(split.textures, taken_textures, false);
    extras(split.buffers, taken_buffers, true);

    // Rule 5: a declaration named in_<X> that is fed something else while X
    // is connected elsewhere on the same pass reads as a swapped order.
    std::set<std::string> connected;
    for (const auto &socket : result.sockets)
        if (socket.connected()) connected.insert(socket.resource);
    for (auto &socket : result.sockets) {
        if (socket.kind != FramePlanInputSocket::Kind::declared ||
            !socket.connected() || !socket.declaration.starts_with("in_"))
            continue;
        const std::string named = socket.declaration.substr(3);
        if (named == socket.resource || !connected.contains(named)) continue;
        socket.warning = "This declaration is named for " + named +
                         ", but binding " + std::to_string(*socket.binding) +
                         " is fed " + socket.resource + ".";
    }
    return result;
}

FramePlanEditRequest framePlanSocketConnectRequest(
    const FramePlanAuthoring &authoring, const FramePlanModel &model,
    const std::string &graph, const std::string &pass,
    const std::string &resource, const FramePlanSocketDrop &drop) {
    const FramePlanNode *node = findNode(model, pass);
    // VI-3b: a shader that declares no input takes none, wherever it lands.
    // Nothing is sent; the line offers engine://passthrough with the input.
    if (node != nullptr && framePlanNodeDeclaresNoInputs(*node)) {
        auto request = refuseLine(pass + "'s shader declares no inputs.");
        auto inputs = editableInputs(authoring, graph, pass)
                          .value_or(std::vector<std::string>{});
        inputs.push_back(resource);
        request.passthrough_inputs = std::move(inputs);
        request.fragment = node->pass_inputs->fragment;
        return request;
    }
    if (node == nullptr || !framePlanNodeDeclaresInputs(*node))
        return framePlanPassResourceRequest(authoring, graph, pass, {resource},
                                            true);
    if (drop.kind == FramePlanSocketDrop::Kind::body)
        return refuseLine("Drop on a socket of " + pass +
                          ": its inputs follow its shader's declarations.");
    const auto current = editableInputs(authoring, graph, pass);
    if (!current)
        return passInputListRequest(authoring, graph, pass, {});
    const auto &declared = *node->pass_inputs;
    const auto split = splitInputs(model, *current);
    const bool buffer = bufferResource(model, resource);
    const auto fill_first = [&](std::size_t from, std::size_t to_exclusive) {
        return refuseLine("Connect binding " + bindingRange(from, to_exclusive) +
                          " of " + pass +
                          " first: a pass's inputs fill the bindings in order.");
    };
    auto inputs = *current;

    if (drop.kind == FramePlanSocketDrop::Kind::variadic) {
        if (!declared.variadic_range)
            return refuseLine(pass + "'s shader has no variadic group.");
        if (buffer)
            return refuseLine(resource + " is a buffer: " + pass +
                              "'s variadic group takes images.");
        const auto [first, max] = *declared.variadic_range;
        if (split.textures.size() < first)
            return fill_first(split.textures.size(), first);
        if (split.textures.size() - first >= max)
            return refuseLine(pass + "'s shader takes at most " +
                              std::to_string(max) +
                              " inputs in its variadic group.");
        // The engine refuses a pass that names one input twice
        // (duplicate_input); say so here instead of sending it.
        if (std::ranges::find(inputs, resource) != inputs.end())
            return refuseLine(pass + " already reads " + resource + ".");
        inputs.push_back(resource);
        return passInputListRequest(authoring, graph, pass, std::move(inputs));
    }

    const auto declaration = std::ranges::find(
        declared.declarations, drop.binding,
        &FramePlanPassInputDeclaration::binding);
    if (declaration == declared.declarations.end())
        return refuseLine(pass + "'s shader declares no binding " +
                          std::to_string(drop.binding) + ".");
    if (declaration->buffer() != buffer)
        return refuseLine(
            resource + (buffer ? " is a buffer" : " is an image") +
            ": binding " + std::to_string(drop.binding) + " of " + pass +
            " takes " + (buffer ? "an image." : "a buffer."));
    const auto &list = buffer ? split.buffers : split.textures;
    std::size_t ordinal = drop.binding;
    std::size_t base = 0;
    if (buffer) {
        base = declared.buffer_binding_base;
        if (drop.binding < base)
            return refuseLine(pass + "'s binding " +
                              std::to_string(drop.binding) +
                              " is below its buffer bindings.");
        ordinal = drop.binding - base;
    }
    if (ordinal < list.size()) {
        // In place of what is connected there: one input changes, nothing
        // moves.
        inputs[list[ordinal].index] = resource;
        return passInputListRequest(authoring, graph, pass, std::move(inputs));
    }
    if (ordinal > list.size())
        return fill_first(base + list.size(), drop.binding);
    // The next binding: `input` counts textures and buffers apart, so the end
    // of the whole list is the next one of either.
    inputs.push_back(resource);
    return passInputListRequest(authoring, graph, pass, std::move(inputs));
}

FramePlanEditRequest framePlanSocketDisconnectRequest(
    const FramePlanAuthoring &authoring, const FramePlanModel &model,
    const std::string &graph, const std::string &pass,
    std::size_t input_index) {
    const auto current = editableInputs(authoring, graph, pass);
    if (!current)
        return passInputListRequest(authoring, graph, pass, {});
    if (input_index >= current->size())
        return refuseLine(pass + " has no input " +
                          std::to_string(input_index) + " to disconnect.");
    const FramePlanNode *node = findNode(model, pass);
    const auto split = splitInputs(model, *current);
    std::string notice;
    for (const bool buffer : {false, true}) {
        const auto &list = buffer ? split.buffers : split.textures;
        const auto found = std::ranges::find(list, input_index, &PassInput::index);
        if (found == list.end()) continue;
        const auto ordinal =
            static_cast<std::size_t>(std::distance(list.begin(), found));
        const std::size_t base =
            buffer && node != nullptr && node->pass_inputs
                ? node->pass_inputs->buffer_binding_base
                : 0;
        if (ordinal + 1 < list.size())
            notice = pass + ": the inputs after binding " +
                     std::to_string(base + ordinal) + " moved up by one.";
    }
    auto inputs = *current;
    inputs.erase(inputs.begin() + static_cast<std::ptrdiff_t>(input_index));
    return passInputListRequest(authoring, graph, pass, std::move(inputs),
                                std::move(notice));
}

const FramePlanInputSocket *framePlanRemovedSocket(
    const FramePlanInputSockets &sockets) {
    for (const auto &socket : sockets.sockets)
        if (socket.removed) return &socket;
    return nullptr;
}

std::string framePlanReloadRefusalLine(const std::string &pass,
                                       const FramePlanInputSocket &socket) {
    return pass +
           " keeps drawing with its previous shader: the new one no longer "
           "declares binding " +
           (socket.binding ? std::to_string(*socket.binding) : std::string{"?"}) +
           " (" + socket.resource + ").";
}

// --- VI-3b.

namespace {

std::string defineName(std::string_view define) {
    return std::string{define.substr(0, define.find('='))};
}

// The pass's shader object as the document holds it (the composed one when
// the document's own has none).
Json passShader(const FramePlanAuthoringPass &row) {
    auto shader = objectOr(row.editable_declaration, "shader");
    if (shader.empty()) shader = objectOr(row.declaration, "shader");
    return shader;
}

} // namespace

bool framePlanNodeDeclaresNoInputs(const FramePlanNode &node) {
    return node.pass_inputs && node.pass_inputs->declared() &&
           node.pass_inputs->declarations.empty() &&
           !node.pass_inputs->variadic_range;
}

std::optional<std::size_t> framePlanPassthroughShown(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass) {
    const auto *row = authoring.find(graph, pass);
    if (!authoring.received || row == nullptr) return std::nullopt;
    const auto shader = passShader(*row);
    if (textOr(shader, "fragment") != EnginePassthroughFragment)
        return std::nullopt;
    const auto defines = shader.find("defines");
    if (defines == shader.end() || !defines->is_array()) return 0;
    for (const auto &define : *defines) {
        if (!define.is_string()) continue;
        const auto text = define.get<std::string>();
        if (defineName(text) != PassthroughInputDefine) continue;
        const auto equals = text.find('=');
        if (equals == std::string::npos) return 0;
        try {
            return static_cast<std::size_t>(std::stoul(text.substr(equals + 1)));
        } catch (const std::exception &) {
            return 0;
        }
    }
    return 0;
}

Json framePlanShaderWithoutPassthroughInput(Json shader) {
    if (!shader.is_object()) return shader;
    const auto defines = shader.find("defines");
    if (defines == shader.end() || !defines->is_array()) return shader;
    Json kept = Json::array();
    for (const auto &define : *defines)
        if (!define.is_string() ||
            defineName(define.get<std::string>()) != PassthroughInputDefine)
            kept.push_back(define);
    if (kept.empty()) shader.erase("defines");
    else shader["defines"] = std::move(kept);
    return shader;
}

FramePlanEditRequest framePlanPassthroughInputRequest(
    const FramePlanAuthoring &authoring, const FramePlanModel &model,
    const std::string &graph, const std::string &pass, std::size_t binding) {
    const auto shown = framePlanPassthroughShown(authoring, graph, pass);
    if (!shown)
        return refuseLine(pass + " does not draw with " +
                          std::string{EnginePassthroughFragment} +
                          ": a click on its socket shows nothing.");
    const FramePlanNode *node = findNode(model, pass);
    if (node == nullptr || !framePlanNodeDeclaresInputs(*node) ||
        !node->pass_inputs->variadic_range)
        return refuseLine(pass + "'s shader has no variadic group.");
    const auto sockets = framePlanInputSockets(
        model, *node, framePlanPassInputList(authoring, graph, pass));
    const auto socket = std::ranges::find_if(
        sockets.sockets, [binding](const FramePlanInputSocket &entry) {
            return entry.kind == FramePlanInputSocket::Kind::variadic &&
                   entry.binding == binding;
        });
    if (socket == sockets.sockets.end())
        return refuseLine(pass + " has no input connected at binding " +
                          std::to_string(binding) + ".");
    const std::size_t k = binding - node->pass_inputs->variadic_range->first;
    if (k == *shown)
        return refuseLine(pass + " already shows " +
                          connectedLabel(socket->resource, socket->history) +
                          ".");
    const auto *row = authoring.find(graph, pass);
    auto shader = framePlanShaderWithoutPassthroughInput(passShader(*row));
    if (k != 0) {
        auto defines = shader.value("defines", Json::array());
        defines.push_back(std::string{PassthroughInputDefine} + "=" +
                          std::to_string(k));
        shader["defines"] = std::move(defines);
    }
    auto request = passInputListRequest(
        authoring, graph, pass,
        editableInputs(authoring, graph, pass).value_or(std::vector<std::string>{}),
        pass + " shows " + connectedLabel(socket->resource, socket->history) +
            ".");
    if (!request.sendable()) return request;
    request.params["shader"] = std::move(shader);
    return request;
}

FramePlanSocketFix framePlanSocketFix(const FramePlanAuthoring &authoring,
                                      const FramePlanModel &model,
                                      const std::string &graph,
                                      const std::string &pass) {
    FramePlanSocketFix fix;
    const FramePlanNode *node = findNode(model, pass);
    if (node == nullptr || !framePlanNodeDeclaresInputs(*node) ||
        !node->pass_inputs->variadic_range) {
        fix.refusal = pass + "'s shader has no variadic group to fix.";
        return fix;
    }
    const auto &declared = *node->pass_inputs;
    fix.fragment = declared.fragment;
    if (!fix.fragment.starts_with("project://") &&
        fix.fragment != EnginePassthroughFragment) {
        fix.refusal = pass + " draws with " + fix.fragment +
                      ", an engine shader Studio cannot open or copy.";
        return fix;
    }
    // A buffer's binding follows the group's max; once the group is fixed it
    // would follow the fixed inputs instead, and the shader's buffer
    // declarations would no longer match.
    if (std::ranges::any_of(declared.declarations,
                            &FramePlanPassInputDeclaration::buffer)) {
        fix.refusal = pass +
                      "'s shader declares a buffer after its variadic group: "
                      "fixing the sockets would move its binding. Edit the "
                      "shader by hand.";
        return fix;
    }
    fix.first = declared.variadic_range->first;
    const auto sockets = framePlanInputSockets(
        model, *node, framePlanPassInputList(authoring, graph, pass));
    for (const auto &socket : sockets.sockets) {
        if (socket.kind == FramePlanInputSocket::Kind::variadic)
            fix.inputs.push_back(socket.history
                                     ? socket.resource + std::string{HistorySuffix}
                                     : socket.resource);
    }
    for (const auto &declaration : declared.declarations)
        if (!declaration.variadic) fix.taken.push_back(declaration.name);
    if (fix.inputs.empty())
        fix.refusal = "Connect at least one input before fixing the sockets.";
    return fix;
}

bool framePlanTaskAtRoot(std::string_view source_reference,
                         std::string_view provider_feature,
                         std::string_view root_reference) {
    return !root_reference.empty() && provider_feature.empty() &&
           source_reference == root_reference;
}

std::string framePlanRootTaskRefusal(std::string_view task) {
    const std::string name = task.empty() ? std::string{"This task"} : std::string{task};
    // The first sentence is the status line (at most 90 characters, cut at
    // ". "); the code follows it, for the tooltip and the Task Editor.
    return name +
           ": rendering root holds no tasks; use a feature or a "
           "render.compute node. The engine refuses an edit of it (" +
           std::string{FramePlanRootTaskCode} + ").";
}

std::string framePlanTaskOrigin(const Json &row) {
    return textOr(row, "origin");
}

std::optional<FramePlanGraphNodeRef> framePlanTaskGraphNode(const Json &row) {
    if (framePlanTaskOrigin(row) != "graph_node") return std::nullopt;
    const auto source = objectOr(row, "graph_node");
    FramePlanGraphNodeRef ref{textOr(source, "scope"), textOr(source, "node"),
                              textOr(source, "instance")};
    if (ref.scope.empty() || ref.node.empty()) return std::nullopt;
    return ref;
}

FramePlanTaskLock framePlanTaskLock(std::string_view origin, bool editable,
                                    std::string_view source_reference,
                                    std::string_view provider_feature,
                                    std::string_view root_reference) {
    if (origin == "graph_node") return FramePlanTaskLock::graph_node;
    // The Q-10 sentence belongs to a task the engine credits to the root, and
    // to a row from an engine that does not say (the test Q-10 made).
    if (origin == "root" ||
        (origin.empty() &&
         framePlanTaskAtRoot(source_reference, provider_feature, root_reference)))
        return FramePlanTaskLock::root;
    return editable ? FramePlanTaskLock::none : FramePlanTaskLock::read_only;
}

std::string framePlanGraphNodeTaskReason(std::string_view read_only_reason) {
    return std::string{read_only_reason.empty() ? FramePlanGraphNodeTaskReason
                                                : read_only_reason};
}

namespace {

// Why a task cannot be changed from here, or an empty refusal when it can. A
// node's task says so first; a root task is refused by name even if a stale
// catalog still calls it editable.
FramePlanEditRequest lockedTask(const FramePlanTaskAuthoring &authoring,
                                const FramePlanTaskAuthoring::Task &row,
                                const char *fallback) {
    switch (framePlanTaskLock(row.origin, row.editable, row.source_reference,
                              row.provider_feature, authoring.root_reference)) {
    case FramePlanTaskLock::graph_node: {
        auto request = refuse(row.name + ": " +
                              framePlanGraphNodeTaskReason(row.read_only_reason) +
                              " Right-click the task and choose Open in Graph.");
        request.line = framePlanGraphNodeTaskReason(row.read_only_reason);
        return request;
    }
    case FramePlanTaskLock::root:
        return refuse(framePlanRootTaskRefusal(row.name));
    case FramePlanTaskLock::read_only:
        return refuse(row.name + ": " +
                      (row.read_only_reason.empty() ? std::string{fallback}
                                                    : row.read_only_reason));
    case FramePlanTaskLock::none:
        break;
    }
    return {};
}

} // namespace

FramePlanEditRequest computeTaskEditorRequest(const ComputeTaskEditorSubmit &submit,
                                              const Json &task) {
    FramePlanEditRequest request;
    if (submit.creating) {
        request.method = "add_compute_task";
        request.params = Json{{"base_source_digest", submit.base_source_digest},
                              {"destination_reference", submit.destination_reference},
                              {"task", task}};
        return request;
    }
    if (!task.is_object() || task.value("name", Json{}) != Json(submit.task_name))
        return refuse("The task name cannot be changed.");
    switch (framePlanTaskLock(submit.origin, submit.editable,
                              submit.source_reference, submit.provider_feature,
                              submit.root_reference)) {
    case FramePlanTaskLock::graph_node:
        // The Task Editor shows the task's name above, and Open in Graph
        // beside this line.
        return refuse(framePlanGraphNodeTaskReason(submit.read_only_reason));
    case FramePlanTaskLock::root:
        return refuse(framePlanRootTaskRefusal(submit.task_name));
    case FramePlanTaskLock::read_only:
        return refuse(submit.task_name + ": this task cannot be edited here.");
    case FramePlanTaskLock::none:
        break;
    }
    request.method = "edit_compute_task";
    request.params = Json{{"base_source_digest", submit.base_source_digest},
                          {"base_task_source_digest", submit.task_source_digest},
                          {"task_name", submit.task_name},
                          {"task", task}};
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
    if (auto locked = lockedTask(authoring, *row,
                                 "this task cannot be edited here.");
        !locked.refusal.empty())
        return locked;
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
    if (auto locked = lockedTask(authoring, *row,
                                 "this task cannot be edited here.");
        !locked.refusal.empty())
        return locked;
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
    if (auto locked = lockedTask(authoring, *row,
                                 "this task cannot be removed here.");
        !locked.refusal.empty())
        return locked;
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
        if (row == nullptr ||
            !lockedTask(tasks, *row, "").refusal.empty())
            return fields;
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

// --- F-1: render targets.

namespace {

// The format a kind puts in `format`. Scene and display targets have theirs
// chosen by the engine at compile time; the parser still needs a valid name.
std::string formatForKind(const RenderTargetFormValues &values) {
    if (values.kind == "display") return "B8G8R8A8_SRGB";
    if (values.format.empty()) return "R16G16B16A16_SFLOAT";
    return values.format;
}

std::string formatClassForKind(const RenderTargetFormValues &values) {
    if (values.kind == "scene" || values.kind == "display" ||
        values.kind == "data")
        return values.kind;
    return "explicit(" + formatForKind(values) + ")";
}

const FramePlanRenderTarget *editableTarget(const FramePlanAuthoring &authoring,
                                            const std::string &name,
                                            std::string &refusal) {
    if (!authoring.received) {
        refusal = "The render authoring context has not arrived yet.";
        return nullptr;
    }
    const auto *row = authoring.findRenderTarget(name);
    if (row == nullptr) {
        refusal = name + " is not a render target in the current authoring "
                         "context.";
        return nullptr;
    }
    if (!row->editable) {
        refusal = name + " cannot be changed here: it is " +
                  framePlanRenderTargetOrigin(row->source, name,
                                              authoring.preset_name) +
                  ".";
        return nullptr;
    }
    return row;
}

std::string formRefusal(const RenderTargetFormValues &values) {
    if (values.name.empty()) return "A render target needs a name.";
    if (values.usage.empty())
        return values.name + ": choose at least one usage.";
    if (values.fixed_size && (values.width == 0 || values.height == 0))
        return values.name + ": a fixed size needs a width and a height.";
    if (!values.fixed_size && !(values.extent_scale > 0.0))
        return values.name + ": the screen ratio must be greater than zero.";
    return {};
}

FramePlanEditRequest renderTargetRequest(const FramePlanAuthoring &authoring,
                                         Json op) {
    FramePlanEditRequest request;
    request.method = "edit_render_targets";
    request.params = Json{{"base_source_digest", authoring.source_digest},
                          {"ops", Json::array({std::move(op)})}};
    return request;
}

} // namespace

RenderTargetFormValues renderTargetFormValues(const Json &declaration) {
    RenderTargetFormValues values;
    if (!declaration.is_object()) return values;
    values.name = textOr(declaration, "name");
    values.format = textOr(declaration, "format");
    const auto format_class = textOr(declaration, "format_class");
    if (format_class == "scene" || format_class == "display" ||
        format_class == "data") {
        values.kind = format_class;
    } else {
        values.kind = "explicit";
        if (format_class.starts_with("explicit(") && format_class.ends_with(")"))
            values.format = format_class.substr(9, format_class.size() - 10);
    }
    const auto width = declaration.find("width");
    const auto height = declaration.find("height");
    values.fixed_size = width != declaration.end() && height != declaration.end() &&
                        width->is_number_unsigned() && height->is_number_unsigned();
    if (values.fixed_size) {
        values.width = width->get<std::uint32_t>();
        values.height = height->get<std::uint32_t>();
    }
    if (const auto scale = declaration.find("extent_scale");
        scale != declaration.end() && scale->is_number())
        values.extent_scale = scale->get<double>();
    values.usage.clear();
    if (const auto usage = declaration.find("usage");
        usage != declaration.end() && usage->is_array()) {
        for (const auto &entry : *usage)
            if (entry.is_string()) values.usage.push_back(entry.get<std::string>());
    }
    values.history = declaration.value("history", false);
    if (const auto color = declaration.find("clear_color");
        color != declaration.end() && color->is_array() && color->size() == 4 &&
        std::ranges::all_of(*color, [](const Json &c) { return c.is_number(); })) {
        std::array<double, 4> rgba{};
        for (std::size_t index = 0; index < rgba.size(); ++index)
            rgba[index] = color->at(index).get<double>();
        values.clear_color = rgba;
    }
    return values;
}

Json renderTargetDeclaration(const RenderTargetFormValues &values,
                             const Json &base) {
    Json declaration = base.is_object() ? base : Json::object();
    declaration["name"] = values.name;
    declaration["format"] = formatForKind(values);
    declaration["format_class"] = formatClassForKind(values);
    if (values.fixed_size) {
        declaration["extent_scale"] = 1.0;
        declaration["width"] = values.width;
        declaration["height"] = values.height;
    } else {
        declaration["extent_scale"] = values.extent_scale;
        declaration.erase("width");
        declaration.erase("height");
    }
    declaration["usage"] = values.usage;
    if (values.history) declaration["history"] = true;
    else declaration.erase("history");
    if (values.clear_color)
        declaration["clear_color"] = Json::array(
            {(*values.clear_color)[0], (*values.clear_color)[1],
             (*values.clear_color)[2], (*values.clear_color)[3]});
    else declaration.erase("clear_color");
    return declaration;
}

FramePlanEditRequest framePlanRenderTargetAddRequest(
    const FramePlanAuthoring &authoring, const RenderTargetFormValues &values) {
    if (!authoring.received)
        return refuse("The render authoring context has not arrived yet.");
    if (const auto reason = formRefusal(values); !reason.empty())
        return refuse(reason);
    return renderTargetRequest(
        authoring,
        Json{{"op", "add"}, {"target", renderTargetDeclaration(values)}});
}

FramePlanEditRequest framePlanRenderTargetReplaceRequest(
    const FramePlanAuthoring &authoring, const std::string &name,
    const RenderTargetFormValues &values) {
    std::string refusal;
    const auto *row = editableTarget(authoring, name, refusal);
    if (row == nullptr) return refuse(refusal);
    if (const auto reason = formRefusal(values); !reason.empty())
        return refuse(reason);
    const auto declaration = renderTargetDeclaration(values, row->authored);
    if (declaration == row->authored)
        return refuse(name + " already has exactly these settings.");
    return renderTargetRequest(authoring, Json{{"op", "replace"},
                                               {"name", name},
                                               {"target", declaration}});
}

FramePlanEditRequest framePlanRenderTargetRemoveRequest(
    const FramePlanAuthoring &authoring, const std::string &name) {
    std::string refusal;
    if (editableTarget(authoring, name, refusal) == nullptr)
        return refuse(refusal);
    return renderTargetRequest(authoring,
                               Json{{"op", "remove"}, {"name", name}});
}

std::string framePlanRenderTargetOrigin(const std::string &source,
                                        const std::string &name,
                                        const std::string &preset_name) {
    if (source == "engine")
        return name == "display" ? "the screen output the engine provides"
                                 : "a target the engine adds";
    if (source == "authoring")
        return "declared by this project's editor in "
               "passes/authoring/render-targets.json";
    if (source == "root") return "declared in the project's rendering document";
    if (source == "preset")
        return preset_name.empty() ? "a target of the rendering preset"
                                   : "a target of preset " + preset_name;
    if (source.starts_with("feature:"))
        return "a target of feature " + source.substr(8);
    return "made while the pipeline was composed, with no document the editor "
           "can name";
}

std::string framePlanRenderTargetRejection(const Json &error,
                                           const std::string &preset_name) {
    const auto code = textOr(error, "code");
    const auto name = textOr(error, "name");
    const auto message = textOr(error, "message");
    // LP-0d: a graph variant (preview, XR) that refused the edit says so
    // whatever the code -- its graph refusal keeps the graph's own code now,
    // with `variant` among its fields.
    // LP-2: a loop's refusal (a draw-loop one comes back with its own code)
    // reads as its sentence the same way.
    if (!textOr(error, "variant").empty() || !graphLoopRefusalDescription(error).empty())
        return renderAuthoringRefusalLine(error);
    if (code == "render_target_referenced") {
        std::vector<std::string> users;
        if (const auto found = error.find("users");
            found != error.end() && found->is_array())
            for (const auto &user : *found)
                if (user.is_string()) users.push_back(user.get<std::string>());
        return name + " is still used by " + join(users) +
               ": disconnect it there before deleting or renaming it.";
    }
    if (code == "render_target_not_editable")
        return name + " cannot be changed here: it is " +
               framePlanRenderTargetOrigin(textOr(error, "source"), name,
                                           preset_name) +
               ".";
    if (code == "render_target_exists")
        return "A render target named " + name + " already exists (" +
               framePlanRenderTargetOrigin(textOr(error, "source"), name,
                                           preset_name) +
               ").";
    if (code == "render_target_reserved")
        return name + " is the engine's own name for the screen output: "
                      "choose another name.";
    if (code == "render_target_not_found")
        return "No render target named " + name + " is declared.";
    if (code == "render_pipeline_preflight_failed")
        return "The engine refused the render target: " + message;
    if (code.empty()) return message;
    return code + ": " + message;
}

std::string framePlanLoopOriginLine(const FramePlanLoopOrigin &origin) {
    return "loop " + origin.loop + " · element " + std::to_string(origin.element) +
           " · body node " + origin.node;
}

// --- LP-4e: Decompose into loop.

const FramePlanLoop *framePlanDrawLoopOf(const FramePlanModel &model, const std::string &pass) {
    const auto found = std::ranges::find_if(model.loops, [&](const FramePlanLoop &loop) {
        return loop.tier == "draw_loop" && !loop.pass.empty() && loop.pass == pass;
    });
    return found == model.loops.end() ? nullptr : &*found;
}

std::string framePlanDrawLoopLine(const FramePlanModel &model, const std::string &pass) {
    const auto *loop = framePlanDrawLoopOf(model, pass);
    if (loop == nullptr) return {};
    const auto count = loop->elementCount();
    return "loop: " + loop->node + " (" + std::to_string(count) +
           (count == 1 ? " material)" : " materials)");
}

std::string framePlanDecomposeRefusal(const FramePlanAuthoring &authoring,
                                      const FramePlanModel *model, const std::string &graph,
                                      const std::string &pass) {
    if (model != nullptr)
        if (const auto *loop = framePlanDrawLoopOf(*model, pass))
            return pass + " is already a render.material_pass of the graph; its loop " +
                   loop->node + " is edited in the Graph tab.";
    if (!authoring.received)
        return "The render authoring context has not arrived yet: " + pass +
               " cannot be decomposed until it does.";
    const auto *row = authoring.find(graph, pass);
    // Nothing the context lists: the engine is the one that knows (a
    // render.material_pass it already is, a name it does not declare).
    if (row == nullptr) return {};
    const auto type = row->declaration.value("type", std::string{});
    if (!type.empty() && type != "material")
        return pass + " is a " + type + " pass; only a material pass decomposes into a loop.";
    if (row->provenance_source == "feature" || row->feature)
        return pass + " is a pass of a feature" +
               (row->provider_reference.empty() ? std::string{}
                                                : " (" + row->provider_reference + ")") +
               ": a feature's pass is replaced through its pass_replacements, not decomposed.";
    if (row->root) return {};
    if (!authoring.direct && !authoring.preset_forked)
        return pass + " is a pass of the engine preset" +
               (authoring.preset_name.empty() ? std::string{} : " " + authoring.preset_name) +
               ": fork the preset into the project first (Features tab, Fork), then "
               "decompose the pass there.";
    if (row->preset) return {};
    return passRefusal(authoring, *row);
}

FramePlanEditRequest framePlanDecomposeRequest(const FramePlanAuthoring &authoring,
                                               const FramePlanModel *model,
                                               const std::string &graph,
                                               const std::string &pass) {
    if (auto refusal = framePlanDecomposeRefusal(authoring, model, graph, pass); !refusal.empty())
        return refuse(std::move(refusal));
    FramePlanEditRequest request;
    request.method = "decompose_material_pass";
    request.params = Json{{"base_source_digest", authoring.source_digest}, {"pass", pass}};
    // A pass of the project's preset lives in the preset: its digest goes
    // too. A pass the direct root declares -- or one the context does not
    // list, which the engine then names -- goes with the root's alone.
    const auto *row = authoring.find(graph, pass);
    if (row != nullptr && !row->root && row->preset && !authoring.direct)
        request.params["base_preset_digest"] = authoring.preset_digest;
    request.notice = "Decomposed " + pass + " into a loop; the Graph tab shows it.";
    return request;
}

std::string framePlanDecomposeConfirmation(const std::string &pass) {
    return "This rewrites " + pass +
           " as graph nodes and a loop over its materials. Undo takes it back "
           "(Undo on the Frame Plan, or Ctrl+Z there).\n\nWith the loop, each kind of material can be "
           "drawn its own way: another variant, another render state, or not at all.";
}

namespace {

// A refusal's fields: beside its code, or relayed under error_fields.
Json refusalFields(const Json &error) {
    if (const auto relayed = error.find("error_fields");
        relayed != error.end() && relayed->is_object())
        return *relayed;
    return error;
}

} // namespace

std::string framePlanDecomposeRejection(const Json &error) {
    if (!error.is_object()) return "The engine refused to decompose the pass.";
    const auto code = textOr(error, "code");
    const auto fields = refusalFields(error);
    const auto pass = textOr(fields, "pass").empty() ? std::string{"The pass"}
                                                     : textOr(fields, "pass");
    const auto suffix = " (" + code + ").";
    if (code == "material_pass_not_decomposable") {
        const auto reason = textOr(fields, "reason");
        if (reason == "not_found")
            return "Neither the rendering config nor its preset declares a pass named " + pass +
                   suffix;
        if (reason == "graph_pass")
            return pass + " is already a render.material_pass of the graph: edit its loop "
                          "in the Graph tab" + suffix;
        if (reason == "feature_pass")
            return pass + " is a pass of the feature " + textOr(fields, "feature") +
                   ": a feature's pass is replaced through its pass_replacements, not "
                   "decomposed" + suffix;
        if (reason == "not_material")
            return pass + " is a " + textOr(fields, "type") +
                   " pass; only a material pass decomposes into a loop" + suffix;
        if (reason == "contract_unsupported")
            return pass + " draws with " + textOr(fields, "contract") +
                   ", which no draw_loop runs over" + suffix;
        if (reason == "field_unsupported")
            return pass + " sets " + textOr(fields, "field") +
                   ", which a render.material_pass cannot carry: remove it first" + suffix;
        if (reason == "attachment_unsupported")
            return "The graph cannot name " + pass + "'s " + textOr(fields, "socket") +
                   " attachment (a subresource, no depth, or more colours than the "
                   "contract's sockets)" + suffix;
        return pass + " cannot be decomposed: " + reason + suffix;
    }
    if (code == "project_preset_required")
        return pass + " is a pass of the engine preset " + textOr(fields, "preset") +
               ": fork the preset into the project first (Features tab, Fork), then "
               "decompose the pass there" + suffix;
    if (code == "graph.node_duplicate@1")
        return "The graph already has a node named " + textOr(fields, "node") +
               ", which decomposing would add: rename it first" + suffix;
    return renderAuthoringRefusalLine(error);
}

std::string framePlanDecomposedLoop(const Json &response) {
    const auto decomposed = objectOr(response, "decomposed");
    return textOr(decomposed, "loop");
}

// --- LP-4f2: a shipped decomposed graph in place of a feature.

std::vector<GraphTemplateInfo> graphTemplatesFromResponse(const Json &response) {
    std::vector<GraphTemplateInfo> templates;
    const auto list = response.is_object() ? response.find("templates") : response.end();
    if (!response.is_object() || list == response.end() || !list->is_array()) return templates;
    for (const auto &entry : *list) {
        if (!entry.is_object()) continue;
        GraphTemplateInfo info;
        info.reference = textOr(entry, "reference");
        info.title = textOr(entry, "title");
        info.description = textOr(entry, "description");
        const auto decomposes = objectOr(entry, "decomposes");
        info.feature = textOr(decomposes, "feature");
        if (info.reference.empty() || info.feature.empty()) continue;
        if (info.title.empty()) info.title = info.reference;
        info.remove_feature = decomposes.value("remove_feature", false);
        const auto replacements = objectOr(decomposes, "pass_replacements");
        for (const auto &[pass, node] : replacements.items())
            if (node.is_string()) info.pass_replacements.emplace_back(pass, node.get<std::string>());
        if (const auto defines = decomposes.find("shader_defines");
            defines != decomposes.end() && defines->is_array())
            for (const auto &define : *defines)
                if (define.is_string()) info.shader_defines.push_back(define.get<std::string>());
        const auto document = objectOr(entry, "document");
        info.definitions = objectOr(document, "definitions").size();
        if (const auto nodes = document.find("nodes"); nodes != document.end() && nodes->is_array()) {
            info.nodes = nodes->size();
            for (const auto &node : *nodes)
                if (node.is_object() && textOr(node, "op") == "graph.loop" &&
                    !textOr(node, "id").empty())
                    info.loops.push_back(textOr(node, "id"));
        }
        templates.push_back(std::move(info));
    }
    return templates;
}

const GraphTemplateInfo *graphTemplateForFeature(const std::vector<GraphTemplateInfo> &templates,
                                                 const std::string &feature) {
    if (feature.empty()) return nullptr;
    const auto found = std::ranges::find(templates, feature, &GraphTemplateInfo::feature);
    return found == templates.end() ? nullptr : &*found;
}

std::string graphTemplateFeatureName(const std::string &feature) {
    auto name = feature.substr(feature.rfind('/') == std::string::npos ? 0 : feature.rfind('/') + 1);
    if (name.ends_with(".json")) name.resize(name.size() - 5);
    return name.empty() ? feature : name;
}

std::string graphTemplateRefusal(const std::string &feature, const std::string &scope,
                                 const std::string &document) {
    if (scope != "preset" || document.starts_with("project://")) return {};
    return graphTemplateFeatureName(feature) + " is a feature of the engine preset " +
           (document.empty() ? std::string{"the project selects"} : document) +
           ": fork the preset into the project first (Features tab, Fork), then replace the "
           "feature there.";
}

Json graphTemplateApplyParams(const GraphTemplateInfo &graph_template,
                              const std::string &source_digest, const std::string &preset_digest) {
    Json params{{"base_source_digest", source_digest}, {"template", graph_template.reference}};
    if (!preset_digest.empty()) params["base_preset_digest"] = preset_digest;
    return params;
}

std::string graphTemplateConfirmation(const GraphTemplateInfo &graph_template) {
    const auto feature = graphTemplateFeatureName(graph_template.feature);
    std::string text = graph_template.title;
    if (!graph_template.description.empty()) text += ": " + graph_template.description;
    text += "\n\n";
    if (graph_template.remove_feature) {
        text += "The feature " + feature + " is removed, and the graph draws what it drew.";
    } else if (!graph_template.pass_replacements.empty()) {
        std::set<std::string> nodes;
        for (const auto &[pass, node] : graph_template.pass_replacements) nodes.insert(node);
        std::string by;
        for (const auto &node : nodes) by += (by.empty() ? "" : ", ") + node;
        const auto count = graph_template.pass_replacements.size();
        text += "The feature " + feature + " stays; " + std::to_string(count) +
                (count == 1 ? " of its passes is" : " of its passes are") + " replaced by " + by +
                ".";
    } else {
        text += "The feature " + feature + " stays as it is.";
    }
    text += "\nThe graph gains " + std::to_string(graph_template.nodes) +
            (graph_template.nodes == 1 ? " node" : " nodes") + " and " +
            std::to_string(graph_template.definitions) +
            (graph_template.definitions == 1 ? " definition." : " definitions.");
    if (!graph_template.shader_defines.empty()) {
        text += "\nShader defines added: ";
        for (std::size_t index = 0; index < graph_template.shader_defines.size(); ++index)
            text += (index == 0 ? "" : ", ") + graph_template.shader_defines[index];
        text += ".";
    }
    text += "\n\nUndo takes it back (Ctrl+Z here, or Undo on the Frame Plan).";
    return text;
}

std::string graphTemplateRejection(const Json &error) {
    if (!error.is_object()) return "The engine refused to replace the feature.";
    const auto code = textOr(error, "code");
    const auto fields = refusalFields(error);
    const auto suffix = " (" + code + ").";
    const auto feature = graphTemplateFeatureName(textOr(fields, "feature"));
    const auto reference = textOr(fields, "template");
    if (code == "graph_template_unknown")
        return "The engine does not ship the graph " +
               (reference.empty() ? std::string{"Studio asked for"} : reference) + suffix;
    if (code == "graph_template_invalid")
        return "The graph " + reference + " cannot be taken in: " + textOr(fields, "reason") + suffix;
    if (code == "graph_template_feature_absent")
        return "Neither the rendering config nor its preset lists " + feature +
               ", which the graph replaces: add the feature, or refresh the list" + suffix;
    if (code == "graph_template_pass_replacement_conflict")
        return "The feature " + feature + " already replaces its pass " + textOr(fields, "pass") +
               " with " + textOr(fields, "current") + ", and the graph would replace it with " +
               textOr(fields, "requested") + ": remove that replacement first" + suffix;
    if (code == "project_preset_required")
        return feature + " is a feature of the engine preset " + textOr(fields, "preset") +
               ": fork the preset into the project first (Features tab, Fork), then replace the "
               "feature there" + suffix;
    if (code == "graph.node_duplicate@1")
        return "The graph already has a node named " + textOr(fields, "node") +
               ", which the loop version adds -- perhaps it is in already (Undo takes an earlier "
               "one back), or rename that node first" + suffix;
    if (code == "graph.definition_duplicate@1")
        return "The graph already has a definition named " + textOr(fields, "definition") +
               ", which the loop version adds -- perhaps it is in already (Undo takes an earlier "
               "one back), or rename that definition first" + suffix;
    return renderAuthoringRefusalLine(error);
}

std::string graphTemplateAppliedLoop(const GraphTemplateInfo &graph_template, const Json &accepted) {
    const auto applied = objectOr(accepted, "applied");
    const auto nodes = applied.find("nodes");
    if (nodes == applied.end() || !nodes->is_array()) return {};
    for (const auto &node : *nodes)
        if (node.is_string() &&
            std::ranges::find(graph_template.loops, node.get<std::string>()) !=
                graph_template.loops.end())
            return node.get<std::string>();
    return {};
}

std::string graphTemplateAppliedLine(const GraphTemplateInfo &graph_template) {
    return "Replaced " + graphTemplateFeatureName(graph_template.feature) + " with " +
           graph_template.title + ". Undo takes it back.";
}

std::string renderAuthoringRefusalLine(const Json &error) {
    const auto code = textOr(error, "code");
    const auto message = textOr(error, "message");
    // LP-0d: the graph variant that refused it; a draw-loop refusal's own
    // `variant` (the material variant it looked for) is not one (LP-2).
    const auto variant = graphRefusalVariant(error);
    std::string line;
    if (!variant.empty()) line = "The " + variant + " graph refused it: ";
    // LP-4a: a view_select refusal reads as its sentence, the code last.
    if (const auto sentence = viewSelectRefusalDescription(error);
        !sentence.empty())
        return line + sentence;
    // LP-4c: and so does a loop's or a definition's (LP-0d hands these on
    // with the graph's own code and the fields beside it, no message).
    if (const auto sentence = graphLoopRefusalDescription(error);
        !sentence.empty())
        return line + sentence;
    if (!code.empty()) line += code;
    if (!message.empty()) line += (code.empty() ? "" : ": ") + message;
    return line.empty() ? std::string{"the engine refused the edit"} : line;
}

std::string framePlanViewSelectLine(
    const std::vector<FramePlanViewSelectAxis> &axes,
    const std::vector<FramePlanViewAxis> &view_axes) {
    if (axes.empty()) return {};
    std::string line = "view: ";
    for (std::size_t index = 0; index < axes.size(); ++index) {
        if (index != 0) line += ", ";
        line += axes[index].axis + " " +
                framePlanViewValueText(view_axes, axes[index].axis,
                                       axes[index].value);
    }
    return line;
}

std::string framePlanNoSelectedViewNotice(const FramePlanNode &node) {
    if (!node.no_selected_view) return {};
    return "No view of " +
           (node.view_family.empty() ? std::string{"its view family"}
                                     : node.view_family) +
           " matches view_select this frame.";
}

std::string framePlanFamilyExtentLine(const FramePlanNode &node) {
    if (!node.family_extent) return {};
    const auto &extent = *node.family_extent;
    // The extent as VF-1's refusal writes it.
    const auto json = extent.size ? nlohmann::json::array({(*extent.size)[0], (*extent.size)[1]})
                                  : nlohmann::json{{"output_scale", extent.output_scale}};
    auto line = "Family size: " + viewFamilyExtentText(json);
    if (extent.layers > 1) line += " ×" + std::to_string(extent.layers) + " layers";
    if (!extent.pass.empty()) line += " (from " + extent.pass + ")";
    return line;
}

// --- LP-4b.

std::string framePlanViewValueText(const std::vector<FramePlanViewAxis> &axes,
                                   const std::string &axis,
                                   std::uint64_t value) {
    std::string shown = std::to_string(value);
    const auto found = std::ranges::find(axes, axis, &FramePlanViewAxis::axis);
    if (found != axes.end() && value < found->labels.size() &&
        !found->labels[value].empty())
        shown += " (" + found->labels[value] + ")";
    return shown;
}

std::vector<FramePlanViewFieldRow> framePlanViewFieldRows(
    const std::vector<FramePlanViewAxis> &view_axes,
    const std::vector<FramePlanViewSelectAxis> &view_select,
    const std::string &family) {
    std::vector<FramePlanViewFieldRow> rows;
    if (view_axes.empty()) return rows;
    const std::string family_name =
        family.empty() ? std::string{"This view family"} : family;
    const auto selectedValue =
        [&](const std::string &axis) -> std::optional<std::uint64_t> {
        const auto found = std::ranges::find(view_select, axis,
                                             &FramePlanViewSelectAxis::axis);
        if (found == view_select.end()) return std::nullopt;
        return found->value;
    };
    for (const auto &axis : view_axes) {
        FramePlanViewFieldRow row;
        row.axis = axis.axis;
        row.selected = selectedValue(axis.axis);
        if (axis.runtime || !axis.length) {
            row.kind = FramePlanViewFieldRow::Kind::runtime;
            row.text = axis.axis + ": every view, decided each frame";
            rows.push_back(std::move(row));
            continue;
        }
        row.kind = FramePlanViewFieldRow::Kind::choice;
        for (std::uint64_t value = 0; value < *axis.length; ++value)
            row.values.push_back(
                framePlanViewValueText(view_axes, axis.axis, value));
        if (row.selected && *row.selected >= *axis.length)
            row.text = axis.axis + " " + std::to_string(*row.selected) +
                       " is outside its " + std::to_string(*axis.length) +
                       " views" +
                       (*axis.length == 0
                            ? std::string{" (it has none)"}
                            : " (0.." + std::to_string(*axis.length - 1) +
                                  ")") +
                       ": the engine will refuse it.";
        rows.push_back(std::move(row));
    }
    for (const auto &selected : view_select) {
        if (std::ranges::find(view_axes, selected.axis,
                              &FramePlanViewAxis::axis) != view_axes.end())
            continue;
        FramePlanViewFieldRow row;
        row.kind = FramePlanViewFieldRow::Kind::unknown;
        row.axis = selected.axis;
        row.selected = selected.value;
        row.text = family_name + " has no view axis named " + selected.axis +
                   ": the engine will refuse it. Remove it.";
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<FramePlanViewSelectAxis> framePlanViewSelectWith(
    std::vector<FramePlanViewSelectAxis> view_select, const std::string &axis,
    std::optional<std::uint64_t> value) {
    const auto found =
        std::ranges::find(view_select, axis, &FramePlanViewSelectAxis::axis);
    if (!value) {
        if (found != view_select.end()) view_select.erase(found);
    } else if (found != view_select.end()) {
        found->value = *value;
    } else {
        view_select.push_back(FramePlanViewSelectAxis{axis, *value});
    }
    return view_select;
}

Json framePlanViewSelectJson(
    const std::vector<FramePlanViewSelectAxis> &view_select) {
    if (view_select.empty()) return nullptr;
    Json object = Json::object();
    for (const auto &axis : view_select) object[axis.axis] = axis.value;
    return object;
}

std::vector<FramePlanViewSelectAxis> framePlanDeclaredViewSelect(
    const Json &declaration) {
    std::vector<FramePlanViewSelectAxis> axes;
    if (!declaration.is_object()) return axes;
    const auto found = declaration.find("view_select");
    if (found == declaration.end() || !found->is_object()) return axes;
    for (const auto &[axis, value] : found->items()) {
        if (value.is_number_unsigned())
            axes.push_back({axis, value.get<std::uint64_t>()});
        else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
            axes.push_back(
                {axis, static_cast<std::uint64_t>(value.get<std::int64_t>())});
    }
    return axes;
}

namespace {

// The same axes and values, whatever order they are listed in.
bool sameViewSelect(const std::vector<FramePlanViewSelectAxis> &left,
                    const std::vector<FramePlanViewSelectAxis> &right) {
    return framePlanViewSelectJson(left) == framePlanViewSelectJson(right);
}

std::string viewPickText(const std::string &axis,
                         std::optional<std::uint64_t> value) {
    return axis + " " +
           (value ? std::to_string(*value) : std::string{FramePlanAllViewsText});
}

} // namespace

FramePlanEditRequest framePlanPassViewSelectRequest(
    const FramePlanAuthoring &authoring, const std::string &graph,
    const std::string &pass, const std::string &axis,
    std::optional<std::uint64_t> value) {
    if (!authoring.received)
        return refuse("The render authoring context has not arrived yet.");
    const auto *row = authoring.find(graph, pass);
    if (row == nullptr)
        return refuse(pass + " is not a pass of " + graph +
                      " in the current authoring context.");
    if (const auto reason = passRefusal(authoring, *row); !reason.empty())
        return refuse(reason);
    const auto &declaration = row->editable_declaration;
    const auto current = framePlanDeclaredViewSelect(declaration);
    const auto next = framePlanViewSelectWith(current, axis, value);
    if (sameViewSelect(current, next))
        return refuse(pass + " already runs for " + viewPickText(axis, value) +
                      ".");
    const auto type = textOr(declaration, "type");

    FramePlanEditRequest request;
    request.method = "edit_preset_pass";
    Json params = Json::object();
    addDigests(params, authoring, *row);
    params["graph"] = graph;
    params["pass_name"] = pass;
    // A fullscreen edit requires input, output and shader; they go back as
    // the declaration holds them. Every other type takes view_select alone.
    if (type == "fullscreen") {
        params["input"] = declaredInputs(declaration);
        addDeclarationFields(params, declaration, type);
        if (!params.contains("output") || !params.contains("shader"))
            return refuse(pass +
                          " has no complete output and shader declaration to "
                          "send back: edit it in the Pass Editor.");
    }
    params["view_select"] = framePlanViewSelectJson(next);
    request.params = std::move(params);
    return request;
}

FramePlanEditRequest framePlanTaskViewSelectRequest(
    const FramePlanTaskAuthoring &authoring, const std::string &task,
    const std::string &axis, std::optional<std::uint64_t> value) {
    if (!authoring.received)
        return refuse("The compute task catalog has not arrived yet.");
    const auto *row = authoring.find(task);
    if (row == nullptr)
        return refuse(task + " is not a task in the current catalog.");
    if (auto locked = lockedTask(authoring, *row,
                                 "this task cannot be edited here.");
        !locked.refusal.empty())
        return locked;
    const auto current = framePlanDeclaredViewSelect(row->declaration);
    const auto next = framePlanViewSelectWith(current, axis, value);
    if (sameViewSelect(current, next))
        return refuse(task + " already runs for " + viewPickText(axis, value) +
                      ".");
    auto declaration = row->declaration;
    if (next.empty()) declaration.erase("view_select");
    else declaration["view_select"] = framePlanViewSelectJson(next);
    FramePlanEditRequest request;
    request.method = "edit_compute_task";
    request.params = Json{{"base_source_digest", authoring.source_digest},
                          {"base_task_source_digest", row->digest},
                          {"task_name", task},
                          {"task", std::move(declaration)}};
    return request;
}

} // namespace PelicanStudio
