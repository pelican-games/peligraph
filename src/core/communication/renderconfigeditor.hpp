#pragma once

#include "../../project/renderconfigdocument.hpp"
#include "../../project/renderpipeline.hpp"
#include "../vkcore/renderer_config.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Pelican {

struct RenderFeatureCatalogEntry {
    std::string name;
    std::string reference;
    bool requires_runtime_module = false;
    bool available = false;
    std::string unavailable_reason;

    bool operator==(const RenderFeatureCatalogEntry &) const = default;
};

struct RenderConfigEditorGateObservation {
    bool can_edit = true;
    std::uint64_t transition_epoch = 0;
    std::vector<std::string> reasons;
};

struct RenderConfigRuntimeSnapshot {
    std::uint64_t published_generation = 0;
    std::vector<std::string> enabled_feature_names;
};

struct RenderConfigRuntimeApplyResult {
    bool committed = false;
    std::uint64_t published_generation = 0;
    std::string error;
    std::string post_commit_error;
};

using RenderConfigSourceCommit = std::function<void()>;

struct ResolvedRenderConfigAuthoringContext {
    nlohmann::json config = nlohmann::json::object();
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    std::vector<RenderPassProvenance> pass_provenance;
};

struct RenderConfigEditorDependencies {
    std::string source_reference;
    std::filesystem::path source_path;
    std::string source_bytes;
    std::filesystem::path project_root;
    std::function<std::string(std::string_view)> normalize_reference;
    // Resolves a project document whether or not it exists yet. Engine and
    // fragment-address references must be rejected by the implementation.
    std::function<std::filesystem::path(std::string_view)>
        resolve_document_path;
    std::function<RenderConfigEditorGateObservation()> gate;
    std::function<RenderConfigRuntimeSnapshot()> runtime_snapshot;
    // Production Renderer compiles and validates the private candidate first,
    // invokes source_commit as its final fallible pre-publication action, then
    // publishes the runtime generation without throwing.
    std::function<RenderConfigRuntimeApplyResult(
        RenderConfigCandidateDocumentSet,
        const RenderConfigSourceCommit &)>
        apply_candidate;
    std::function<ResolvedRenderConfigAuthoringContext(
        const RenderConfigCandidateDocumentSet &)>
        resolve_authoring_context;
    std::function<std::vector<RenderFeatureCatalogEntry>()> feature_catalog;
};

// Engine-owned render-authoring document/ticket service. Feature toggles edit
// only the lossless root span; authored passes use a candidate document set
// and a multi-file commit that owns marked fragments in its fixed namespace.
class RenderConfigEditorService {
    struct Ticket {
        std::string id;
        std::uint64_t accepted_gate_epoch = 0;
        std::uint64_t accepted_transition_epoch = 0;
        std::string base_source_digest;
        AuthoredRenderConfigDocument candidate;
        std::optional<RenderConfigCandidateDocumentSet> candidate_documents;
        std::string operation;
        std::string fragment_reference;
    };

    struct ManagedDocumentBaseline {
        std::string reference;
        std::string normalized_reference;
        std::filesystem::path path;
        std::string bytes;
        std::string digest;
    };

    RenderConfigEditorDependencies dependencies_;
    AuthoredRenderConfigDocument document_;
    std::vector<RenderFeatureCatalogEntry> feature_catalog_;
    std::vector<Ticket> pending_;
    std::unordered_map<std::string, nlohmann::ordered_json> results_;
    std::vector<nlohmann::ordered_json> completed_;
    std::unordered_map<std::string, ManagedDocumentBaseline>
        managed_documents_;
    std::string source_normalized_reference_;
    std::uint64_t next_ticket_ = 1;
    std::uint64_t gate_epoch_ = 1;
    std::uint64_t observed_transition_epoch_ = 0;
    bool observed_can_edit_ = true;
    std::vector<std::string> observed_gate_reasons_;
    bool gate_initialized_ = false;

    struct GateSnapshot {
        bool can_edit = true;
        std::uint64_t epoch = 1;
        std::uint64_t transition_epoch = 0;
        std::vector<std::string> reasons;
    };

    GateSnapshot gateSnapshot();
    nlohmann::ordered_json rejection(
        std::string code, std::string message,
        nlohmann::ordered_json details = nlohmann::ordered_json::object()) const;
    const RenderFeatureCatalogEntry *findCatalogReference(
        std::string_view reference) const noexcept;
    RenderConfigCandidateDocumentSet currentDocumentSet() const;
    void loadManagedDocumentBaselines();

  public:
    explicit RenderConfigEditorService(
        RenderConfigEditorDependencies dependencies);

    nlohmann::ordered_json getRenderFeatures(
        const nlohmann::json &params) const;
    nlohmann::ordered_json listRenderFeatures(
        const nlohmann::json &params) const;
    nlohmann::ordered_json editRenderFeatures(
        const nlohmann::json &params);
    nlohmann::ordered_json getRenderAuthoringContext(
        const nlohmann::json &params) const;
    nlohmann::ordered_json addAuthoredPass(
        const nlohmann::json &params);
    nlohmann::ordered_json removeAuthoredPass(
        const nlohmann::json &params);
    nlohmann::ordered_json getResult(
        const nlohmann::json &params) const;
    bool ownsTicket(std::string_view ticket) const noexcept;
    void commitPending() noexcept;
    std::vector<nlohmann::ordered_json> takeCompletedResults();

    const AuthoredRenderConfigDocument &documentForTesting() const noexcept {
        return document_;
    }
};

std::vector<RenderFeatureCatalogEntry>
enumerateEngineRenderFeatureDocuments(
    const std::function<RenderFeatureRuntimeAvailability(
        std::string_view, const nlohmann::json &)> &availability);

} // namespace Pelican
