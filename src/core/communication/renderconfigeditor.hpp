#pragma once

#include "../../project/renderconfigdocument.hpp"
#include "../../project/renderpipeline.hpp"
#include "renderconfigtransaction.hpp"
#include "editorgate.hpp"
#include "../userpublic/valuegraph.hpp"
#include "../vkcore/renderer_config.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Pelican {

struct RenderFeatureCatalogEntry {
    std::string name;
    std::string reference;
    bool requires_runtime_module = false;
    bool available = false;
    std::string unavailable_reason;
    // X-1: where the document came from.  "engine" for the shared set built
    // into the binary, "package" for one vendored under
    // `project://packages/<namespace>/`.  The catalog is engine + package and
    // nothing else; a loose `project://features/*.json` stays off it.
    std::string source = "engine";

    bool operator==(const RenderFeatureCatalogEntry &) const = default;
};

struct RenderConfigEditorGateObservation {
    bool can_edit = true;
    std::uint64_t transition_epoch = 0;
    std::vector<std::string> reasons;

    bool operator==(const RenderConfigEditorGateObservation &) const = default;
};

struct RenderConfigRuntimeSnapshot {
    std::uint64_t published_generation = 0;
    std::vector<std::string> enabled_feature_names;
};

struct RenderConfigRuntimeApplyResult {
    bool committed = false;
    bool staged = false;
    std::uint64_t staged_generation = 0;
    std::uint64_t published_generation = 0;
    std::string error;
    std::string post_commit_error;
    // Q-14e: the exception `error` was read from, when there was one. The
    // editor reads a typed refusal's fields from it
    // (render_pipeline_preflight_failed's shader_input_mismatch).
    std::exception_ptr exception;
};

using RenderConfigSourceCommit = std::function<void()>;
using RenderConfigRuntimeApplyCompletion =
    std::function<void(RenderConfigRuntimeApplyResult)>;

struct ResolvedRenderConfigAuthoringContext {
    nlohmann::json config = nlohmann::json::object();
    std::optional<RenderPipelinePresetInfo> pipeline_preset;
    std::vector<RenderPassProvenance> pass_provenance;
    std::optional<std::string> pipeline_preset_bytes;
    // F-1: which document each composed render target / buffer came from.
    // A resolver that leaves it empty makes every target that is not in the
    // root, the preset or the managed fragment read as "composed".
    std::vector<RenderResourceProvenance> resource_provenance;
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
        RenderConfigSourceCommit,
        RenderConfigRuntimeApplyCompletion)>
        apply_candidate;
    std::function<ResolvedRenderConfigAuthoringContext(
        const RenderConfigCandidateDocumentSet &)>
        resolve_authoring_context;
    // G-6: composes an edit's candidate with every graph variant the engine
    // registered at start (flat and preview, xr when the launch activated
    // it) before the edit takes a ticket. A variant that cannot compose it
    // throws GraphVariantCompositionError; the edit is refused as
    // render_pipeline_preflight_failed with that `variant`, and no document
    // changes. LP-0d: a named refusal (a GraphRegistryError, or a carried
    // "<code>: <fields>" one) keeps its own code and fields instead, with
    // `variant` beside them. Reading the context never calls it. Unset: no variant is
    // composed here, and the runtime's apply is the only check.
    std::function<void(const RenderConfigCandidateDocumentSet &)>
        preflight_graph_variants;
    std::function<std::vector<RenderFeatureCatalogEntry>()> feature_catalog;
    // The instance input values the last published draw generation used. Left
    // unset outside a running engine, and then no values are reported: the
    // service never evaluates a graph of its own to fill the gap.
    std::function<ValueGraph::PublishedInstanceInputs()> graph_values;
};

// Engine-owned render-authoring document/ticket service. Feature toggles edit
// only the lossless root span; authored passes use a candidate document set
// and a multi-file commit that owns marked fragments in its fixed namespace.
class RenderConfigEditorService {
    struct Ticket {
        std::string id;
        std::uint64_t accepted_gate_epoch = 0;
        std::string base_source_digest;
        AuthoredRenderConfigDocument candidate;
        std::optional<RenderConfigCandidateDocumentSet> candidate_documents;
        std::string operation;
        std::string fragment_reference;
        std::optional<std::string> base_preset_digest;
        bool edits_additional_documents = false;
        // N-6: the edit changes authored bytes nothing in the engine reads, so
        // there is no candidate to compile and nothing new to publish. It still
        // takes a ticket and the same CAS; it simply does not go to the runtime.
        bool source_only = false;
        // G-4: the edit changes material_route_default, which the engine
        // reads once at start. The committed result says restart_required.
        bool restart_required = false;
    };

    struct ManagedDocumentBaseline {
        std::string reference;
        std::string normalized_reference;
        std::filesystem::path path;
        std::string bytes;
        std::string digest;
    };

    struct CommitTransaction {
        Ticket ticket;
        std::optional<RenderConfigDocumentCommitReceipt>
            source_receipt;
        bool source_commit_called = false;
        bool completion_arrived = false;
        std::string source_external_modification;

        explicit CommitTransaction(Ticket value)
            : ticket{std::move(value)} {}
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
    EditorGateEpoch<RenderConfigEditorGateObservation> gate_tracker_;
    std::shared_ptr<const void> lifetime_ =
        std::make_shared<const int>(0);
    std::optional<std::string> inflight_ticket_;

    struct GateSnapshot {
        bool can_edit = true;
        std::uint64_t epoch = 1;
        std::vector<std::string> reasons;
    };

    GateSnapshot gateSnapshot();
    nlohmann::ordered_json changeComputeTask(const nlohmann::json &params, bool remove);
    nlohmann::ordered_json materializeRenderFeature(const std::string &base_digest, const nlohmann::json &operation);
    // F-6: edit_preset_pass's third destination, a pass the direct root
    // declares itself. One root document, the ordinary root-edit ticket.
    nlohmann::ordered_json editRootPass(
        const nlohmann::json &params, const std::string &base_digest,
        const std::string &graph, const std::string &pass_name,
        const ResolvedRenderConfigAuthoringContext &current);
    nlohmann::ordered_json queueRootEdit(
        const std::string &base_digest, const std::string &operation,
        const std::function<std::optional<nlohmann::ordered_json>(AuthoredRenderConfigDocument &)> &transform,
        std::span<const RenderConfigCandidateDocument> additional_documents = {},
        std::optional<std::string> base_preset_digest = std::nullopt,
        bool source_only = false);
    // The authoring precondition preamble shared by queueRootEdit and
    // addAuthoredPass: gate -> base source digest CAS -> re-read-from-disk
    // CAS, in that order.  Returns the rejection the RPC answers with, or
    // nullopt when the edit may proceed.  The gate snapshot stays with the
    // caller because the accepted ticket records its epochs.
    std::optional<nlohmann::ordered_json> checkAuthoringPreconditions(
        const GateSnapshot &gate, const std::string &base_digest,
        std::string_view method,
        std::string_view gate_closed_message) const;
    // The authored-pass tail shared by addAuthoredPass and
    // removeAuthoredPass: candidate-set preflight -> ticket enqueue ->
    // accepted response.  The operation name is the only difference.
    // `edits_additional_documents` is set by an edit whose root can stay
    // byte-identical (edit_render_targets on an existing fragment): without it
    // the commit would answer "no change" and never write the fragment.
    // `check` (G-1) judges the composed candidate further; what it throws is
    // refused as render_pipeline_preflight_failed like a composition error.
    using AuthoredCandidateCheck =
        std::function<void(const ResolvedRenderConfigAuthoringContext &)>;
    nlohmann::ordered_json enqueueAuthoredPassTicket(
        std::string operation, const GateSnapshot &gate,
        const std::string &base_digest, AuthoredRenderConfigDocument root,
        RenderConfigCandidateDocumentSet candidate_documents,
        const std::string &reference,
        bool edits_additional_documents = false,
        const AuthoredCandidateCheck &check = {},
        bool restart_required = false);
    nlohmann::ordered_json rejection(
        std::string code, std::string message,
        nlohmann::ordered_json details = nlohmann::ordered_json::object()) const;
    // render_pipeline_preflight_failed in the exception's words, with the
    // fields a typed refusal carries (Q-14e: shader_input_mismatch) added to
    // `details`.
    nlohmann::ordered_json preflightRejection(
        const std::exception &error,
        nlohmann::ordered_json details = nlohmann::ordered_json::object()) const;
    // G-6: the candidate through preflight_graph_variants. Returns the
    // refusal naming the variant that could not compose it, or nullopt.
    // `details` are the fields the caller's own preflight refusals carry.
    std::optional<nlohmann::ordered_json> preflightGraphVariants(
        const RenderConfigCandidateDocumentSet &candidate,
        nlohmann::ordered_json details) const;
    const RenderFeatureCatalogEntry *findCatalogReference(
        std::string_view reference) const noexcept;
    RenderConfigCandidateDocumentSet currentDocumentSet() const;
    void loadManagedDocumentBaselines();
    void completeCommitTransaction(
        const std::shared_ptr<CommitTransaction> &transaction,
        RenderConfigRuntimeApplyResult applied) noexcept;

  public:
    nlohmann::ordered_json getGraphInstances(const nlohmann::json &params) const;
    nlohmann::ordered_json editGraphInstances(const nlohmann::json &params);
    nlohmann::ordered_json editGraphDocument(const nlohmann::json &params);
    // LP-3b: a one-piece material pass of the rendering section or of a
    // forked (project) preset, rewritten as a render.material_pass with the
    // same name and place, render.target_ref nodes, a draw_loop graph.loop
    // and its body definition (materialpassdecompose.hpp) -- the section or
    // the preset and the graph in one ticket.
    nlohmann::ordered_json decomposeMaterialPass(const nlohmann::json &params);
    nlohmann::ordered_json getGraphValues(const nlohmann::json &params) const;
    // AG-3a. Every node in one scope of the published document with what the
    // drawn frame's evaluation gave it -- the definition-scope values
    // get_graph_values cannot answer for, since that one answers per instance
    // input port. `instance` omitted or empty is the root scope.
    nlohmann::ordered_json getGraphNodeValues(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphCatalog(const nlohmann::json &params) const;
    nlohmann::ordered_json getRayCallableRecords(const nlohmann::json &params) const;
    nlohmann::ordered_json forkRenderPipeline(const nlohmann::json &params);
    nlohmann::ordered_json editPresetPass(const nlohmann::json &params);
    nlohmann::ordered_json editRegionReplacement(const nlohmann::json &params);
    nlohmann::ordered_json getComputeTaskAuthoring(const nlohmann::json &params) const;
    nlohmann::ordered_json getFrameNodeOrder(const nlohmann::json &params) const;
    nlohmann::ordered_json editFrameNodeOrder(const nlohmann::json &params);
    // G-4: set the rendering root's material_route_default ("auto" or
    // "forward") through the ordinary root-edit ticket. The committed result
    // carries restart_required: true.
    nlohmann::ordered_json setMaterialRouteDefault(const nlohmann::json &params);
    nlohmann::ordered_json editComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json addComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json removeComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json editRayCallableRecords(const nlohmann::json &params);
    nlohmann::ordered_json getSceneAccelerationStructures(const nlohmann::json &params) const;
    nlohmann::ordered_json editSceneAccelerationStructures(const nlohmann::json &params);
    explicit RenderConfigEditorService(
        RenderConfigEditorDependencies dependencies);
    ~RenderConfigEditorService();

    nlohmann::ordered_json getRenderFeatures(
        const nlohmann::json &params) const;
    nlohmann::ordered_json listRenderFeatures(
        const nlohmann::json &params) const;
    nlohmann::ordered_json editRenderFeatures(
        const nlohmann::json &params);
    // P-2a-3: publish one materialized feature as a shared component under
    // `user://components/<name>/`, and take one back into another project.
    nlohmann::ordered_json exportRenderFeature(
        const nlohmann::json &params);
    nlohmann::ordered_json importRenderFeature(
        const nlohmann::json &params);
    // X-1: vendor a package directory into `project://packages/<namespace>/`
    // and take it back out.  Neither touches `code/`; the CMake side finds a
    // vendored package by directory.
    nlohmann::ordered_json installPackage(const nlohmann::json &params);
    nlohmann::ordered_json uninstallPackage(const nlohmann::json &params);
    nlohmann::ordered_json getRenderAuthoringContext(
        const nlohmann::json &params) const;
    nlohmann::ordered_json addAuthoredPass(
        const nlohmann::json &params);
    nlohmann::ordered_json removeAuthoredPass(
        const nlohmann::json &params);
    // F-1: add / replace / remove render targets in the one managed fragment
    // `project://passes/authoring/render-targets.json`, through the same
    // candidate-set preflight and ticket as add_authored_pass.
    nlohmann::ordered_json editRenderTargets(
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

// X-1: the feature documents every installed package provides.  Unlike the
// engine enumerator this one never throws: a package is bytes a user brought
// in, and a broken one must not stop the editor service from being
// constructed.  Every failure becomes one entry with `available: false` and an
// `unavailable_reason`.
std::vector<RenderFeatureCatalogEntry>
enumeratePackageRenderFeatureDocuments(
    const std::filesystem::path &project_root,
    const std::function<RenderFeatureRuntimeAvailability(
        std::string_view, const nlohmann::json &)> &availability) noexcept;

// Engine documents first, then packages, with a package entry that would
// collide with a name already in the set reported as unavailable instead of
// throwing.
std::vector<RenderFeatureCatalogEntry>
composeRenderFeatureCatalog(
    std::vector<RenderFeatureCatalogEntry> engine_entries,
    std::vector<RenderFeatureCatalogEntry> package_entries);

} // namespace Pelican
