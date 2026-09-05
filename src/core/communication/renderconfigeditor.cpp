#include "../shader/raygrouplayout.hpp"
#include "renderconfigeditor.hpp"
#include "renderconfigtransaction.hpp"

#include "../loader/engineresources.hpp"
#include "../vkcore/renderer_config.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

OrderedJson *findPresetFullscreenPass(OrderedJson &preset, const std::string &graph_name,
                                      const std::string &pass_name) {
    for (auto &graph : preset.at("config").at("rendering_passes")) {
        if (graph.at("name") != graph_name) continue;
        for (auto &pass : graph.at("passes")) {
            if (pass.value("name", std::string{}) == pass_name &&
                pass.value("type", std::string{}) == "fullscreen") return &pass;
        }
    }
    return nullptr;
}

void requireObject(const Json &params, std::string_view method) {
    if (!params.is_object()) {
        throw std::invalid_argument(std::string{method} +
                                    " params must be an object");
    }
}

void requireOnly(const Json &params,
                 std::initializer_list<std::string_view> allowed,
                 std::string_view method) {
    requireObject(params, method);
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) ==
            allowed.end()) {
            throw std::invalid_argument(std::string{method} +
                                        " contains unsupported field '" +
                                        key + "'");
        }
    }
}

std::string requireString(const Json &object, std::string_view field,
                          std::string_view method) {
    const auto found = object.find(std::string{field});
    if (found == object.end() || !found->is_string()) {
        throw std::invalid_argument(std::string{method} +
                                    " requires string field '" +
                                    std::string{field} + "'");
    }
    return found->get<std::string>();
}

bool isLowerHexDigest(std::string_view value) {
    return value.size() == 64 &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return (ch >= '0' && ch <= '9') ||
                      (ch >= 'a' && ch <= 'f');
           });
}

std::string readFileBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error(
            "could not open render config source: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

OrderedJson digestJson(std::string_view digest) {
    return {{"algorithm", "sha256"}, {"hex", digest}};
}

OrderedJson runtimeJson(const RenderConfigRuntimeSnapshot &runtime) {
    return {{"published_generation", runtime.published_generation},
            {"enabled_feature_names", runtime.enabled_feature_names}};
}

std::string primaryReason(const std::vector<std::string> &reasons) {
    return reasons.empty() ? std::string{"editor_gate_closed"}
                           : reasons.front();
}

constexpr std::string_view managedFragmentDirectory =
    "project://passes/authoring/";
constexpr std::string_view managedMarkerField =
    "pelican_editor_managed";

std::vector<std::string> allFeatureReferences(std::string_view bytes) {
    const auto root = Json::parse(bytes);
    std::vector<std::string> result;
    const auto features = root.find("features");
    if (features == root.end()) return result;
    if (!features->is_array()) {
        throw std::invalid_argument(
            "render config requires a top-level features array");
    }
    for (const auto &entry : *features) {
        if (entry.is_string()) {
            result.push_back(entry.get<std::string>());
        } else if (entry.is_object()) {
            const auto ref = entry.find("ref");
            if (ref != entry.end() && ref->is_string()) {
                result.push_back(ref->get<std::string>());
            }
        }
    }
    return result;
}

bool hasManagedFragmentMarker(std::string_view bytes) {
    try {
        const auto feature = Json::parse(bytes);
        if (!feature.is_object() ||
            feature.value("schema", std::string{}) !=
                "pelican.render_feature" ||
            feature.value("version", 0) != 1) {
            return false;
        }
        const auto marker = feature.find(std::string{managedMarkerField});
        return marker != feature.end() && marker->is_object() &&
               marker->value("schema", std::string{}) ==
                   "pelican.authored_render_fragment" &&
               marker->value("version", 0) == 1 &&
               marker->value("owner", std::string{}) ==
                   "pelican.devstudio" &&
               marker->contains("id") && marker->at("id").is_string();
    } catch (const Json::exception &) {
        return false;
    }
}

std::string authoredPassId(std::string_view graph,
                           std::string_view pass_name) {
    std::string identity{graph};
    identity.push_back('\0');
    identity.append(pass_name);
    return renderConfigSourceDigest(identity).substr(0, 20);
}

std::string managedFragmentReference(std::string_view id) {
    return std::string{managedFragmentDirectory} + "pass-" +
           std::string{id} + ".json";
}

std::string managedFragmentBytes(std::string_view id,
                                 std::string_view insert,
                                 const Json &pass) {
    OrderedJson fragment{
        {"schema", "pelican.render_feature"},
        {"version", 1},
        {"name", "authored_pass_" + std::string{id}},
        {"runtime_shader_compiler", "optional"},
        {std::string{managedMarkerField},
         {{"schema", "pelican.authored_render_fragment"},
          {"version", 1},
          {"owner", "pelican.devstudio"},
          {"id", id}}},
        {"passes",
         OrderedJson::array(
             {{{"insert", insert}, {"pass", pass}}})},
    };
    return fragment.dump(2) + "\n";
}

std::string provenanceSourceName(RenderPipelineProvenanceSource source) {
    switch (source) {
    case RenderPipelineProvenanceSource::project: return "project";
    case RenderPipelineProvenanceSource::feature: return "feature";
    case RenderPipelineProvenanceSource::engine: return "engine";
    }
    return "engine";
}

bool isManagedFragmentPath(const std::filesystem::path &project_root,
                           const std::filesystem::path &path) {
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(
        project_root / "passes" / "authoring", error);
    if (error) return false;
    const auto candidate = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end() ||
            *root_it != *candidate_it) {
            return false;
        }
    }
    return candidate_it != candidate.end();
}

} // namespace

std::vector<RenderFeatureCatalogEntry>
enumerateEngineRenderFeatureDocuments(
    const std::function<RenderFeatureRuntimeAvailability(
        std::string_view, const Json &)> &availability) {
    if (!availability) {
        throw std::invalid_argument(
            "render feature enumeration requires an engine availability evaluator");
    }
    std::vector<RenderFeatureCatalogEntry> result;
    std::set<std::string, std::less<>> names;
    for (const auto id : registeredEngineResourceIds()) {
        if (!id.starts_with("features/") || !id.ends_with(".json")) {
            continue;
        }
        const auto resource = engineResource(id);
        if (!resource) continue;
        const auto document = Json::parse(*resource);
        if (!document.is_object() ||
            document.value("schema", std::string{}) !=
                "pelican.render_feature") {
            continue;
        }
        if (!document.contains("name") ||
            !document.at("name").is_string()) {
            throw std::runtime_error(
                "registered render feature has no string name: " +
                std::string{id});
        }
        const auto name = document.at("name").get<std::string>();
        if (!names.insert(name).second) {
            throw std::runtime_error(
                "registered render feature name is duplicated: " + name);
        }
        auto evaluated = availability(name, document);
        if (!evaluated.available &&
            evaluated.unavailable_reason.empty()) {
            throw std::runtime_error(
                "engine marked render feature unavailable without a reason: " +
                name);
        }
        if (evaluated.available) {
            evaluated.unavailable_reason.clear();
        }
        result.push_back(RenderFeatureCatalogEntry{
            .name = name,
            .reference = "engine://" + std::string{id},
            .requires_runtime_module =
                renderFeatureRequiresRuntimeModule(name),
            .available = evaluated.available,
            .unavailable_reason =
                std::move(evaluated.unavailable_reason),
        });
    }
    std::ranges::sort(result, {}, &RenderFeatureCatalogEntry::name);
    return result;
}

RenderConfigEditorService::RenderConfigEditorService(
    RenderConfigEditorDependencies dependencies)
    : dependencies_{std::move(dependencies)},
      document_{AuthoredRenderConfigDocument::inspect(
          dependencies_.source_bytes)} {
    if (dependencies_.source_reference.empty() ||
        dependencies_.source_path.empty() || !dependencies_.gate ||
        !dependencies_.runtime_snapshot || !dependencies_.apply_candidate) {
        throw std::invalid_argument(
            "RenderConfigEditorService requires source, gate, runtime and apply dependencies");
    }
    if (!dependencies_.feature_catalog) {
        throw std::invalid_argument(
            "RenderConfigEditorService requires an engine-evaluated feature catalog");
    }
    if (dependencies_.project_root.empty()) {
        dependencies_.project_root =
            dependencies_.source_path.parent_path();
    }
    if (dependencies_.normalize_reference) {
        source_normalized_reference_ =
            dependencies_.normalize_reference(
                dependencies_.source_reference);
    } else {
        std::error_code error;
        source_normalized_reference_ =
            "file:" +
            std::filesystem::weakly_canonical(
                dependencies_.source_path, error)
                .generic_string();
        if (error) {
            source_normalized_reference_ =
                "file:" + dependencies_.source_path.generic_string();
        }
    }
    feature_catalog_ = dependencies_.feature_catalog();
    loadManagedDocumentBaselines();
}

RenderConfigEditorService::~RenderConfigEditorService() {
    lifetime_.reset();
}

void RenderConfigEditorService::loadManagedDocumentBaselines() {
    managed_documents_.clear();
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path) {
        return;
    }
    for (const auto &reference : allFeatureReferences(document_.bytes())) {
        try {
            const auto path =
                dependencies_.resolve_document_path(reference);
            if (!isManagedFragmentPath(dependencies_.project_root, path)) {
                continue;
            }
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error) || error) {
                continue;
            }
            auto bytes = readFileBytes(path);
            if (!hasManagedFragmentMarker(bytes)) continue;
            auto normalized =
                dependencies_.normalize_reference(reference);
            const auto digest = renderConfigSourceDigest(bytes);
            managed_documents_.insert_or_assign(
                normalized,
                ManagedDocumentBaseline{
                    .reference = reference,
                    .normalized_reference = normalized,
                    .path = path,
                    .bytes = std::move(bytes),
                    .digest = digest,
                });
        } catch (const std::exception &) {
            // Engine/fragment-address/manual references are not owned by this
            // transaction service and remain PathResolver fallbacks.
        }
    }
}

RenderConfigCandidateDocumentSet
RenderConfigEditorService::currentDocumentSet() const {
    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 1);
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected =
            {RenderConfigDocumentExistence::present,
             document_.sourceDigest()},
        .bytes = document_.bytes(),
    });
    for (const auto &[key, managed] : managed_documents_) {
        (void)key;
        documents.push_back(RenderConfigCandidateDocument{
            .reference = managed.reference,
            .normalized_reference = managed.normalized_reference,
            .path = managed.path,
            .operation = RenderConfigDocumentOperation::replace,
            .expected =
                {RenderConfigDocumentExistence::present,
                 managed.digest},
            .bytes = managed.bytes,
        });
    }
    return RenderConfigCandidateDocumentSet{
        source_normalized_reference_, std::move(documents)};
}

RenderConfigEditorService::GateSnapshot
RenderConfigEditorService::gateSnapshot() {
    const auto observation = dependencies_.gate();
    if (!gate_initialized_) {
        gate_initialized_ = true;
        observed_can_edit_ = observation.can_edit;
        observed_transition_epoch_ = observation.transition_epoch;
        observed_gate_reasons_ = observation.reasons;
    } else if (observed_can_edit_ != observation.can_edit ||
               observed_transition_epoch_ != observation.transition_epoch ||
               observed_gate_reasons_ != observation.reasons) {
        observed_can_edit_ = observation.can_edit;
        observed_transition_epoch_ = observation.transition_epoch;
        observed_gate_reasons_ = observation.reasons;
        ++gate_epoch_;
    }
    return {.can_edit = observed_can_edit_,
            .epoch = gate_epoch_,
            .transition_epoch = observed_transition_epoch_,
            .reasons = observed_gate_reasons_};
}

OrderedJson RenderConfigEditorService::rejection(
    std::string code, std::string message, OrderedJson details) const {
    details["code"] = std::move(code);
    details["message"] = std::move(message);
    return {{"status", "rejected"},
            {"committed", false},
            {"error", std::move(details)}};
}

const RenderFeatureCatalogEntry *
RenderConfigEditorService::findCatalogReference(
    std::string_view reference) const noexcept {
    const auto found = std::find_if(
        feature_catalog_.begin(), feature_catalog_.end(),
        [&](const auto &entry) { return entry.reference == reference; });
    return found == feature_catalog_.end() ? nullptr : &*found;
}

OrderedJson RenderConfigEditorService::getRenderFeatures(
    const Json &params) const {
    requireOnly(params, {}, "get_render_features");
    const auto uneditable_entries =
        document_.uneditableFeatureEntryCount();
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"features", document_.featureReferences()},
            {"has_uneditable_feature_entries",
             uneditable_entries != 0},
            {"uneditable_feature_entry_count",
             uneditable_entries},
            {"runtime", runtimeJson(dependencies_.runtime_snapshot())}};
}

OrderedJson RenderConfigEditorService::listRenderFeatures(
    const Json &params) const {
    requireOnly(params, {}, "list_render_features");
    auto features = OrderedJson::array();
    for (const auto &entry : feature_catalog_) {
        auto encoded = OrderedJson{
            {"name", entry.name},
            {"reference", entry.reference},
            {"requires_runtime_module", entry.requires_runtime_module},
            {"hot_add_supported", entry.available},
            {"available", entry.available},
        };
        if (!entry.available) {
            encoded["unavailable_reason"] =
                entry.unavailable_reason;
        }
        features.push_back(std::move(encoded));
    }
    return {{"features", std::move(features)}};
}

OrderedJson RenderConfigEditorService::editRenderFeatures(
    const Json &params) {
    requireOnly(params, {"base_source_digest", "operations"},
                "edit_render_features");
    const auto base_digest = requireString(
        params, "base_source_digest", "edit_render_features");
    const auto operations = params.find("operations");
    if (operations == params.end() || !operations->is_array() ||
        operations->size() > 1) {
        throw std::invalid_argument(
            "edit_render_features operations must be an array containing at most one element in v1");
    }

    return queueRootEdit(base_digest, "edit_render_features",
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            candidate = document_.hasFeaturesArray()
                                 ? document_
                                 : AuthoredRenderConfigDocument::initialize(
                                       document_.bytes());
            if (!operations->empty()) {
                const auto &operation = operations->front();
                requireOnly(operation, {"op", "feature"},
                            "edit_render_features operation");
                const auto op = requireString(operation, "op",
                                              "edit_render_features operation");
                const auto feature = requireString(
                    operation, "feature", "edit_render_features operation");
                if (op == "add") {
                    const auto *catalog = findCatalogReference(feature);
                    if (catalog == nullptr) {
                        return rejection(
                            "unknown_render_feature",
                            "render feature is not present in the engine catalog: " +
                                feature,
                            {{"feature", feature}});
                    }
                    if (!candidate.containsFeature(feature) &&
                        !catalog->available) {
                        return rejection(
                            catalog->requires_runtime_module
                                ? "restart_required_feature"
                                : "render_feature_unavailable",
                            catalog->unavailable_reason,
                            {{"feature", catalog->name},
                             {"reference", catalog->reference}});
                    }
                    candidate = candidate.withFeatureAdded(feature);
                } else if (op == "remove") {
                    try {
                        candidate = candidate.withFeatureRemoved(feature);
                    } catch (const std::invalid_argument &error) {
                        return rejection("render_feature_not_enabled", error.what(),
                                         {{"feature", feature}});
                    }
                } else {
                    throw std::invalid_argument(
                        "edit_render_features operation op must be 'add' or 'remove'");
                }
            }

            return std::nullopt;
        });
}

OrderedJson RenderConfigEditorService::queueRootEdit(
    const std::string &base_digest, const std::string &operation,
    const std::function<std::optional<OrderedJson>(AuthoredRenderConfigDocument &)> &transform,
    std::span<const RenderConfigCandidateDocument> additional_documents,
    std::optional<std::string> base_preset_digest) {
    if (!isLowerHexDigest(base_digest))
        throw std::invalid_argument(operation + " base_source_digest must be 64 lowercase hex characters");
    const auto gate = gateSnapshot();
    if (!gate.can_edit) {
        return rejection(
            "gate_closed", "render config editing is currently disabled",
            {{"method", operation},
             {"reason", primaryReason(gate.reasons)},
             {"gate_epoch", gate.epoch}});
    }
    if (base_digest != document_.sourceDigest()) {
        return rejection(
            "external_modification",
            "render config base digest no longer matches the engine-owned document",
            {{"expected_source_digest", document_.sourceDigest()},
             {"provided_source_digest", base_digest}});
    }
    try {
        const auto disk_digest =
            renderConfigSourceDigest(readFileBytes(dependencies_.source_path));
        if (disk_digest != document_.sourceDigest()) {
            return rejection(
                "external_modification",
                "render config source changed outside the editor",
                {{"expected_source_digest", document_.sourceDigest()},
                 {"actual_source_digest", disk_digest}});
        }
    } catch (const std::exception &error) {
        return rejection("external_modification", error.what());
    }

    auto candidate = document_;
    if (auto rejected = transform(candidate)) return *rejected;
    const auto ticket_id =
        "render-config-edit-" + std::to_string(next_ticket_++);
    auto candidate_documents = currentDocumentSet().documents();
    const auto root_document = std::find_if(
        candidate_documents.begin(), candidate_documents.end(),
        [&](const auto &entry) {
            return entry.normalized_reference ==
                   source_normalized_reference_;
        });
    if (root_document == candidate_documents.end()) {
        throw std::logic_error(
            "render config edit candidate lost its root document");
    }
    root_document->bytes = candidate.bytes();
    candidate_documents.insert(candidate_documents.begin(), additional_documents.begin(), additional_documents.end());
    pending_.push_back(Ticket{
        .id = ticket_id,
        .accepted_gate_epoch = gate.epoch,
        .accepted_transition_epoch = gate.transition_epoch,
        .base_source_digest = base_digest,
        .candidate = std::move(candidate),
        .candidate_documents =
            RenderConfigCandidateDocumentSet{
                source_normalized_reference_,
                std::move(candidate_documents)},
        .operation = operation,
        .base_preset_digest = std::move(base_preset_digest),
    });
    auto accepted = OrderedJson{
        {"ticket", ticket_id},
        {"status", "accepted"},
        {"base_source_digest", digestJson(base_digest)},
    };
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return accepted;
}

OrderedJson RenderConfigEditorService::getRayCallableRecords(const Json &params) const {
    requireOnly(params, {}, "get_ray_callable_records");
    const auto root = Json::parse(document_.bytes());
    const auto config = dependencies_.resolve_authoring_context
        ? dependencies_.resolve_authoring_context(currentDocumentSet()).config : root;
    const auto overrides = root.value("ray_task_callable_records", Json::object());
    auto tasks = Json::array();
    for (const auto &task : config.value("compute_tasks", Json::array())) {
        if (!task.contains("ray_tracing")) continue;
        const auto &ray = task.at("ray_tracing");
        if (!ray.contains("callable_record")) continue;
        const auto name = task.at("name").get<std::string>();
        const auto &declared_callables = ray.at("callable");
        const auto callables = declared_callables.is_array() ? declared_callables : Json::array({declared_callables});
        tasks.push_back({{"name", name}, {"schema", ray.at("callable_record")},
            {"callables", callables}, {"authored_records", ray.at("callable_records")},
            {"records", overrides.value(name, ray.at("callable_records"))}});
    }
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())}, {"tasks", tasks}};
}

OrderedJson RenderConfigEditorService::editRayCallableRecords(const Json &params) {
    requireOnly(params, {"base_source_digest", "task_records"}, "edit_ray_callable_records");
    const auto digest = requireString(params, "base_source_digest", "edit_ray_callable_records");
    return queueRootEdit(digest, "edit_ray_callable_records",
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            if (!params.contains("task_records") || !params.at("task_records").is_object())
                return rejection("invalid_callable_records", "task_records must be an object");
            const auto context = getRayCallableRecords(Json::object());
            const auto &tasks = context.at("tasks");
            for (const auto &[name, records] : params.at("task_records").items()) {
                const auto task = std::find_if(tasks.begin(), tasks.end(), [&](const auto &entry) {
                    return entry.at("name") == name;
                });
                if (task == tasks.end())
                    return rejection("invalid_callable_records", "Unknown callable record task: " + name);
                try {
                    const auto layout = parseRayCallableRecordLayout(task->at("schema"), name);
                    (void)encodeRayCallableRecords(records, layout, task->at("callables").size(), name);
                } catch (const std::exception &error) {
                    return rejection("invalid_callable_records", error.what());
                }
            }
            candidate = candidate.withRootValue("ray_task_callable_records", params.at("task_records").dump(2));
            return std::nullopt;
        });
}

OrderedJson RenderConfigEditorService::getSceneAccelerationStructures(const Json &params) const {
    requireOnly(params, {}, "get_scene_acceleration_structures");
    const auto root = Json::parse(document_.bytes());
    Json connections = Json::array();
    const auto config = dependencies_.resolve_authoring_context
        ? dependencies_.resolve_authoring_context(currentDocumentSet()).config : root;
    const auto overrides = root.value("ray_task_acceleration_structures", Json::object());
    for (const auto &task : config.value("compute_tasks", Json::array())) {
        if (!task.contains("ray_tracing") && !task.contains("ray_query")) continue;
        const auto name = task.at("name").get<std::string>();
        const auto &ray = task.at(task.contains("ray_tracing") ? "ray_tracing" : "ray_query");
        const auto authored = ray.value("acceleration_structure", std::string{"scene"});
        connections.push_back({{"name", name}, {"authored_acceleration_structure", authored},
            {"acceleration_structure", overrides.value(name, authored)}});
    }
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"task_connections", connections},
            {"definitions", root.value("scene_acceleration_structures", Json::array())}};
}

OrderedJson RenderConfigEditorService::editSceneAccelerationStructures(const Json &params) {
    requireOnly(params, {"base_source_digest", "definitions", "task_connections"}, "edit_scene_acceleration_structures");
    const auto base_digest = requireString(params, "base_source_digest", "edit_scene_acceleration_structures");
    if (!params.contains("definitions"))
        throw std::invalid_argument("edit_scene_acceleration_structures requires definitions");
    return queueRootEdit(base_digest, "edit_scene_acceleration_structures",
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            ResolvedRenderPipeline validation;
            validation.normalized_config["scene_acceleration_structures"] = params.at("definitions");
            try {
                (void)compileRenderPipeline(validation);
            } catch (const std::exception &error) {
                return rejection("invalid_scene_acceleration_structures", error.what());
            }
            if (params.contains("task_connections")) {
                const auto &connections = params.at("task_connections");
                if (!connections.is_object())
                    return rejection("invalid_task_connections", "task_connections must be an object");
                const auto context = getSceneAccelerationStructures(Json::object());
                for (const auto &[name, selected] : connections.items()) {
                    if (!selected.is_string() || selected.get_ref<const std::string &>().empty())
                        return rejection("invalid_task_connections", "AS names must be nonempty strings");
                    const auto &tasks = context.at("task_connections");
                    if (std::none_of(tasks.begin(), tasks.end(), [&](const auto &task) { return task.at("name") == name; }))
                        return rejection("invalid_task_connections", "Unknown ray task: " + name);
                    const auto target = selected.get<std::string>();
                    if (target != "scene" && std::none_of(params.at("definitions").begin(), params.at("definitions").end(),
                        [&](const auto &definition) { return definition.at("name") == target; }))
                        return rejection("invalid_task_connections", "Unknown scene acceleration structure: " + target);
                }
                candidate = candidate.withRootValue("ray_task_acceleration_structures", connections.dump(2));
            }
            candidate = candidate.withRootValue("scene_acceleration_structures", params.at("definitions").dump(2));
            return std::nullopt;
        });
}

OrderedJson RenderConfigEditorService::getRenderAuthoringContext(
    const Json &params) const {
    requireOnly(params, {}, "get_render_authoring_context");
    if (!dependencies_.resolve_authoring_context) {
        throw std::logic_error(
            "render authoring context resolver is unavailable");
    }
    const auto resolved =
        dependencies_.resolve_authoring_context(currentDocumentSet());
    const auto runtime = dependencies_.runtime_snapshot();

    std::optional<OrderedJson> editable_preset;
    if (resolved.pipeline_preset && resolved.pipeline_preset_bytes &&
        resolved.pipeline_preset->reference.starts_with("project://")) {
        editable_preset = OrderedJson::parse(*resolved.pipeline_preset_bytes);
    }

    std::unordered_map<std::string, const RenderPassProvenance *>
        provenance;
    for (const auto &entry : resolved.pass_provenance) {
        provenance.insert_or_assign(entry.name, &entry);
    }

    auto graphs = OrderedJson::array();
    const auto declarations = resolved.config.find("rendering_passes");
    if (declarations == resolved.config.end() ||
        !declarations->is_array()) {
        throw std::runtime_error(
            "resolved render authoring context requires rendering_passes[]");
    }
    for (const auto &graph : *declarations) {
        if (!graph.is_object() || !graph.contains("name") ||
            !graph.at("name").is_string() || !graph.contains("passes") ||
            !graph.at("passes").is_array()) {
            throw std::runtime_error(
                "resolved render authoring graph requires name and passes[]");
        }
        auto passes = OrderedJson::array();
        const auto &pass_declarations = graph.at("passes");
        for (std::size_t index = 0; index < pass_declarations.size();
             ++index) {
            const auto &pass = pass_declarations.at(index);
            if (!pass.is_object() || !pass.contains("name") ||
                !pass.at("name").is_string()) {
                throw std::runtime_error(
                    "resolved render authoring pass requires a string name");
            }
            const auto name = pass.at("name").get<std::string>();
            OrderedJson encoded{
                {"name", name},
                {"declaration_index", index},
                {"declaration", pass},
            };
            auto *preset_pass = editable_preset
                ? findPresetFullscreenPass(*editable_preset, graph.at("name").get<std::string>(), name)
                : nullptr;
            encoded["editable_preset_pass"] = preset_pass != nullptr;
            if (preset_pass) encoded["preset_declaration"] = *preset_pass;
            const auto source = provenance.find(name);
            if (source != provenance.end()) {
                encoded["provenance"] = {
                    {"source",
                     provenanceSourceName(source->second->source)},
                    {"provider_feature",
                     source->second->provider_feature},
                    {"provider_reference",
                     source->second->provider_reference},
                };
                if (!source->second->provider_reference.empty() &&
                    dependencies_.normalize_reference) {
                    try {
                        const auto key = dependencies_.normalize_reference(
                            source->second->provider_reference);
                        encoded["managed"] =
                            managed_documents_.contains(key);
                    } catch (const std::exception &) {
                        encoded["managed"] = false;
                    }
                }
            }
            passes.push_back(std::move(encoded));
        }

        auto anchors = OrderedJson::array();
        if (pass_declarations.empty()) {
            anchors.push_back({{"position", 0}, {"insert", "begin"}});
        } else {
            anchors.push_back(
                {{"position", 0},
                 {"insert", "before:" +
                                pass_declarations.front()
                                    .at("name")
                                    .get<std::string>()}});
            for (std::size_t position = 1;
                 position <= pass_declarations.size(); ++position) {
                anchors.push_back(
                    {{"position", position},
                     {"insert", "after:" +
                                    pass_declarations.at(position - 1)
                                        .at("name")
                                        .get<std::string>()}});
            }
        }
        graphs.push_back({
            {"name", graph.at("name")},
            {"passes", std::move(passes)},
            {"anchor_candidates", std::move(anchors)},
        });
    }

    auto fragments = OrderedJson::array();
    for (const auto &[key, managed] : managed_documents_) {
        (void)key;
        auto pass_names = OrderedJson::array();
        const auto feature = Json::parse(managed.bytes);
        if (const auto entries = feature.find("passes");
            entries != feature.end() && entries->is_array()) {
            for (const auto &entry : *entries) {
                if (entry.is_object() && entry.contains("pass") &&
                    entry.at("pass").is_object() &&
                    entry.at("pass").contains("name") &&
                    entry.at("pass").at("name").is_string()) {
                    pass_names.push_back(
                        entry.at("pass").at("name"));
                }
            }
        }
        fragments.push_back({
            {"reference", managed.reference},
            {"digest", digestJson(managed.digest)},
            {"pass_names", std::move(pass_names)},
        });
    }

    OrderedJson result{
        {"source_reference", dependencies_.source_reference},
        {"source_digest", digestJson(document_.sourceDigest())},
        {"published_generation", runtime.published_generation},
        {"config_kind",
         resolved.pipeline_preset ? "preset" : "direct"},
        {"graphs", std::move(graphs)},
        {"managed_fragments", std::move(fragments)},
    };
    if (resolved.pipeline_preset) {
        result["pipeline_preset"] = {
            {"reference", resolved.pipeline_preset->reference},
            {"name", resolved.pipeline_preset->name},
            {"version", resolved.pipeline_preset->version},
        };
        if (resolved.pipeline_preset_bytes)
            result["pipeline_preset"]["source_digest"] = digestJson(renderConfigSourceDigest(*resolved.pipeline_preset_bytes));
    }
    return result;
}

OrderedJson RenderConfigEditorService::forkRenderPipeline(const Json &params) {
    requireOnly(params, {"base_source_digest", "base_preset_digest", "destination_reference"},
        "fork_render_pipeline");
    const auto base_digest = requireString(params, "base_source_digest", "fork_render_pipeline");
    const auto preset_digest = requireString(params, "base_preset_digest", "fork_render_pipeline");
    const auto destination = requireString(params, "destination_reference", "fork_render_pipeline");
    if (!isLowerHexDigest(preset_digest))
        throw std::invalid_argument("fork_render_pipeline base_preset_digest must be 64 lowercase hex characters");
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference || !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "fork_render_pipeline requires the project document resolver");
    if (!destination.starts_with("project://") || destination.find('#') != std::string::npos)
        return rejection("invalid_preset_destination", "Choose a new project:// file for the preset");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (!current.pipeline_preset || !current.pipeline_preset_bytes)
            return rejection("pipeline_preset_required", "The current rendering config does not select a preset");
        if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != preset_digest)
            return rejection("external_modification", "The selected preset changed since it was read");
        const auto path = dependencies_.resolve_document_path(destination);
        if (std::filesystem::exists(path))
            return rejection("preset_destination_exists", "The destination already exists: " + destination);
        const std::array documents{RenderConfigCandidateDocument{
            .reference = destination,
            .normalized_reference = dependencies_.normalize_reference(destination),
            .path = path,
            .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}},
            .bytes = *current.pipeline_preset_bytes,
        }};
        auto result = queueRootEdit(base_digest, "fork_render_pipeline",
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                auto pipeline = Json::parse(candidate.bytes()).at("pipeline");
                pipeline["preset"] = destination;
                candidate = candidate.withRootValue("pipeline", pipeline.dump(2));
                return std::nullopt;
            }, documents, preset_digest);
        if (result.at("status") == "accepted") result["preset_reference"] = destination;
        return result;
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what());
    }
}

OrderedJson RenderConfigEditorService::editPresetPass(const Json &params) {
    constexpr auto method = "edit_preset_pass";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "graph", "pass_name", "input", "output", "shader"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto preset_digest = requireString(params, "base_preset_digest", method);
    const auto graph = requireString(params, "graph", method);
    const auto name = requireString(params, "pass_name", method);
    if (!isLowerHexDigest(base_digest) || !isLowerHexDigest(preset_digest))
        throw std::invalid_argument("edit_preset_pass requires 64 lowercase hex source digests");
    for (const auto *key : {"input", "output", "shader"}) {
        if (!params.contains(key)) throw std::invalid_argument(std::string{method} + " requires " + key);
    }
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference || !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "edit_preset_pass requires the project document resolver");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (!current.pipeline_preset || !current.pipeline_preset_bytes ||
            !current.pipeline_preset->reference.starts_with("project://"))
            return rejection("project_preset_required", "Fork the selected preset into the project before editing its passes");
        if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != preset_digest)
            return rejection("external_modification", "The selected preset changed since it was read");
        auto preset = OrderedJson::parse(*current.pipeline_preset_bytes);
        auto *pass = findPresetFullscreenPass(preset, graph, name);
        if (!pass) return rejection("preset_pass_not_editable", "The selected node is not a fullscreen pass in the project preset");
        for (const auto *key : {"input", "output", "shader"}) (*pass)[key] = params.at(key);
        const auto &reference = current.pipeline_preset->reference;
        const std::array documents{RenderConfigCandidateDocument{
            .reference = reference,
            .normalized_reference = dependencies_.normalize_reference(reference),
            .path = dependencies_.resolve_document_path(reference),
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, preset_digest},
            .bytes = preset.dump(2),
        }};
        return queueRootEdit(base_digest, method,
            [](AuthoredRenderConfigDocument &) -> std::optional<OrderedJson> { return std::nullopt; },
            documents, preset_digest);
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what());
    }
}

OrderedJson RenderConfigEditorService::addAuthoredPass(
    const Json &params) {
    requireOnly(params,
                {"base_source_digest", "graph", "insert", "pass"},
                "add_authored_pass");
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path ||
        !dependencies_.resolve_authoring_context) {
        return rejection(
            "method_unavailable",
            "add_authored_pass requires the production document-set resolver",
            {{"method", "add_authored_pass"}});
    }
    const auto base_digest = requireString(
        params, "base_source_digest", "add_authored_pass");
    if (!isLowerHexDigest(base_digest)) {
        throw std::invalid_argument(
            "add_authored_pass base_source_digest must be 64 lowercase hex characters");
    }
    const auto graph = requireString(params, "graph", "add_authored_pass");
    const auto insert = requireString(params, "insert", "add_authored_pass");
    const auto pass = params.find("pass");
    if (pass == params.end() || !pass->is_object() ||
        !pass->contains("name") || !pass->at("name").is_string() ||
        pass->at("name").get_ref<const std::string &>().empty()) {
        throw std::invalid_argument(
            "add_authored_pass requires pass object with a non-empty string name");
    }
    if (pass->value("type", std::string{}) != "fullscreen") {
        return rejection(
            "authored_pass_type_unavailable",
            "add_authored_pass currently authors fullscreen passes only",
            {{"requested_type", pass->value("type", std::string{})},
             {"supported_type", "fullscreen"}});
    }

    const auto gate = gateSnapshot();
    if (!gate.can_edit) {
        return rejection(
            "gate_closed", "render pass authoring is currently disabled",
            {{"method", "add_authored_pass"},
             {"reason", primaryReason(gate.reasons)},
             {"gate_epoch", gate.epoch}});
    }
    if (base_digest != document_.sourceDigest()) {
        return rejection(
            "external_modification",
            "render config base digest no longer matches the engine-owned document",
            {{"expected_source_digest", document_.sourceDigest()},
             {"provided_source_digest", base_digest}});
    }
    try {
        const auto disk_digest =
            renderConfigSourceDigest(readFileBytes(dependencies_.source_path));
        if (disk_digest != document_.sourceDigest()) {
            return rejection(
                "external_modification",
                "render config source changed outside the editor",
                {{"expected_source_digest", document_.sourceDigest()},
                 {"actual_source_digest", disk_digest}});
        }
    } catch (const std::exception &error) {
        return rejection("external_modification", error.what());
    }

    try {
        const auto current =
            dependencies_.resolve_authoring_context(currentDocumentSet());
        const auto graphs = current.config.find("rendering_passes");
        if (graphs == current.config.end() || !graphs->is_array()) {
            throw std::runtime_error(
                "resolved authoring context has no rendering_passes[]");
        }
        const auto selected = std::find_if(
            graphs->begin(), graphs->end(), [&](const auto &candidate) {
                return candidate.is_object() &&
                       candidate.value("name", std::string{}) == graph;
            });
        if (selected == graphs->end() || !selected->contains("passes") ||
            !selected->at("passes").is_array()) {
            return rejection(
                "render_graph_not_found",
                "the selected render graph is not present in the resolved authoring context",
                {{"graph", graph}});
        }
        bool valid_insert = false;
        const auto &passes = selected->at("passes");
        if (passes.empty()) {
            valid_insert = insert == "begin";
        } else {
            valid_insert =
                insert == "before:" +
                              passes.front().at("name").get<std::string>();
            for (const auto &candidate : passes) {
                valid_insert = valid_insert ||
                               insert == "after:" +
                                             candidate.at("name")
                                                 .get<std::string>();
            }
        }
        if (!valid_insert) {
            return rejection(
                "render_anchor_not_found",
                "choose an anchor returned by get_render_authoring_context",
                {{"graph", graph}, {"insert", insert}});
        }
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what(),
                         {{"participant", "pelican.render_pipeline"}});
    }

    const auto id = authoredPassId(
        graph, pass->at("name").get_ref<const std::string &>());
    const auto reference = managedFragmentReference(id);
    const auto normalized =
        dependencies_.normalize_reference(reference);
    const auto path = dependencies_.resolve_document_path(reference);
    if (!isManagedFragmentPath(dependencies_.project_root, path)) {
        return rejection(
            "managed_fragment_namespace_violation",
            "managed render fragments must resolve below project://passes/authoring/",
            {{"reference", reference}});
    }
    std::error_code exists_error;
    if (managed_documents_.contains(normalized) ||
        std::filesystem::exists(path, exists_error)) {
        return rejection(
            "authored_pass_exists",
            "a managed fragment already owns this graph/pass identity",
            {{"reference", reference},
             {"graph", graph},
             {"pass", pass->at("name")}});
    }
    if (exists_error) {
        return rejection("external_modification", exists_error.message(),
                         {{"reference", reference}});
    }

    auto root = document_.hasFeaturesArray()
                    ? document_
                    : AuthoredRenderConfigDocument::initialize(
                          document_.bytes());
    root = root.withFeatureAdded(reference);
    const auto fragment_bytes = managedFragmentBytes(id, insert, *pass);

    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 2);
    // Addition exposes dependencies before the root reference. If root CAS
    // loses, the transaction rolls this create back.
    documents.push_back(RenderConfigCandidateDocument{
        .reference = reference,
        .normalized_reference = normalized,
        .path = path,
        .operation = RenderConfigDocumentOperation::create,
        .expected = {RenderConfigDocumentExistence::missing, {}},
        .bytes = fragment_bytes,
    });
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected = {RenderConfigDocumentExistence::present,
                     document_.sourceDigest()},
        .bytes = root.bytes(),
    });
    for (const auto &[key, managed] : managed_documents_) {
        (void)key;
        documents.push_back(RenderConfigCandidateDocument{
            .reference = managed.reference,
            .normalized_reference = managed.normalized_reference,
            .path = managed.path,
            .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present,
                         managed.digest},
            .bytes = managed.bytes,
        });
    }
    RenderConfigCandidateDocumentSet candidate_documents{
        source_normalized_reference_, std::move(documents)};
    try {
        // This is the decisive staged-file contrast: neither the feature nor
        // preset resolver may bypass the request-local document set.
        (void)dependencies_.resolve_authoring_context(candidate_documents);
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what(),
                         {{"participant", "pelican.render_pipeline"},
                          {"fragment_reference", reference}});
    }

    const auto ticket_id =
        "render-authored-pass-" + std::to_string(next_ticket_++);
    pending_.push_back(Ticket{
        .id = ticket_id,
        .accepted_gate_epoch = gate.epoch,
        .accepted_transition_epoch = gate.transition_epoch,
        .base_source_digest = base_digest,
        .candidate = std::move(root),
        .candidate_documents = std::move(candidate_documents),
        .operation = "add_authored_pass",
        .fragment_reference = reference,
    });
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return {{"ticket", ticket_id},
            {"status", "accepted"},
            {"base_source_digest", digestJson(base_digest)},
            {"fragment_reference", reference}};
}

OrderedJson RenderConfigEditorService::removeAuthoredPass(
    const Json &params) {
    requireOnly(params, {"base_source_digest", "fragment_reference"},
                "remove_authored_pass");
    if (!dependencies_.normalize_reference ||
        !dependencies_.resolve_document_path ||
        !dependencies_.resolve_authoring_context) {
        return rejection(
            "method_unavailable",
            "remove_authored_pass requires the production document-set resolver",
            {{"method", "remove_authored_pass"}});
    }
    const auto base_digest = requireString(
        params, "base_source_digest", "remove_authored_pass");
    const auto reference = requireString(
        params, "fragment_reference", "remove_authored_pass");
    if (!isLowerHexDigest(base_digest)) {
        throw std::invalid_argument(
            "remove_authored_pass base_source_digest must be 64 lowercase hex characters");
    }
    const auto gate = gateSnapshot();
    if (!gate.can_edit) {
        return rejection(
            "gate_closed", "render pass authoring is currently disabled",
            {{"method", "remove_authored_pass"},
             {"reason", primaryReason(gate.reasons)},
             {"gate_epoch", gate.epoch}});
    }
    if (base_digest != document_.sourceDigest()) {
        return rejection(
            "external_modification",
            "render config base digest no longer matches the engine-owned document",
            {{"expected_source_digest", document_.sourceDigest()},
             {"provided_source_digest", base_digest}});
    }
    try {
        if (renderConfigSourceDigest(
                readFileBytes(dependencies_.source_path)) !=
            document_.sourceDigest()) {
            return rejection(
                "external_modification",
                "render config source changed outside the editor");
        }
    } catch (const std::exception &error) {
        return rejection("external_modification", error.what());
    }

    AuthoredRenderConfigDocument root = document_;
    try {
        root = root.withFeatureRemoved(reference);
    } catch (const std::invalid_argument &error) {
        return rejection("render_feature_not_enabled", error.what(),
                         {{"fragment_reference", reference}});
    }
    const auto normalized =
        dependencies_.normalize_reference(reference);
    const auto managed = managed_documents_.find(normalized);
    if (managed != managed_documents_.end()) {
        try {
            if (renderConfigSourceDigest(
                    readFileBytes(managed->second.path)) !=
                managed->second.digest) {
                return rejection(
                    "external_modification",
                    "managed render fragment changed outside the editor",
                    {{"fragment_reference", reference}});
            }
        } catch (const std::exception &error) {
            return rejection("external_modification", error.what(),
                             {{"fragment_reference", reference}});
        }
    }
    const auto remaining = allFeatureReferences(root.bytes());
    const auto remaining_count = static_cast<std::size_t>(std::count_if(
        remaining.begin(), remaining.end(), [&](const auto &candidate) {
            return dependencies_.normalize_reference(candidate) == normalized;
        }));

    std::vector<RenderConfigCandidateDocument> documents;
    documents.reserve(managed_documents_.size() + 1);
    // Removal severs the reference first. If fragment deletion fails, the
    // transaction restores the root from its backup.
    documents.push_back(RenderConfigCandidateDocument{
        .reference = dependencies_.source_reference,
        .normalized_reference = source_normalized_reference_,
        .path = dependencies_.source_path,
        .operation = RenderConfigDocumentOperation::replace,
        .expected = {RenderConfigDocumentExistence::present,
                     document_.sourceDigest()},
        .bytes = root.bytes(),
    });
    for (const auto &[key, baseline] : managed_documents_) {
        const bool purge = key == normalized && remaining_count == 0;
        documents.push_back(RenderConfigCandidateDocument{
            .reference = baseline.reference,
            .normalized_reference = baseline.normalized_reference,
            .path = baseline.path,
            .operation = purge ? RenderConfigDocumentOperation::erase
                               : RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present,
                         baseline.digest},
            .bytes = purge ? std::string{} : baseline.bytes,
        });
    }
    RenderConfigCandidateDocumentSet candidate_documents{
        source_normalized_reference_, std::move(documents)};
    try {
        (void)dependencies_.resolve_authoring_context(candidate_documents);
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what(),
                         {{"participant", "pelican.render_pipeline"},
                          {"fragment_reference", reference}});
    }

    const auto ticket_id =
        "render-authored-pass-" + std::to_string(next_ticket_++);
    pending_.push_back(Ticket{
        .id = ticket_id,
        .accepted_gate_epoch = gate.epoch,
        .accepted_transition_epoch = gate.transition_epoch,
        .base_source_digest = base_digest,
        .candidate = std::move(root),
        .candidate_documents = std::move(candidate_documents),
        .operation = "remove_authored_pass",
        .fragment_reference = reference,
    });
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return {{"ticket", ticket_id},
            {"status", "accepted"},
            {"base_source_digest", digestJson(base_digest)},
            {"fragment_reference", reference}};
}

OrderedJson RenderConfigEditorService::getResult(
    const Json &params) const {
    requireOnly(params, {"ticket"}, "get_edit_result");
    const auto ticket = requireString(params, "ticket", "get_edit_result");
    const auto found = results_.find(ticket);
    if (found == results_.end()) {
        throw std::invalid_argument(
            "get_edit_result ticket was not issued by this session");
    }
    return found->second;
}

bool RenderConfigEditorService::ownsTicket(
    std::string_view ticket) const noexcept {
    return results_.contains(std::string{ticket});
}

void RenderConfigEditorService::completeCommitTransaction(
    const std::shared_ptr<CommitTransaction> &transaction,
    RenderConfigRuntimeApplyResult applied) noexcept {
    transaction->completion_arrived = true;
    if (inflight_ticket_ == transaction->ticket.id) {
        inflight_ticket_.reset();
    }
    const auto finish = [this, &transaction](OrderedJson result) {
        if (!result.contains("ticket")) {
            result["ticket"] = transaction->ticket.id;
        }
        results_[transaction->ticket.id] = result;
        completed_.push_back(std::move(result));
    };
    const auto rollback_source = [&transaction]() noexcept {
        std::string error;
        if (transaction->source_receipt &&
            transaction->source_receipt->changed()) {
            try {
                rollbackCommittedRenderConfigDocuments(
                    std::move(*transaction->source_receipt));
            } catch (const std::exception &caught) {
                error = caught.what();
            } catch (...) {
                error =
                    "unknown render config rollback failure";
            }
        }
        return error;
    };
    try {
        if (!transaction->source_external_modification.empty()) {
            finish(rejection(
                "external_modification",
                transaction->source_external_modification,
                {{"ticket", transaction->ticket.id},
                 {"expected_source_digest",
                  transaction->ticket.base_source_digest}}));
            return;
        }
        if (applied.staged || !applied.committed) {
            const auto rollback_error = rollback_source();
            auto details = OrderedJson{
                {"ticket", transaction->ticket.id},
                {"participant", "pelican.render_pipeline"},
            };
            if (!rollback_error.empty()) {
                details["rollback_error"] = rollback_error;
            }
            finish(rejection(
                rollback_error.empty()
                    ? "render_pipeline_preflight_failed"
                    : "render_config_rollback_failed",
                applied.error.empty()
                    ? (applied.staged
                           ? "render pipeline completion returned staged twice"
                           : "render pipeline candidate was rejected")
                    : applied.error,
                std::move(details)));
            return;
        }
        if (!transaction->source_commit_called) {
            finish(rejection(
                "render_pipeline_commit_protocol_error",
                "pelican.render_pipeline reported a commit without invoking the source commit callback",
                {{"ticket", transaction->ticket.id},
                 {"participant", "pelican.render_pipeline"}}));
            return;
        }

        document_ = std::move(transaction->ticket.candidate);
        if (transaction->ticket.candidate_documents) {
            for (const auto &candidate :
                 transaction->ticket.candidate_documents->documents()) {
                if (candidate.normalized_reference ==
                    source_normalized_reference_) {
                    continue;
                }
                if (candidate.operation ==
                    RenderConfigDocumentOperation::erase) {
                    managed_documents_.erase(
                        candidate.normalized_reference);
                } else if (hasManagedFragmentMarker(candidate.bytes) &&
                           isManagedFragmentPath(
                               dependencies_.project_root,
                               candidate.path)) {
                    managed_documents_.insert_or_assign(
                        candidate.normalized_reference,
                        ManagedDocumentBaseline{
                            .reference = candidate.reference,
                            .normalized_reference =
                                candidate.normalized_reference,
                            .path = candidate.path,
                            .bytes = candidate.bytes,
                            .digest = renderConfigSourceDigest(
                                candidate.bytes),
                        });
                }
            }
        }
        auto completed = OrderedJson{
            {"ticket", transaction->ticket.id},
            {"status", "committed"},
            {"committed", true},
            {"published_generation", applied.published_generation},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"source_commit_called",
             transaction->source_commit_called},
        };
        if (!transaction->ticket.operation.empty()) {
            completed["operation"] =
                transaction->ticket.operation;
        }
        if (!transaction->ticket.fragment_reference.empty()) {
            completed["fragment_reference"] =
                transaction->ticket.fragment_reference;
        }
        if (!applied.post_commit_error.empty()) {
            completed["post_commit_error"] =
                applied.post_commit_error;
        }
        finish(std::move(completed));
    } catch (const std::exception &caught) {
        const auto rollback_error = rollback_source();
        finish(rejection(
            rollback_error.empty()
                ? "render_pipeline_preflight_failed"
                : "render_config_rollback_failed",
            caught.what(),
            {{"ticket", transaction->ticket.id},
             {"participant", "pelican.render_pipeline"},
             {"rollback_error", rollback_error}}));
    } catch (...) {
        (void)rollback_source();
        finish(rejection(
            "render_pipeline_preflight_failed",
            "unknown render pipeline completion failure",
            {{"ticket", transaction->ticket.id},
             {"participant", "pelican.render_pipeline"}}));
    }
}

void RenderConfigEditorService::commitPending() noexcept {
    if (inflight_ticket_) return;
    auto queue = std::move(pending_);
    pending_.clear();
    for (auto ticket_iterator = queue.begin();
         ticket_iterator != queue.end(); ++ticket_iterator) {
        auto &ticket = *ticket_iterator;
        auto finish = [&](OrderedJson result) {
            if (!result.contains("ticket")) result["ticket"] = ticket.id;
            results_[ticket.id] = result;
            completed_.push_back(std::move(result));
        };
        std::shared_ptr<CommitTransaction> transaction;
        try {
            const auto gate = gateSnapshot();
            if (!gate.can_edit || gate.epoch != ticket.accepted_gate_epoch ||
                gate.transition_epoch != ticket.accepted_transition_epoch) {
                finish(rejection(
                    "gate_closed",
                    "editor gate changed after render feature edit acceptance",
                    {{"ticket", ticket.id},
                     {"method", "edit_render_features"},
                     {"reason", primaryReason(gate.reasons)},
                     {"gate_epoch", gate.epoch}}));
                continue;
            }
            if (ticket.base_source_digest != document_.sourceDigest()) {
                finish(rejection(
                    "stale_source_digest",
                    "another render feature edit committed first",
                    {{"ticket", ticket.id},
                     {"current_source_digest", document_.sourceDigest()}}));
                continue;
            }
            const auto disk_digest = renderConfigSourceDigest(
                readFileBytes(dependencies_.source_path));
            if (disk_digest != ticket.base_source_digest) {
                finish(rejection(
                    "external_modification",
                    "render config source changed outside the editor",
                    {{"ticket", ticket.id},
                     {"expected_source_digest", ticket.base_source_digest},
                     {"actual_source_digest", disk_digest}}));
                continue;
            }

            // A preset edit can change another document while the root stays
            // byte-identical. It must still compile, check its CAS and publish.
            if (!ticket.base_preset_digest && ticket.candidate.sourceDigest() ==
                document_.sourceDigest()) {
                const auto runtime =
                    dependencies_.runtime_snapshot();
                finish(OrderedJson{
                    {"ticket", ticket.id},
                    {"status", "committed"},
                    {"committed", true},
                    {"no_change", true},
                    {"published_generation",
                     runtime.published_generation},
                    {"source_digest",
                     digestJson(document_.sourceDigest())},
                    {"source_commit_called", false},
                });
                continue;
            }

            transaction = std::make_shared<CommitTransaction>(
                std::move(ticket));
            const auto project_root =
                dependencies_.project_root;
            std::function<void()> verify_preset;
            if (transaction->ticket.base_preset_digest) {
                verify_preset = [resolve = dependencies_.resolve_authoring_context,
                                 baseline = currentDocumentSet(),
                                 expected = *transaction->ticket.base_preset_digest] {
                    try {
                        const auto source = resolve(baseline);
                        if (!source.pipeline_preset_bytes || renderConfigSourceDigest(*source.pipeline_preset_bytes) != expected)
                            throw RenderConfigExternalModification("The source preset changed before the edit was saved");
                    } catch (const std::exception &error) {
                        throw RenderConfigExternalModification(error.what());
                    }
                };
            }
            const auto source_commit =
                [transaction, project_root, verify_preset = std::move(verify_preset)] {
                transaction->source_commit_called = true;
                try {
                    if (verify_preset) verify_preset();
                    if (!transaction->ticket.candidate_documents) {
                        throw std::logic_error(
                            "render config edit ticket has no candidate document set");
                    }
                    transaction->source_receipt =
                        commitRenderConfigCandidateDocuments(
                            project_root,
                            *transaction->ticket.candidate_documents);
                } catch (const RenderConfigExternalModification &error) {
                    // Production's reload participant converts callback
                    // exceptions to its result type. Preserve the CAS
                    // classification across that boundary.
                    transaction->source_external_modification =
                        error.what();
                    throw;
                }
            };
            const std::weak_ptr<const void> lifetime =
                lifetime_;
            const auto completion =
                [this, lifetime, transaction](
                    RenderConfigRuntimeApplyResult completed) noexcept {
                    if (lifetime.expired()) return;
                    completeCommitTransaction(
                        transaction, std::move(completed));
                };
            // Mark the transaction before calling into the runtime so a
            // synchronous completion cannot race with installing the
            // in-flight ticket after it has already cleared it.
            inflight_ticket_ = transaction->ticket.id;
            const auto applied = dependencies_.apply_candidate(
                *transaction->ticket.candidate_documents, source_commit,
                completion);
            if (applied.staged) {
                if (applied.staged_generation == 0) {
                    completeCommitTransaction(
                        transaction,
                        RenderConfigRuntimeApplyResult{
                            .error =
                                "render pipeline returned staged without a generation",
                        });
                } else if (!transaction->completion_arrived) {
                    const auto remaining =
                        std::next(ticket_iterator);
                    pending_.insert(
                        pending_.end(),
                        std::make_move_iterator(remaining),
                        std::make_move_iterator(queue.end()));
                    break;
                }
                continue;
            }
            if (!transaction->completion_arrived) {
                completeCommitTransaction(transaction, applied);
            }
        } catch (const RenderConfigExternalModification &error) {
            if (transaction && !transaction->completion_arrived) {
                if (transaction->source_external_modification.empty()) {
                    transaction->source_external_modification =
                        error.what();
                }
                completeCommitTransaction(
                    transaction,
                    RenderConfigRuntimeApplyResult{
                        .error = error.what(),
                    });
            } else if (!transaction) {
                finish(rejection(
                    "external_modification", error.what(),
                    {{"ticket", ticket.id},
                     {"expected_source_digest",
                      ticket.base_source_digest}}));
            }
        } catch (const std::exception &error) {
            if (transaction && !transaction->completion_arrived) {
                completeCommitTransaction(
                    transaction,
                    RenderConfigRuntimeApplyResult{
                        .error = error.what(),
                    });
            } else if (!transaction) {
                finish(rejection(
                    "render_pipeline_preflight_failed", error.what(),
                    {{"ticket", ticket.id},
                     {"participant", "pelican.render_pipeline"}}));
            }
        } catch (...) {
            if (transaction && !transaction->completion_arrived) {
                completeCommitTransaction(
                    transaction,
                    RenderConfigRuntimeApplyResult{
                        .error =
                            "unknown render pipeline candidate failure",
                    });
            } else if (!transaction) {
                finish(rejection(
                    "render_pipeline_preflight_failed",
                    "unknown render pipeline candidate failure",
                    {{"ticket", ticket.id},
                     {"participant", "pelican.render_pipeline"}}));
            }
        }
    }
}

std::vector<OrderedJson>
RenderConfigEditorService::takeCompletedResults() {
    auto result = std::move(completed_);
    completed_.clear();
    return result;
}

} // namespace Pelican
