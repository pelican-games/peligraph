#include "../shader/raygrouplayout.hpp"
#include "renderconfigeditor.hpp"
#include "../../project/rasterrayquery.hpp"
#include "../../project/rasterpass.hpp"
#include "../../project/featurecompose.hpp"
#include "renderconfigtransaction.hpp"

#include "../loader/engineresources.hpp"
#include "../vkcore/renderer_config.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

bool isEditableAuxiliaryPass(std::string_view type) {
    return type == "debug_draw" || type == "debug_text" || type == "gizmo" ||
           type == "shadow_depth" || type == "velocity" || type == "picking";
}

bool isEditablePassType(std::string_view type) {
    return type == "fullscreen" || type == "material" || type == "raster" ||
           isEditableAuxiliaryPass(type);
}

OrderedJson *findPresetEditablePass(OrderedJson &preset, const std::string &graph_name,
                                      const std::string &pass_name) {
    for (auto &graph : preset.at("config").at("rendering_passes")) {
        if (graph.at("name") != graph_name) continue;
        for (auto &pass : graph.at("passes")) {
            if (pass.value("name", std::string{}) == pass_name &&
                isEditablePassType(pass.value("type", std::string{}))) return &pass;
        }
    }
    return nullptr;
}

OrderedJson &featureSourceFragment(OrderedJson &feature, std::string_view provider) {
    const auto name = feature.at("name").get<std::string>();
    if (provider == name) return feature;
    OrderedJson *found = nullptr;
    if (auto integrations = feature.find("integrations"); integrations != feature.end()) {
        for (auto &integration : *integrations) {
            if (name + "/" + integration.at("name").get<std::string>() != provider) continue;
            if (found) throw std::runtime_error("The feature has ambiguous integration source declarations");
            found = &integration.at("fragment");
        }
    }
    if (!found) throw std::runtime_error("The node's feature source fragment is no longer present");
    return *found;
}

OrderedJson *findFeatureEditablePass(OrderedJson &feature, std::string_view name, std::string_view provider) {
    if (feature.value("schema", std::string{}) != "pelican.render_feature") return nullptr;
    if (feature.contains("parameters"))
        throw std::runtime_error("Parameterized feature passes must be edited in their source document.");
    auto &fragment = featureSourceFragment(feature, provider);
    const auto entries = fragment.find("passes");
    if (entries == fragment.end() || !entries->is_array()) return nullptr;
    OrderedJson *found = nullptr;
    for (auto &entry : *entries) {
        if (!entry.is_object() || !entry.contains("pass") || !entry.at("pass").is_object()) continue;
        auto &pass = entry.at("pass");
        const auto type = pass.value("type", std::string{});
        if (pass.value("name", std::string{}) != name ||
            !isEditablePassType(type)) continue;
        if (found) throw std::runtime_error("The feature has ambiguous source pass declarations");
        found = &pass;
    }
    return found;
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

struct GeometrySpaceDocument {
    OrderedJson info{{"space", "geometry"}, {"editable", false},
                     {"source_reference", ""}, {"source_digest", digestJson("")}};
    std::filesystem::path path;
    std::string bytes;
};

GeometrySpaceDocument geometrySpaceDocument(
    const Json &config, const RenderConfigEditorDependencies &dependencies, bool procedural = false) {
    GeometrySpaceDocument result;
    const auto reference = config.find("ray_group_layout");
    if (reference == config.end()) {
        result.info["reason"] = "Coordinate space editing requires a project ray group layout.";
        return result;
    }
    try {
        const auto source = reference->get<std::string>();
        result.info["source_reference"] = source;
        const bool engine = source.starts_with("engine://");
        if (engine) {
            result.bytes = engineResourceOrThrow(source.substr(9));
        } else {
            if (!dependencies.resolve_document_path || !dependencies.normalize_reference)
                throw std::runtime_error("Project document access is unavailable.");
            result.path = dependencies.resolve_document_path(source);
            result.bytes = readFileBytes(result.path);
        }
        result.info["source_digest"] = digestJson(renderConfigSourceDigest(result.bytes));
        const auto layout = parseRayGroupLayout(result.bytes, source);
        result.info["space"] = layout.acceleration_structure && (procedural
                                   ? layout.acceleration_structure->procedural_world_space
                                   : layout.acceleration_structure->triangle_world_space)
                                   ? "world" : "geometry";
        if (engine) result.info["reason"] = "Engine ray group layouts cannot be edited here.";
        else if (procedural && (layout.version < 3 ||
                 !layout.engineRecordOffset(RayEngineRecordField::procedural_transform)))
            result.info["reason"] = "AABB space requires layout v3 with engine_record procedural_transform and shaders using the procedural transform helpers.";
        else if (layout.version < 2) result.info["reason"] = "Triangle space requires ray group layout version 2 or 3.";
        else result.info["editable"] = true;
    } catch (const std::exception &error) {
        result.info["reason"] = error.what();
    }
    return result;
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
    auto instances = OrderedJson::array();
    const auto root = Json::parse(document_.bytes());
    const auto append_instances = [&](const Json &features, std::string_view scope,
                                      const std::string &source_reference, const std::string &source_digest,
                                      bool source_editable) {
    for (std::size_t index = 0; index < features.size(); ++index) {
        const auto &entry = features.at(index);
        OrderedJson instance{{"index", index}, {"reference", ""}, {"parameters", Json::object()},
                             {"parameter_schema", nullptr}, {"editable", false}, {"removable", scope == "root" && entry.is_string()},
                             {"scope", scope}, {"source_reference", source_reference}, {"source_digest", digestJson(source_digest)}};
        try {
            const auto reference = entry.is_string() ? entry.get<std::string>() : entry.at("ref").get<std::string>();
            instance["reference"] = reference;
            if (entry.is_object()) instance["parameters"] = entry.value("parameters", Json::object());
            std::string bytes;
            if (reference.starts_with("engine://")) bytes = engineResourceOrThrow(reference.substr(9));
            else if (dependencies_.resolve_document_path) bytes = readFileBytes(dependencies_.resolve_document_path(reference));
            else throw std::runtime_error("Project feature document access is unavailable.");
            const auto feature = Json::parse(bytes);
            instance["feature_document_digest"] = digestJson(renderConfigSourceDigest(bytes));
            instance["materializable"] = source_editable && !feature.contains("bound_parameters");
            if (!source_editable) instance["materialize_reason"] = "Fork the engine preset into the project first.";
            else if (feature.contains("bound_parameters")) instance["materialize_reason"] = "This feature is already materialized in the project.";
            instance["parameter_schema"] = feature.value("parameters", Json(nullptr));
            if (instance.at("parameter_schema").is_null())
                instance["reason"] = "This feature does not declare editable parameters.";
            else if (!source_editable) instance["reason"] = "Fork the engine preset into the project before editing its feature parameters.";
            else instance["editable"] = true;
        } catch (const std::exception &error) {
            instance["reason"] = error.what();
        }
        instances.push_back(std::move(instance));
    }
    };
    append_instances(root.value("features", Json::array()), "root", dependencies_.source_reference, document_.sourceDigest(), true);
    if (dependencies_.resolve_authoring_context) {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (current.pipeline_preset && current.pipeline_preset_bytes) {
            const auto preset = Json::parse(*current.pipeline_preset_bytes);
            append_instances(preset.at("config").value("features", Json::array()), "preset",
                             current.pipeline_preset->reference, renderConfigSourceDigest(*current.pipeline_preset_bytes),
                             current.pipeline_preset->reference.starts_with("project://"));
        }
    }
    return {{"source_reference", dependencies_.source_reference},
            {"source_digest", digestJson(document_.sourceDigest())},
            {"features", document_.featureReferences()},
            {"feature_instances", std::move(instances)},
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

OrderedJson RenderConfigEditorService::materializeRenderFeature(const std::string &base_digest, const Json &operation) {
    constexpr auto method = "materialize render feature";
    requireOnly(operation, {"op", "scope", "index", "destination_reference", "base_feature_digest", "base_preset_digest"}, method);
    const auto scope = operation.value("scope", std::string{"root"});
    const auto destination = requireString(operation, "destination_reference", method);
    const auto feature_digest = requireString(operation, "base_feature_digest", method);
    if ((scope != "root" && scope != "preset") || !operation.contains("index") ||
        !operation.at("index").is_number_integer() || operation.at("index") < 0)
        throw std::invalid_argument("materialize requires root/preset scope and a nonnegative feature index");
    if (!isLowerHexDigest(feature_digest))
        throw std::invalid_argument("base_feature_digest must be a SHA-256 digest");
    if (!dependencies_.resolve_authoring_context || !dependencies_.resolve_document_path || !dependencies_.normalize_reference)
        return rejection("method_unavailable", "Feature materialization requires the project document resolver");
    if (!destination.starts_with("project://") || destination.find('#') != std::string::npos)
        return rejection("invalid_feature_destination", "Choose a new project:// feature file");
    try {
        const auto documents = currentDocumentSet();
        const auto current = dependencies_.resolve_authoring_context(documents);
        auto source = OrderedJson::parse(document_.bytes());
        std::optional<std::string> preset_digest;
        if (scope == "preset") {
            preset_digest = requireString(operation, "base_preset_digest", method);
            if (!isLowerHexDigest(*preset_digest)) throw std::invalid_argument("base_preset_digest must be a SHA-256 digest");
            if (!current.pipeline_preset || !current.pipeline_preset_bytes ||
                !current.pipeline_preset->reference.starts_with("project://"))
                return rejection("project_preset_required", "Fork the engine preset before materializing its features");
            if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != *preset_digest)
                return rejection("external_modification", "The preset changed since it was read");
            source = OrderedJson::parse(*current.pipeline_preset_bytes);
        } else if (operation.contains("base_preset_digest")) {
            throw std::invalid_argument("Root feature materialization does not take base_preset_digest");
        }
        auto &config = scope == "preset" ? source.at("config") : source;
        auto &features = config.at("features");
        const auto index = operation.at("index").get<std::size_t>();
        if (index >= features.size()) return rejection("feature_not_found", "Feature instance index is out of range");
        const auto &entry = features.at(index);
        const auto reference = entry.is_string() ? entry.get<std::string>() : entry.at("ref").get<std::string>();
        const auto path = dependencies_.resolve_document_path(destination);
        if (std::filesystem::exists(path))
            return rejection("feature_destination_exists", "The destination already exists: " + destination);
        const auto loader = [&](std::string_view ref) {
            std::string bytes;
            if (const auto *candidate = documents.find(dependencies_.normalize_reference(ref))) bytes = candidate->bytes;
            else if (ref.starts_with("engine://")) bytes = engineResourceOrThrow(ref.substr(9));
            else bytes = readFileBytes(dependencies_.resolve_document_path(ref));
            if (ref == reference && renderConfigSourceDigest(bytes) != feature_digest)
                throw std::runtime_error("The feature document changed since it was read");
            return bytes;
        };
        std::optional<Json> materialized;
        std::set<std::string> edited_passes;
        const auto include_overrides = [&](const Json &fragment) {
            if (const auto overrides = fragment.find("pass_overrides"); overrides != fragment.end())
                for (const auto &[name, value] : overrides->items()) edited_passes.insert(name);
        };
        const auto composition = composeRenderFeatureConfig(Json::parse(document_.bytes()), RenderFeatureComposeDependencies{
            .load_feature_json = loader,
            // This authors the currently selected feature. Actual device/module
            // availability is checked by the existing candidate publication path.
            .runtime_shader_compiler_enabled = true,
            .load_pipeline_json = loader,
            .materialized_feature = [&](std::string_view ref, const Json &feature) {
                if (ref != reference) return;
                if (materialized) throw std::runtime_error("Select a uniquely referenced feature instance");
                materialized = feature;
            },
            .applied_integration = [&](std::string_view ref, const Json &fragment) {
                if (ref == reference) include_overrides(fragment);
            },
        });
        if (!materialized) return rejection("feature_not_found", "The selected feature was not composed");
        features[index] = destination;
        for (const auto &pass : composition.pass_provenance)
            if (pass.provider_reference == reference) edited_passes.insert(pass.name);
        include_overrides(*materialized);
        for (const auto &contract : composition.surface_resource_contracts) {
            if (contract.at("provider_ref") != reference) continue;
            for (const auto *field : {"material_consumers", "fullscreen_consumers"})
                for (const auto &consumer : contract.at(field)) edited_passes.insert(consumer.get<std::string>());
        }
        std::map<std::string, std::set<std::string>> released_regions;
        for (const auto &graph : composition.config.at("rendering_passes"))
            for (const auto &pass : graph.at("passes")) {
                if (!edited_passes.contains(pass.at("name").get<std::string>())) continue;
                for (const auto &tag : pass.value("regions", Json::array()))
                    released_regions[graph.at("name").get<std::string>()].insert(tag.get<std::string>());
            }
        const auto release_replacements = [&](OrderedJson &owner_config) {
            bool changed = false;
            if (!owner_config.contains("rendering_passes")) return changed;
            for (auto &graph : owner_config.at("rendering_passes")) {
                const auto regions = released_regions.find(graph.at("name").get<std::string>());
                if (regions == released_regions.end() || !graph.contains("region_replacements")) continue;
                auto &requests = graph.at("region_replacements");
                for (auto request = requests.begin(); request != requests.end();) {
                    if (regions->second.contains(request->at("region").get<std::string>())) {
                        request = requests.erase(request);
                        changed = true;
                    } else ++request;
                }
                if (requests.empty()) graph.erase("region_replacements");
            }
            return changed;
        };
        // Persist removal of the selected regions, so later compiles cannot
        // replace the newly editable nodes with their former fused implementation.
        const bool root_regions_changed = scope == "root" && release_replacements(source);
        std::optional<OrderedJson> preset;
        if (scope == "preset") preset = source;
        else if (current.pipeline_preset_bytes) preset = OrderedJson::parse(*current.pipeline_preset_bytes);
        const bool preset_regions_changed = preset && release_replacements(preset->at("config"));
        if (preset_regions_changed && !current.pipeline_preset->reference.starts_with("project://"))
            return rejection("project_preset_required", "Fork the engine preset before releasing its region replacements");
        if (preset_regions_changed && !preset_digest)
            preset_digest = renderConfigSourceDigest(*current.pipeline_preset_bytes);
        std::vector<RenderConfigCandidateDocument> additional{{
            .reference = destination, .normalized_reference = dependencies_.normalize_reference(destination),
            .path = path, .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}}, .bytes = materialized->dump(2)}};
        if (scope == "preset" || preset_regions_changed) {
            const auto &preset_ref = current.pipeline_preset->reference;
            additional.push_back({.reference = preset_ref, .normalized_reference = dependencies_.normalize_reference(preset_ref),
                .path = dependencies_.resolve_document_path(preset_ref), .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, *preset_digest}, .bytes = preset->dump(2)});
        }
        return queueRootEdit(base_digest, "materialize_render_feature",
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                if (scope == "root") candidate = candidate.withRootValue("features", features.dump(2));
                if (root_regions_changed) candidate = candidate.withRootValue("rendering_passes", source.at("rendering_passes").dump(2));
                return std::nullopt;
            }, additional, preset_digest);
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what());
    }
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

    const auto patch_parameters = [&](Json &features, const Json &operation) -> std::optional<OrderedJson> {
        if (!operation.contains("index") || !operation.at("index").is_number_integer() || operation.at("index") < 0 ||
            !operation.contains("parameters") || !operation.at("parameters").is_object())
            return rejection("invalid_feature_parameters", "set_parameters requires a nonnegative index and parameters object");
        const auto index = operation.at("index").get<std::size_t>();
        if (!features.is_array() || index >= features.size())
            return rejection("invalid_feature_parameters", "Feature instance index is out of range");
        auto &entry = features.at(index);
        const auto &parameters = operation.at("parameters");
        if (entry.is_string()) {
            if (!parameters.empty()) entry = {{"ref", entry.get<std::string>()}, {"parameters", parameters}};
        } else if (entry.is_object() && entry.contains("ref") && entry.at("ref").is_string()) {
            entry["parameters"] = parameters;
        } else return rejection("invalid_feature_parameters", "The selected feature instance has no editable reference");
        return std::nullopt;
    };
    if (!operations->empty() && operations->front().value("op", std::string{}) == "set_parameters" &&
        operations->front().value("scope", std::string{"root"}) == "preset") {
        const auto &operation = operations->front();
        requireOnly(operation, {"op", "scope", "index", "parameters", "base_preset_digest"}, "edit_render_features operation");
        const auto digest = requireString(operation, "base_preset_digest", "set_parameters");
        if (!isLowerHexDigest(digest)) throw std::invalid_argument("base_preset_digest must be a SHA-256 digest");
        try {
            if (!dependencies_.resolve_authoring_context || !dependencies_.resolve_document_path || !dependencies_.normalize_reference)
                return rejection("project_preset_required", "Project preset document access is unavailable");
            const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
            if (!current.pipeline_preset || !current.pipeline_preset_bytes ||
                !current.pipeline_preset->reference.starts_with("project://"))
                return rejection("project_preset_required", "Fork the selected preset into the project before editing its feature parameters");
            if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != digest)
                return rejection("external_modification", "The preset changed since its feature parameters were read");
            auto preset = OrderedJson::parse(*current.pipeline_preset_bytes);
            auto features = Json(preset.at("config").value("features", OrderedJson::array()));
            if (const auto rejected = patch_parameters(features, operation)) return *rejected;
            preset["config"]["features"] = features;
            const auto &reference = current.pipeline_preset->reference;
            const std::array documents{RenderConfigCandidateDocument{
                .reference = reference, .normalized_reference = dependencies_.normalize_reference(reference),
                .path = dependencies_.resolve_document_path(reference), .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, digest}, .bytes = preset.dump(2)}};
            return queueRootEdit(base_digest, "edit_render_features",
                [](AuthoredRenderConfigDocument &) -> std::optional<OrderedJson> { return std::nullopt; }, documents, digest);
        } catch (const std::exception &error) {
            return rejection("invalid_feature_parameters", error.what());
        }
    }

    if (!operations->empty() && operations->front().value("op", std::string{}) == "materialize")
        return materializeRenderFeature(base_digest, operations->front());
    return queueRootEdit(base_digest, "edit_render_features",
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            candidate = document_.hasFeaturesArray()
                                 ? document_
                                 : AuthoredRenderConfigDocument::initialize(
                                       document_.bytes());
            if (!operations->empty()) {
                const auto &operation = operations->front();
                const auto op = requireString(operation, "op",
                                              "edit_render_features operation");
                if (op == "set_parameters") {
                    requireOnly(operation, {"op", "scope", "index", "parameters"}, "edit_render_features operation");
                    if (operation.value("scope", std::string{"root"}) != "root")
                        return rejection("invalid_feature_parameters", "Feature parameter scope must be root or preset");
                    auto features = Json::parse(candidate.bytes()).value("features", Json::array());
                    if (const auto rejected = patch_parameters(features, operation)) return rejected;
                    candidate = candidate.withRootValue("features", features.dump(2));
                    return std::nullopt;
                }
                requireOnly(operation, {"op", "feature"}, "edit_render_features operation");
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
                        "edit_render_features operation op must be 'add', 'remove' or 'set_parameters'");
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
    for (const auto &extra : additional_documents) {
        if (extra.normalized_reference == source_normalized_reference_)
            return rejection("invalid_render_document", "Additional documents cannot replace the root edit");
        const auto existing = std::find_if(candidate_documents.begin(), candidate_documents.end(), [&](const auto &entry) {
            return entry.normalized_reference == extra.normalized_reference;
        });
        if (existing == candidate_documents.end()) {
            candidate_documents.insert(candidate_documents.begin(), extra);
        } else {
            if (existing->expected.digest != extra.expected.digest)
                return rejection("external_modification", "The managed document changed outside the editor: " + extra.reference);
            *existing = extra;
        }
    }
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
        .edits_additional_documents = !additional_documents.empty(),
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
            {"triangle_geometry", geometrySpaceDocument(config, dependencies_).info},
            {"procedural_geometry", geometrySpaceDocument(config, dependencies_, true).info},
            {"definitions", root.value("scene_acceleration_structures", Json::array())}};
}

OrderedJson RenderConfigEditorService::editSceneAccelerationStructures(const Json &params) {
    requireOnly(params, {"base_source_digest", "definitions", "task_connections", "triangle_space", "procedural_space", "base_layout_digest"}, "edit_scene_acceleration_structures");
    const auto base_digest = requireString(params, "base_source_digest", "edit_scene_acceleration_structures");
    if (!params.contains("definitions"))
        throw std::invalid_argument("edit_scene_acceleration_structures requires definitions");
    std::vector<RenderConfigCandidateDocument> layout_documents;
    const bool edits_space = params.contains("triangle_space") || params.contains("procedural_space");
    const auto error_code = params.contains("procedural_space") ? "invalid_procedural_space" : "invalid_triangle_space";
    if (edits_space != params.contains("base_layout_digest"))
        return rejection(error_code, "Coordinate space edits and base_layout_digest must be supplied together");
    if (edits_space) {
        try {
            const auto digest = requireString(params, "base_layout_digest", "edit_scene_acceleration_structures");
            if (!isLowerHexDigest(digest))
                return rejection(error_code, "base_layout_digest must be a SHA-256 digest");
            const auto config = dependencies_.resolve_authoring_context
                ? dependencies_.resolve_authoring_context(currentDocumentSet()).config : Json::parse(document_.bytes());
            auto layout = geometrySpaceDocument(config, dependencies_, params.contains("procedural_space"));
            if (!layout.info.at("editable").get<bool>())
                return rejection(params.contains("procedural_space") ? "procedural_space_not_editable" : "triangle_space_not_editable", layout.info.value("reason", std::string{"Ray group layout is not editable"}));
            if (layout.info.at("source_digest").at("hex") != digest)
                return rejection("external_modification", "The ray group layout changed since it was read");
            auto document = OrderedJson::parse(layout.bytes);
            if (!document.contains("acceleration_structure"))
                document["acceleration_structure"] = {{"defaults", OrderedJson::object()}};
            for (const auto *field : {"triangle_space", "procedural_space"}) {
                if (!params.contains(field)) continue;
                const auto space = requireString(params, field, "edit_scene_acceleration_structures");
                if (space != "geometry" && space != "world")
                    return rejection(error_code, "Coordinate space must be geometry or world");
                document["acceleration_structure"][field] = space;
            }
            const auto bytes = document.dump(2);
            const auto reference = layout.info.at("source_reference").get<std::string>();
            (void)parseRayGroupLayout(bytes, reference);
            layout_documents.push_back({
                .reference = reference, .normalized_reference = dependencies_.normalize_reference(reference),
                .path = layout.path, .operation = RenderConfigDocumentOperation::replace,
                .expected = {RenderConfigDocumentExistence::present, digest}, .bytes = bytes});
        } catch (const std::exception &error) {
            return rejection(error_code, error.what());
        }
    }
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
        }, layout_documents);
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
                ? findPresetEditablePass(*editable_preset, graph.at("name").get<std::string>(), name)
                : nullptr;
            encoded["editable_preset_pass"] = preset_pass != nullptr;
            if (preset_pass) encoded["preset_declaration"] = *preset_pass;
            const auto source = provenance.find(name);
            encoded["editable_feature_pass"] = false;
            if (source != provenance.end() && source->second->source == RenderPipelineProvenanceSource::feature) {
                try {
                    const auto &reference = source->second->provider_reference;
                    if (reference.empty() || reference.starts_with("engine://"))
                        throw std::runtime_error("This pass belongs to a shared engine feature. Copy it into the project before editing.");
                    if (!dependencies_.resolve_document_path)
                        throw std::runtime_error("Project feature document access is unavailable.");
                    const auto bytes = readFileBytes(dependencies_.resolve_document_path(reference));
                    auto feature = OrderedJson::parse(bytes);
                    if (!source->second->authored_name.empty() && source->second->authored_name != name)
                        throw std::runtime_error("Generated feature variants must be edited in their source document.");
                    const auto *declaration = findFeatureEditablePass(feature,
                        source->second->authored_name.empty() ? name : source->second->authored_name,
                        source->second->provider_feature);
                    if (!declaration) throw std::runtime_error("This node is generated and has no editable pass declaration in its feature.");
                    encoded["editable_feature_pass"] = true;
                    encoded["feature_declaration"] = *declaration;
                    encoded["feature_source_reference"] = reference;
                    encoded["feature_source_digest"] = digestJson(renderConfigSourceDigest(bytes));
                } catch (const std::exception &error) {
                    encoded["feature_edit_reason"] = error.what();
                }
            }
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
            {"region_replacements", graph.value("region_replacements", Json::array())},
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
        result["preset_material_routing"] = resolved.pipeline_preset_bytes
            ? Json::parse(*resolved.pipeline_preset_bytes).at("config").value("material_routing", Json(nullptr))
            : Json(nullptr);
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

OrderedJson RenderConfigEditorService::getComputeTaskAuthoring(const Json &params) const {
    requireOnly(params, {}, "get_compute_task_authoring");
    if (!dependencies_.resolve_authoring_context)
        throw std::logic_error("render authoring context resolver is unavailable");
    const auto context = dependencies_.resolve_authoring_context(currentDocumentSet());
    auto tasks = OrderedJson::array();
    for (const auto &task : context.config.value("compute_tasks", Json::array())) {
        const auto name = task.at("name").get<std::string>();
        const auto provenance = std::find_if(context.pass_provenance.begin(), context.pass_provenance.end(),
            [&](const auto &entry) { return entry.name == name; });
        const bool from_feature = provenance != context.pass_provenance.end() &&
            provenance->source == RenderPipelineProvenanceSource::feature;
        const bool from_preset = !from_feature && context.pipeline_preset.has_value();
        const auto reference = from_feature ? provenance->provider_reference
            : from_preset ? context.pipeline_preset->reference : dependencies_.source_reference;
        OrderedJson entry{{"name", name}, {"kind", task.contains("ray_tracing") ? "ray_tracing" : "compute"},
                          {"source_reference", reference}, {"editable", false}, {"declaration", task}};
        if (from_feature) entry["provider_feature"] = provenance->provider_feature;
        if (reference.starts_with("engine://")) {
            entry["read_only_reason"] = "This task belongs to a shared engine document. Copy it into the project to edit it.";
        } else {
            try {
                const auto bytes = reference == dependencies_.source_reference ? document_.bytes()
                    : from_preset && context.pipeline_preset_bytes ? *context.pipeline_preset_bytes
                    : readFileBytes(dependencies_.resolve_document_path(reference));
                auto source = OrderedJson::parse(bytes);
                const auto &config = from_feature ? featureSourceFragment(source, provenance->provider_feature)
                    : from_preset ? source.at("config") : source;
                const auto declarations = config.find("compute_tasks");
                if (declarations == config.end()) throw std::runtime_error("The task is generated, not declared in this document");
                const auto original = std::find_if(declarations->begin(), declarations->end(),
                    [&](const auto &candidate) { return candidate.at("name") == name; });
                if (original == declarations->end()) throw std::runtime_error("The task is no longer present in its source document");
                entry["declaration"] = *original;
                entry["source_digest"] = digestJson(renderConfigSourceDigest(bytes));
                entry["editable"] = true;
            } catch (const std::exception &error) {
                entry["read_only_reason"] = error.what();
            }
        }
        // Qt's JSON transport cannot represent every uint64 task value as a number.
        entry["declaration_json"] = entry.at("declaration").dump(2);
        tasks.push_back(std::move(entry));
    }
    auto resources = OrderedJson::array();
    for (const auto &[field, kind] : std::array{
             std::pair{"render_targets", "image"}, std::pair{"buffers", "buffer"}}) {
        for (const auto &resource : context.config.value(field, Json::array())) {
            const auto name = resource.is_string() ? resource.get<std::string>()
                                                   : resource.at("name").get<std::string>();
            resources.push_back({{"name", name}, {"kind", kind},
                                 {"declaration_json", resource.dump(2)}});
        }
    }
    return {{"source_digest", digestJson(document_.sourceDigest())}, {"tasks", std::move(tasks)},
            {"resources", std::move(resources)}};
}

OrderedJson RenderConfigEditorService::getFrameNodeOrder(const Json &params) const {
    requireOnly(params, {}, "get_frame_node_order");
    return {{"source_digest", digestJson(document_.sourceDigest())},
            {"published_generation", dependencies_.runtime_snapshot().published_generation},
            {"orders", Json::parse(document_.bytes()).value("frame_node_order", Json::object())}};
}

OrderedJson RenderConfigEditorService::editFrameNodeOrder(const Json &params) {
    constexpr auto method = "edit_frame_node_order";
    requireOnly(params, {"base_source_digest", "orders"}, method);
    const auto digest = requireString(params, "base_source_digest", method);
    if (!params.contains("orders") || !params.at("orders").is_object())
        return rejection("invalid_frame_node_order", "Provide an object mapping graph names to node arrays");
    return queueRootEdit(digest, method,
        [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
            candidate = candidate.withRootValue("frame_node_order", params.at("orders").dump(2));
            return std::nullopt;
        });
}

OrderedJson RenderConfigEditorService::editComputeTask(const Json &params) {
    requireOnly(params, {"base_source_digest", "base_task_source_digest", "task_name", "task"}, "edit_compute_task");
    return changeComputeTask(params, false);
}

OrderedJson RenderConfigEditorService::removeComputeTask(const Json &params) {
    requireOnly(params, {"base_source_digest", "base_task_source_digest", "task_name"}, "remove_compute_task");
    return changeComputeTask(params, true);
}

OrderedJson RenderConfigEditorService::addComputeTask(const Json &params) {
    constexpr auto method = "add_compute_task";
    requireOnly(params, {"base_source_digest", "destination_reference", "task"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto destination = requireString(params, "destination_reference", method);
    if (!params.contains("task") || !params.at("task").is_object())
        return rejection("invalid_compute_task", "Provide a task declaration object");
    const auto name = requireString(params.at("task"), "name", method);
    if (name.empty()) return rejection("invalid_compute_task", "The task name must not be empty");
    if (!destination.starts_with("project://") || destination.find('#') != std::string::npos)
        return rejection("invalid_task_destination", "Choose a new project:// file for the task feature");
    try {
        const auto catalog = getComputeTaskAuthoring(Json::object());
        for (const auto &task : catalog.at("tasks"))
            if (task.at("name") == name)
                return rejection("compute_task_exists", "A task already uses this name: " + name);
        const auto path = dependencies_.resolve_document_path(destination);
        if (std::filesystem::exists(path))
            return rejection("task_destination_exists", "The destination already exists: " + destination);
        const OrderedJson feature{{"schema", "pelican.render_feature"}, {"version", 1},
            {"name", name}, {"compute_tasks", OrderedJson::array({params.at("task")})}};
        const std::array documents{RenderConfigCandidateDocument{
            .reference = destination, .normalized_reference = dependencies_.normalize_reference(destination),
            .path = path, .operation = RenderConfigDocumentOperation::create,
            .expected = {RenderConfigDocumentExistence::missing, {}}, .bytes = feature.dump(2)}};
        return queueRootEdit(base_digest, method,
            [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                if (!candidate.hasFeaturesArray())
                    candidate = AuthoredRenderConfigDocument::initialize(candidate.bytes());
                candidate = candidate.withFeatureAdded(destination);
                return std::nullopt;
            }, documents);
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what());
    }
}

OrderedJson RenderConfigEditorService::changeComputeTask(const Json &params, bool remove) {
    const auto method = remove ? "remove_compute_task" : "edit_compute_task";
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto task_digest = requireString(params, "base_task_source_digest", method);
    const auto name = requireString(params, "task_name", method);
    if (!isLowerHexDigest(base_digest) || !isLowerHexDigest(task_digest))
        throw std::invalid_argument(std::string{method} + " requires 64 lowercase hex source digests");
    if (!remove && (!params.contains("task") || !params.at("task").is_object() || params.at("task").value("name", std::string{}) != name))
        return rejection("invalid_compute_task", "The task declaration must retain its name");
    try {
        const auto catalog = getComputeTaskAuthoring(Json::object());
        const auto &tasks = catalog.at("tasks");
        const auto selected = std::find_if(tasks.begin(), tasks.end(), [&](const auto &entry) { return entry.at("name") == name; });
        if (selected == tasks.end() || !selected->at("editable").get<bool>())
            return rejection("compute_task_not_editable", "Select a task declared in a project-owned document");
        if (selected->at("source_digest").at("hex") != task_digest)
            return rejection("external_modification", "The task source changed since it was read");
        const auto reference = selected->at("source_reference").get<std::string>();
        const bool root = reference == dependencies_.source_reference;
        const auto path = root ? dependencies_.source_path : dependencies_.resolve_document_path(reference);
        const auto bytes = root ? document_.bytes() : readFileBytes(path);
        if (renderConfigSourceDigest(bytes) != task_digest)
            return rejection("external_modification", "The task source changed while the edit was prepared");
        auto source = OrderedJson::parse(bytes);
        const bool preset = source.value("schema", std::string{}) == "pelican.render_pipeline";
        auto &config = source.value("schema", std::string{}) == "pelican.render_feature"
            ? featureSourceFragment(source, selected->at("provider_feature").get<std::string>())
            : preset ? source.at("config") : source;
        auto &declarations = config.at("compute_tasks");
        const auto original = std::find_if(declarations.begin(), declarations.end(), [&](const auto &entry) { return entry.at("name") == name; });
        if (original == declarations.end()) return rejection("compute_task_not_editable", "The source task is absent");
        if (remove) declarations.erase(original);
        else *original = params.at("task");
        if (root) {
            return queueRootEdit(base_digest, method,
                [&](AuthoredRenderConfigDocument &candidate) -> std::optional<OrderedJson> {
                    candidate = candidate.withRootValue("compute_tasks", declarations.dump(2));
                    return std::nullopt;
                });
        }
        const std::array documents{RenderConfigCandidateDocument{
            .reference = reference, .normalized_reference = dependencies_.normalize_reference(reference),
            .path = path, .operation = RenderConfigDocumentOperation::replace,
            .expected = {RenderConfigDocumentExistence::present, task_digest}, .bytes = source.dump(2)}};
        return queueRootEdit(base_digest, method,
            [](AuthoredRenderConfigDocument &) -> std::optional<OrderedJson> { return std::nullopt; }, documents);
    } catch (const std::exception &error) {
        return rejection("render_pipeline_preflight_failed", error.what());
    }
}

OrderedJson RenderConfigEditorService::editRegionReplacement(const Json &params) {
    constexpr auto method = "edit_region_replacement";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "graph", "region", "provider"}, method);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const auto preset_digest = requireString(params, "base_preset_digest", method);
    const auto graph_name = requireString(params, "graph", method);
    const auto region = requireString(params, "region", method);
    if (!isLowerHexDigest(base_digest) || !isLowerHexDigest(preset_digest))
        throw std::invalid_argument("edit_region_replacement requires 64 lowercase hex source digests");
    if (!params.contains("provider") ||
        (!params.at("provider").is_null() &&
         (!params.at("provider").is_string() || params.at("provider").get_ref<const std::string &>().empty())))
        throw std::invalid_argument("provider must be a non-empty name or null to use the authored nodes");
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference || !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "edit_region_replacement requires the project document resolver");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (!current.pipeline_preset || !current.pipeline_preset_bytes ||
            !current.pipeline_preset->reference.starts_with("project://"))
            return rejection("project_preset_required", "Fork the selected preset into the project before editing region replacements");
        if (renderConfigSourceDigest(*current.pipeline_preset_bytes) != preset_digest)
            return rejection("external_modification", "The selected preset changed since it was read");
        auto preset = OrderedJson::parse(*current.pipeline_preset_bytes);
        auto &graphs = preset.at("config").at("rendering_passes");
        auto graph = std::find_if(graphs.begin(), graphs.end(), [&](const auto &value) {
            return value.at("name") == graph_name;
        });
        if (graph == graphs.end())
            return rejection("render_graph_not_found", "The selected graph is not declared in the project preset");
        const auto &passes = graph->at("passes");
        const bool declared = std::any_of(passes.begin(), passes.end(), [&](const auto &pass) {
            const auto tags = pass.find("regions");
            return tags != pass.end() && std::find(tags->begin(), tags->end(), region) != tags->end();
        });
        if (!declared)
            return rejection("region_not_found", "The selected region is not declared in the project preset");
        auto requests = graph->value("region_replacements", OrderedJson::array());
        const auto request = std::find_if(requests.begin(), requests.end(), [&](const auto &value) {
            return value.at("region") == region;
        });
        // Removing the request restores the authored graph, including edits made
        // while a replacement was selected. Never materialize provider output here.
        if (params.at("provider").is_null()) {
            if (request != requests.end()) requests.erase(request);
        } else if (request != requests.end()) {
            (*request)["provider"] = params.at("provider");
        } else {
            requests.push_back({{"region", region}, {"provider", params.at("provider")}});
        }
        if (requests.empty()) graph->erase("region_replacements");
        else (*graph)["region_replacements"] = std::move(requests);
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

OrderedJson RenderConfigEditorService::editPresetPass(const Json &params) {
    constexpr auto method = "edit_preset_pass";
    requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "draw", "raster_state", "ray_query", "material_settings", "material_routing"}, method);
    (void)parseRasterRayQuery(params);
    const auto base_digest = requireString(params, "base_source_digest", method);
    const bool feature_edit = params.contains("base_feature_digest");
    if (feature_edit && params.contains("base_preset_digest"))
        throw std::invalid_argument("edit_preset_pass requires only the selected source document digest");
    const auto preset_digest = requireString(params, feature_edit ? "base_feature_digest" : "base_preset_digest", method);
    const auto graph = requireString(params, "graph", method);
    const auto name = requireString(params, "pass_name", method);
    if (!isLowerHexDigest(base_digest) || !isLowerHexDigest(preset_digest))
        throw std::invalid_argument("edit_preset_pass requires 64 lowercase hex source digests");
    if (!dependencies_.resolve_authoring_context || !dependencies_.normalize_reference || !dependencies_.resolve_document_path)
        return rejection("method_unavailable", "edit_preset_pass requires the project document resolver");
    try {
        const auto current = dependencies_.resolve_authoring_context(currentDocumentSet());
        if (!feature_edit && (!current.pipeline_preset || !current.pipeline_preset_bytes ||
            !current.pipeline_preset->reference.starts_with("project://")))
            return rejection("project_preset_required", "Fork the selected preset into the project before editing its passes");
        std::string reference;
        std::string bytes;
        std::string authored_name = name;
        std::string provider;
        if (feature_edit) {
            if (params.contains("material_routing"))
                return rejection("feature_routing_not_editable", "Material routing belongs to the rendering preset, not a feature pass");
            bool graph_contains_pass = false;
            for (const auto &candidate_graph : current.config.at("rendering_passes"))
                if (candidate_graph.at("name") == graph)
                    for (const auto &node : candidate_graph.at("passes"))
                        if (node.at("name") == name) graph_contains_pass = true;
            const auto source = std::find_if(current.pass_provenance.begin(), current.pass_provenance.end(),
                [&](const auto &entry) { return entry.name == name && entry.source == RenderPipelineProvenanceSource::feature; });
            if (!graph_contains_pass || source == current.pass_provenance.end() || source->provider_reference.empty() ||
                source->provider_reference.starts_with("engine://"))
                return rejection("feature_pass_not_editable", "The selected node is not a project feature pass in this graph");
            reference = source->provider_reference;
            provider = source->provider_feature;
            if (!source->authored_name.empty() && source->authored_name != name)
                return rejection("feature_pass_not_editable", "Generated feature variants must be edited in their source document");
            if (!source->authored_name.empty()) authored_name = source->authored_name;
            bytes = readFileBytes(dependencies_.resolve_document_path(reference));
        } else {
            reference = current.pipeline_preset->reference;
            bytes = *current.pipeline_preset_bytes;
        }
        if (renderConfigSourceDigest(bytes) != preset_digest)
            return rejection("external_modification", "The selected pass source changed since it was read");
        auto preset = OrderedJson::parse(bytes);
        auto *pass = feature_edit ? findFeatureEditablePass(preset, authored_name, provider) : findPresetEditablePass(preset, graph, name);
        if (!pass) return rejection("preset_pass_not_editable", "The selected node is not an editable pass in the project preset");
        if (isEditableAuxiliaryPass(pass->at("type").get<std::string>())) {
            requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "output", "shader", "ray_query"}, method);
            bool changed = false;
            for (const auto *key : {"output", "shader", "ray_query"}) {
                if (!params.contains(key)) continue;
                changed = true;
                if (std::string_view{key} == "shader") {
                    const auto type = pass->at("type").get<std::string>();
                    if (type == "shadow_depth") requireOnly(params.at(key), {"vertex"}, "shadow_depth shader");
                    else if (type == "velocity" || type == "picking")
                        requireOnly(params.at(key), {"vertex", "fragment", "skinned_vertex"}, "geometry shader");
                    else requireOnly(params.at(key), {"vertex", "fragment"}, "auxiliary shader");
                    if (!pass->contains(key)) {
                        (*pass)[key] = {{"vertex", "engine://" + type}};
                        if (type != "shadow_depth") (*pass)[key]["fragment"] = "engine://" + type;
                    }
                    for (const auto &[field, value] : params.at(key).items()) {
                        if (field == "skinned_vertex" && value.is_null()) (*pass)[key].erase(field);
                        else (*pass)[key][field] = value;
                    }
                } else (*pass)[key] = params.at(key);
            }
            if (!changed) throw std::invalid_argument("Auxiliary pass editing requires at least one editable field");
        } else if (pass->at("type") == "raster") {
            requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "draw", "raster_state", "ray_query"}, method);
            bool changed = false;
            for (const auto *key : {"input", "output", "shader", "draw", "raster_state", "ray_query"}) {
                if (!params.contains(key)) continue;
                changed = true;
                if (std::string_view{key} == "raster_state" && params.at(key).is_null()) pass->erase(key);
                else (*pass)[key] = params.at(key);
            }
            if (!changed) throw std::invalid_argument("Raster pass editing requires at least one editable field");
            const auto &output = pass->at("output");
            const auto &color = output.at("color");
            (void)parseRasterPassContract(Json(*pass), color.is_array() ? color.size() : 1,
                output.contains("depth") && !output.at("depth").is_null(), "edit_preset_pass");
        } else if (pass->at("type") == "material") {
            requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "ray_query", "material_settings", "material_routing"}, method);
            if (!params.contains("input") && !params.contains("output") && !params.contains("ray_query") &&
                !params.contains("material_settings") && !params.contains("material_routing"))
                throw std::invalid_argument("Material pass editing requires at least one editable field");
            for (const auto *key : {"input", "output"})
                if (params.contains(key)) (*pass)[key] = params.at(key);
            if (params.contains("material_settings")) {
                const auto &settings = params.at("material_settings");
                requireOnly(settings, {"material_range", "material_filter", "material_variant", "material_contract",
                    "gpu_draw_source", "material_outputs", "material_output_states", "screen_inputs",
                    "surface_resources", "material_resources"}, "material_settings");
                for (const auto &[key, value] : settings.items()) {
                    if (value.is_null()) pass->erase(key);
                    else (*pass)[key] = value;
                }
            }
            if (params.contains("material_routing")) {
                if (params.at("material_routing").is_null()) preset.at("config").erase("material_routing");
                else preset.at("config")["material_routing"] = params.at("material_routing");
            }
        } else {
            requireOnly(params, {"base_source_digest", "base_preset_digest", "base_feature_digest", "graph", "pass_name", "input", "output", "shader", "ray_query"}, method);
            for (const auto *key : {"input", "output", "shader"}) {
                if (!params.contains(key)) throw std::invalid_argument(std::string{method} + " requires " + key);
                (*pass)[key] = params.at(key);
            }
        }
        if (params.contains("ray_query")) (*pass)["ray_query"] = params.at("ray_query");
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
            documents, feature_edit ? std::nullopt : std::optional{preset_digest});
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
            if (!ticket.edits_additional_documents && ticket.candidate.sourceDigest() ==
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
