#include "renderconfigeditor.hpp"
#include "renderconfigtransaction.hpp"

#include "../loader/engineresources.hpp"
#include "../vkcore/renderer_config.hpp"

#include <algorithm>
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
    if (!isLowerHexDigest(base_digest)) {
        throw std::invalid_argument(
            "edit_render_features base_source_digest must be 64 lowercase hex characters");
    }
    const auto operations = params.find("operations");
    if (operations == params.end() || !operations->is_array() ||
        operations->size() > 1) {
        throw std::invalid_argument(
            "edit_render_features operations must be an array containing at most one element in v1");
    }

    const auto gate = gateSnapshot();
    if (!gate.can_edit) {
        return rejection(
            "gate_closed", "render feature editing is currently disabled",
            {{"method", "edit_render_features"},
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

    auto candidate = document_.hasFeaturesArray()
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

    const auto ticket_id =
        "render-feature-edit-" + std::to_string(next_ticket_++);
    auto candidate_documents = currentDocumentSet().documents();
    const auto root_document = std::find_if(
        candidate_documents.begin(), candidate_documents.end(),
        [&](const auto &entry) {
            return entry.normalized_reference ==
                   source_normalized_reference_;
        });
    if (root_document == candidate_documents.end()) {
        throw std::logic_error(
            "render feature edit candidate lost its root document");
    }
    root_document->bytes = candidate.bytes();
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
        .operation = "edit_render_features",
    });
    auto accepted = OrderedJson{
        {"ticket", ticket_id},
        {"status", "accepted"},
        {"base_source_digest", digestJson(base_digest)},
    };
    results_[ticket_id] = {{"ticket", ticket_id}, {"status", "pending"}};
    return accepted;
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
    }
    return result;
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

void RenderConfigEditorService::commitPending() noexcept {
    auto queue = std::move(pending_);
    pending_.clear();
    for (auto &ticket : queue) {
        auto finish = [&](OrderedJson result) {
            if (!result.contains("ticket")) result["ticket"] = ticket.id;
            results_[ticket.id] = result;
            completed_.push_back(std::move(result));
        };
        std::optional<RenderConfigDocumentCommitReceipt>
            source_receipt;
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

            if (ticket.candidate.sourceDigest() ==
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

            bool source_commit_called = false;
            std::string source_external_modification;
            const auto source_commit = [&] {
                source_commit_called = true;
                try {
                    if (!ticket.candidate_documents) {
                        throw std::logic_error(
                            "render config edit ticket has no candidate document set");
                    }
                    source_receipt =
                        commitRenderConfigCandidateDocuments(
                            dependencies_.project_root,
                            *ticket.candidate_documents);
                } catch (const RenderConfigExternalModification &error) {
                    // Production's reload participant converts callback
                    // exceptions to its result type. Preserve the CAS
                    // classification across that boundary.
                    source_external_modification = error.what();
                    throw;
                }
            };
            const auto applied = dependencies_.apply_candidate(
                *ticket.candidate_documents, source_commit);
            if (!source_external_modification.empty()) {
                finish(rejection(
                    "external_modification",
                    source_external_modification,
                    {{"ticket", ticket.id},
                     {"expected_source_digest",
                      ticket.base_source_digest}}));
                continue;
            }
            if (!applied.committed) {
                std::string rollback_error;
                if (source_receipt && source_receipt->changed()) {
                    try {
                        rollbackCommittedRenderConfigDocuments(
                            std::move(*source_receipt));
                    } catch (const std::exception &error) {
                        rollback_error = error.what();
                    }
                }
                auto details = OrderedJson{
                    {"ticket", ticket.id},
                    {"participant", "pelican.render_pipeline"},
                };
                if (!rollback_error.empty()) {
                    details["rollback_error"] = rollback_error;
                }
                finish(rejection(
                    rollback_error.empty() ? "render_pipeline_preflight_failed"
                                           : "render_config_rollback_failed",
                    applied.error.empty()
                        ? "render pipeline candidate was rejected"
                        : applied.error,
                    std::move(details)));
                continue;
            }
            if (!source_commit_called) {
                finish(rejection(
                    "render_pipeline_commit_protocol_error",
                    "pelican.render_pipeline reported a commit without invoking the source commit callback",
                    {{"ticket", ticket.id},
                     {"participant", "pelican.render_pipeline"}}));
                continue;
            }

            document_ = std::move(ticket.candidate);
            if (ticket.candidate_documents) {
                for (const auto &candidate :
                     ticket.candidate_documents->documents()) {
                    if (candidate.normalized_reference ==
                        source_normalized_reference_) {
                        continue;
                    }
                    if (candidate.operation ==
                        RenderConfigDocumentOperation::erase) {
                        managed_documents_.erase(
                            candidate.normalized_reference);
                    } else if (hasManagedFragmentMarker(
                                   candidate.bytes) &&
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
                                .digest =
                                    renderConfigSourceDigest(
                                        candidate.bytes),
                            });
                    }
                }
            }
            auto result = OrderedJson{
                {"ticket", ticket.id},
                {"status", "committed"},
                {"committed", true},
                {"published_generation", applied.published_generation},
                {"source_digest", digestJson(document_.sourceDigest())},
                {"source_commit_called", source_commit_called},
            };
            if (!ticket.operation.empty()) {
                result["operation"] = ticket.operation;
            }
            if (!ticket.fragment_reference.empty()) {
                result["fragment_reference"] =
                    ticket.fragment_reference;
            }
            if (!applied.post_commit_error.empty()) {
                result["post_commit_error"] = applied.post_commit_error;
            }
            finish(std::move(result));
        } catch (const RenderConfigExternalModification &error) {
            finish(rejection(
                "external_modification", error.what(),
                {{"ticket", ticket.id},
                 {"expected_source_digest", ticket.base_source_digest}}));
        } catch (const std::exception &error) {
            if (source_receipt && source_receipt->changed()) {
                try {
                    rollbackCommittedRenderConfigDocuments(
                        std::move(*source_receipt));
                } catch (...) {
                    finish(rejection(
                        "render_config_rollback_failed", error.what(),
                        {{"ticket", ticket.id}}));
                    continue;
                }
            }
            finish(rejection(
                "render_pipeline_preflight_failed", error.what(),
                {{"ticket", ticket.id},
                 {"participant", "pelican.render_pipeline"}}));
        } catch (...) {
            finish(rejection(
                "render_pipeline_preflight_failed",
                "unknown render pipeline candidate failure",
                {{"ticket", ticket.id},
                 {"participant", "pelican.render_pipeline"}}));
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
