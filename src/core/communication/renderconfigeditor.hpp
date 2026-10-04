#pragma once

#include "../../project/materialdrawloop.hpp"
#include "../../project/projectpathresolver.hpp"
#include "../../project/renderconfigdocument.hpp"
#include "../../project/renderpipeline.hpp"
#include "renderconfigtransaction.hpp"
#include "editorgate.hpp"
#include "graphfoldverify.hpp"
#include "../userpublic/valuegraph.hpp"
#include "../vkcore/renderer_config.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <future>
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

// RD-1: get_render_documents / restore_render_documents named a reference
// that is not a render document the current composition writes. Fields:
// {method, reference, reason}; reason is "engine_document",
// "fragment_address", "outside_project", "not_render_document" or (RD-1b,
// restore only) "root_document" for a request to delete the root.
inline constexpr std::string_view renderDocumentNotWritableCode =
    "render_document_not_writable";

// RD-1b: get_render_documents named a document inside the project that is
// not there now (a fragment an edit deleted, or one an undo would create).
// Fields: {method, reference}. Its restore_render_documents base_digest is
// null.
inline constexpr std::string_view renderDocumentMissingCode =
    "render_document_missing";

// RL-1: reload_render_documents was called while a render edit ticket is
// pending or in flight, or while a scene preview is open. Fields: {method,
// reason: "ticket_pending" | "preview_open", tickets: [...]}.
inline constexpr std::string_view reloadBusyCode = "reload_busy";

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

// EL-1: one region replacement provider edit_region_replacement may name, as
// the subgraph replacement registry resolves names now.
struct RenderAuthoringRegionProvider {
    std::string name;
    // Registered by the engine (the builtin identity provider), not by the
    // game DLL.
    bool engine_owned = false;
    std::uint32_t provider_version = 0;

    bool operator==(const RenderAuthoringRegionProvider &) const = default;
};

// IA-2: a file the published generation read when it was composed and
// does not see change by itself: the asset reference as written, the file it
// resolved to, and the sha256 of the bytes that generation read. A persistent
// render.image's initial asset (IA-2); a value.sample_table's table and the
// `.import.json` beside it, both under the table's reference (IA-3).
struct RenderConfigPublishedAsset {
    std::string asset;
    std::filesystem::path file;
    std::string sha256;
};

// RTL-1a: one material a ray binding may select, spelled as the engine
// matches a scene row (raygrouplayout.cpp selectorMatches): `document` (the
// material's source document, compared as a string) or `asset` with
// `source_material_index` (a glTF material of a model). `name` is the
// registered material name.
struct RaySelectableMaterial {
    std::string name;
    std::string document;
    std::string asset;
    std::optional<std::uint32_t> source_material_index;
};

// RTL-1a: the bindings of the last scene row resolution that matched no
// scene row (get_ray_layout.unresolved_bindings), with the digest of the
// bindings bytes they were resolved from.
struct RayUnresolvedBindingsObservation {
    std::string bindings_reference;
    std::string bindings_digest;
    std::vector<std::size_t> indices;
};

// RTL-1a: the project's asset_data document, whose procedural_geometry
// records edit_ray_layout reads (selectable_procedural_assets) and rewrites
// with the layout (procedural_records).
struct RayAssetDataDocument {
    std::string reference;
    std::filesystem::path path;
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
    // PV-1: the published one-shot preview's state, previewGraphStatusJson
    // ({available} and the reason when not). A committed edit that published
    // reports it as `preview`. Null (a runtime without a renderer): left out.
    nlohmann::json preview = nullptr;
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
    // FG-1: what the root's verified fold groups added to target_planning
    // (RenderFeatureComposeResult::fold_planning). edit_target_planning
    // refuses an explicit opposite of one of them.
    std::vector<GraphFoldPlanningEntry> fold_planning;
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
    // EL-5: resolves a shader stem's candidate reference as the shader
    // library's PathResolver does (resolveProjectRef), for
    // get_shader_interface. Unset: only engine:// stems are answered.
    std::function<ResolvedRef(std::string_view)> resolve_shader_reference;
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
    // EL-1: what a draw_loop judges -- every registered material that is not
    // a variant record, in registration order, as the running generation's
    // draw-loop tables see it (MaterialContainer::drawLoopSubject).
    // get_render_authoring_context lists them as materials[] and
    // material_tags[]. Unset (no running engine): both lists are empty.
    std::function<std::vector<MaterialDrawSubject>()> draw_loop_materials;
    // EL-1: the providers a region replacement may name now
    // (SubgraphReplacementRegistry::providerListing), listed as
    // region_providers[]. Unset: the list is empty.
    std::function<std::vector<RenderAuthoringRegionProvider>()>
        region_providers;
    // RL-1: the documents the published generation was composed from besides
    // the root (its preset and features), {normalized reference: sha256 hex}.
    // reload_render_documents compares a document the editor has not written
    // with it to say whether the document changed. Unset: such a document is
    // reported as changed.
    std::function<std::map<std::string, std::string>()> published_documents;
    // IA-2: the assets the published generation read (render.image
    // `initial: {asset}`; IA-3: value.sample_table's table and its
    // manifest), each with the sha256 of the bytes that generation read.
    // reload_render_documents hashes each file again; one that differs is
    // published anew even when no document changed. Unset: no asset is
    // compared.
    std::function<std::vector<RenderConfigPublishedAsset>()>
        published_assets;
    // BS-1: whether the live generation is the engine's fallback (the
    // project's configuration was refused at start). reload_render_documents
    // then composes what is on disk even when no document changed, since
    // none of it is live. Unset: never.
    std::function<bool()> runtime_fallback;
    // FG-2: draws a verify_fold job's three runs on a worker thread
    // (graphfoldverify.hpp; production: runFoldVerifyProcesses). Unset:
    // verify_fold is fold_verify_unavailable {reason: "no_runner"}.
    FoldVerifyRunner verify_fold;
    // FG-2: why this launch may not start the children now (headless,
    // replay, golden ...), asked on every verify_fold; nullopt: it may.
    std::function<std::optional<std::string>()> verify_fold_unavailable;
    // AB-1: the project's asset stores, which list_derived_assets walks for
    // `*.import.json`. Unset: the project root.
    std::function<std::vector<AssetStoreStatus>()> asset_stores;
    // RTL-1a: the materials the running engine registered that a ray binding
    // can select (get_ray_layout.selectable_materials), with or without a
    // layout. Unset: none.
    std::function<std::vector<RaySelectableMaterial>()> ray_selectable_materials;
    // RTL-1a: the bindings the last resolved scene rows left unmatched.
    // nullopt (or unset): no rows were resolved with bindings.
    std::function<std::optional<RayUnresolvedBindingsObservation>()>
        ray_unresolved_bindings;
    // RTL-1a: where asset_data is. nullopt (or unset): no asset_data, and
    // edit_ray_layout refuses procedural_records.
    std::function<std::optional<RayAssetDataDocument>()> ray_asset_data;
};

// AB-1: what a bake_audio_analysis worker hands back to the engine's thread:
// the committed fields, or a refusal's code, message and fields.
struct AudioAnalysisBakeOutcome {
    bool committed = false;
    nlohmann::ordered_json fields = nlohmann::ordered_json::object();
    std::string code;
    std::string message;
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
        // reads once at start. The committed result says restart_required,
        // with restart_reason {code: "material_route_default_changed"}.
        // RS-1: every ticket that goes to the runtime may also end committed
        // with restart_required when the only live refusal was a live
        // material's physical rendering contract (restart_reason
        // {code: "material_physical_contract_changed", ...}).
        bool restart_required = false;
        // RD-1: restore_render_documents may change which managed fragments
        // the root names, so the baselines are read again after it commits.
        bool reload_managed_baselines = false;
        // RL-1: reload_render_documents. The candidate is what is on disk;
        // nothing is written, and the source commit only checks that every
        // document is still what was read. `reload_documents` is every
        // render document read {reference: sha256 hex}, `reload_changed` the
        // references whose digest differs from what the editor knew.
        bool reload = false;
        bool reload_no_change = false;
        nlohmann::ordered_json reload_documents =
            nlohmann::ordered_json::object();
        nlohmann::ordered_json reload_changed =
            nlohmann::ordered_json::array();
        // {normalized reference: sha256 hex} of what was read.
        std::map<std::string, std::string> reload_digests;
        // IA-2: the initial assets whose bytes differ from what the
        // published generation read, as written in the documents.
        nlohmann::ordered_json reload_changed_assets =
            nlohmann::ordered_json::array();
        // FG-2: fields the committed result carries besides the usual ones
        // (verify_fold's {group, verified}).
        nlohmann::ordered_json result_extras = nlohmann::ordered_json::object();
    };

    // FG-2: one verify_fold between its acceptance and the frame boundary
    // that reads its runs (harvestFoldVerifies).
    struct FoldVerifyTask {
        std::string ticket;
        std::string group;
        std::string base_source_digest;
        // The group's open-form digest when the request was accepted.
        std::string digest;
        std::uint32_t frames = 3;
        std::uint64_t tolerance = 0;
        std::future<FoldVerifyRuns> runs;
    };

    // AB-1: one bake_audio_analysis between its acceptance and the frame
    // boundary that reads its outcome (harvestAudioBakes).
    struct AudioBakeTask {
        std::string ticket;
        std::filesystem::path out;
        std::future<AudioAnalysisBakeOutcome> outcome;
    };

    // RD-1: one render document the current composition can write -- the
    // root, the forked (project) preset, or a project feature document.
    struct WritableRenderDocument {
        std::string reference;
        std::string normalized_reference;
        std::filesystem::path path;
        std::string kind; // "root", "preset" or "feature"
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
        // RS-1: the ticket's preset CAS (empty: none), kept so a candidate
        // the runtime refused only for a live material's physical contract
        // can still be committed, under the same check, for the next start.
        std::function<void()> verify_preset;

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
    // RL-1: the digest of each render document (not the root) as this
    // editor last wrote or reloaded it, {normalized reference: sha256 hex}.
    // A document absent here is compared with published_documents.
    std::map<std::string, std::string> known_document_digests_;
    // RR-1: a reload refused the documents on disk, and the editor holds
    // them (the root's bytes and digest, the managed fragments, the other
    // documents' digests) as the base of the next restore or edit while the
    // live generation stays the one before. Cleared when a ticket publishes
    // a generation composed from what the editor holds.
    bool holds_refused_documents_ = false;
    // RR-1: while the held root is bytes that are not JSON (a reload refused
    // with render_document_syntax@1), the newest root the editor held that
    // reads, and whether it was the live one. The structural reads answer
    // from it (readDocument); the managed baselines stay this root's. Empty
    // otherwise, and on a fallback that started on such bytes.
    std::optional<AuthoredRenderConfigDocument> readable_document_;
    bool readable_document_live_ = false;
    std::uint64_t next_ticket_ = 1;
    EditorGateEpoch<RenderConfigEditorGateObservation> gate_tracker_;
    std::shared_ptr<const void> lifetime_ =
        std::make_shared<const int>(0);
    std::optional<std::string> inflight_ticket_;
    // FG-2: the running verify_fold workers, and the flag that tells them the
    // service (the engine) is going away.
    std::vector<FoldVerifyTask> fold_verifies_;
    std::shared_ptr<std::atomic<bool>> fold_verify_cancelled_ =
        std::make_shared<std::atomic<bool>>(false);
    // AB-1: the running bake_audio_analysis workers.
    std::vector<AudioBakeTask> audio_bakes_;
    void harvestAudioBakes() noexcept;

    struct GateSnapshot {
        bool can_edit = true;
        std::uint64_t epoch = 1;
        std::vector<std::string> reasons;
    };

    GateSnapshot gateSnapshot();
    nlohmann::ordered_json changeComputeTask(const nlohmann::json &params, bool remove);
    // FR-4: a document an edit rewrites (a project preset or feature), as the
    // editor holds it: inside edit_batch, what the batch's earlier edits made
    // of it, with `base_digest` the digest it had before the batch (the one
    // the caller read); otherwise the disk, `base_digest` its digest.
    struct HeldDocument {
        std::string bytes;
        std::string base_digest;
    };
    HeldDocument heldDocument(const std::string &reference) const;
    // RTL-1a: heldDocument for a document that may not be there: `present`
    // false (bytes empty) when neither the batch nor the disk has it;
    // `batch` when an earlier edit of the running edit_batch wrote it, whose
    // `expected` is what the batch holds it to.
    struct HeldOptionalDocument {
        bool present = false;
        bool batch = false;
        std::string bytes;
        RenderConfigDocumentExpectedState expected;
    };
    HeldOptionalDocument heldOptionalDocument(const std::string &reference) const;
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
        bool source_only = false,
        // FG-2: the ticket id to use (verify_fold's, issued when the request
        // was accepted); nullopt: a new one.
        std::optional<std::string> ticket_id = std::nullopt);
    // The authoring precondition preamble shared by queueRootEdit and
    // addAuthoredPass: gate -> base source digest CAS -> re-read-from-disk
    // CAS, in that order.  Returns the rejection the RPC answers with, or
    // nullopt when the edit may proceed.  The gate snapshot stays with the
    // caller because the accepted ticket records its epochs.
    std::optional<nlohmann::ordered_json> checkAuthoringPreconditions(
        const GateSnapshot &gate, const std::string &base_digest,
        std::string_view method,
        std::string_view gate_closed_message) const;
    // BS-1b: render_document_syntax@1 {method, document, line?, column?} when
    // `bytes` (the document `reference`) is not JSON; nullopt otherwise.
    std::optional<nlohmann::ordered_json> syntaxRejection(
        std::string_view bytes, std::string_view reference,
        std::string_view method) const;
    // PB-1: what a finished ticket (or a reload refused at once) leaves in
    // the engine's problem list. A reload's refusal of the documents is
    // listed as source reload_rpc (a busy editor, a closed gate or a file
    // that changed again is not a problem of the documents); a reload that
    // committed, or an edit that committed a new generation, clears the
    // render pipeline's problems. An edit's refusal is the answer's alone.
    void noteProblems(bool reload, bool source_only,
                      const nlohmann::ordered_json &result) const;
    // RR-1: whether a reload's refusal is one of the documents (what
    // noteProblems lists), not of the moment (a busy editor, a closed gate,
    // a file that changed again, a stale digest).
    static bool refusesDocuments(const nlohmann::ordered_json &result);
    // RR-1: the documents a refused reload read become the editor's base
    // (`root` the root as read; `digests` {normalized reference: sha256 hex}
    // of every document it read), and holds_refused_documents_ is set.
    // `unparsed`: the root is bytes that are not JSON; the root held before
    // stays readable (readable_document_).
    void holdRefusedDocuments(AuthoredRenderConfigDocument root,
                              const std::map<std::string, std::string> &digests,
                              bool unparsed = false);
    // RR-1: the root the structural reads answer from -- the held one, or
    // readable_document_ while the held one is not JSON -- with the managed
    // fragments, whether it is live, and the fields that say which it is:
    // {live} and, while the held root is not JSON, held_unparsed: true.
    const AuthoredRenderConfigDocument &readDocument() const;
    RenderConfigCandidateDocumentSet readDocumentSet() const;
    bool readLive() const;
    nlohmann::ordered_json markRead(nlohmann::ordered_json read) const;
    // RR-1: what a finished ticket does to holds_refused_documents_: a
    // reload refused for its documents holds them; a commit that cleared the
    // render pipeline's problems (noteProblems) composed and published what
    // the editor holds.
    void noteHeldDocuments(const Ticket &ticket,
                           const nlohmann::ordered_json &result);
    // RR-1: whether the documents the editor holds are the ones the live
    // generation was composed from: false on the engine's fallback (BS-1)
    // and after a reload refused them, until a ticket publishes them.
    // Reads that answer from the held root carry it as `live`.
    bool documentsLive() const;
    nlohmann::ordered_json reloadRenderDocumentsUnnoted(
        const nlohmann::json &params, bool preview_open);
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
    // FR-3: while edit_batch applies its edits, each to what the ones before
    // it made. document_ and managed_documents_ hold the batch's candidate
    // (restored when the batch returns); `documents` are the other documents
    // its edits wrote (a forked preset, a materialized feature), which
    // currentDocumentSet() carries too. No edit is preflighted on its own:
    // `checks` are the G-1 checks add_authored_pass asked for, and `composes`
    // says an edit's own preflight would have composed its candidate; the
    // batch runs both once on the last candidate. The disk is compared with
    // what the editor holds once, before the first edit.
    struct BatchOverlay {
        std::vector<RenderConfigCandidateDocument> documents;
        std::vector<AuthoredCandidateCheck> checks;
        bool composes = false;
    };
    std::optional<BatchOverlay> batch_;
    nlohmann::ordered_json enqueueAuthoredPassTicket(
        std::string operation, const GateSnapshot &gate,
        const std::string &base_digest, AuthoredRenderConfigDocument root,
        RenderConfigCandidateDocumentSet candidate_documents,
        const std::string &reference,
        bool edits_additional_documents = false,
        const AuthoredCandidateCheck &check = {},
        bool restart_required = false);
    // G-4 / RS-2: the material_route_default a ticket may carry, read and
    // judged by one rule for every ticket that takes it
    // (add_authored_pass, set_material_route_default, edit_graph_document,
    // edit_graph_instances). A value other than "auto" / "forward" throws
    // std::invalid_argument; "forward" on a build without the runtime shader
    // compiler is the returned refusal (material_route_default_unavailable@1).
    struct RequestedRouteDefault {
        std::optional<MaterialRouteDefault> value;
        std::optional<nlohmann::ordered_json> refusal;
    };
    RequestedRouteDefault requestedRouteDefault(
        const nlohmann::json &params, std::string_view method) const;
    // Writes `requested` into the candidate root's rendering section when the
    // committed root says otherwise, and answers whether it did: the engine
    // reads the key once at start, so a change is a restart. Throws
    // RenderConfigSectionUnlocated when the root has no rendering section.
    bool writeRouteDefault(AuthoredRenderConfigDocument &candidate,
                           MaterialRouteDefault requested) const;
    // RS-2: queueRootEdit for a ticket that writes the graph document and may
    // carry material_route_default: the graph edit and the route default are
    // one candidate, judged with the routes the new default gives every
    // material, and a changed default marks the ticket restart_required.
    nlohmann::ordered_json queueGraphRootEdit(
        const nlohmann::json &params, const std::string &base_digest,
        const std::string &operation,
        const std::function<std::optional<nlohmann::ordered_json>(AuthoredRenderConfigDocument &)> &transform,
        bool source_only = false);
    // FG-1: the ticket that writes one group of the root's fold_groups
    // (fold_nodes; FG-2's verify_fold writes `folded.verified` through it).
    // `edit` receives the group's current entry (null when the root has no
    // such group) and returns the new one, or null to remove the group; the
    // root key goes with its last group. The edited root is judged by
    // parseGraphFoldGroups (a named refusal keeps its code and fields), then
    // composed like any root edit (queueRootEdit; RD-1 takes it back).
    nlohmann::ordered_json queueFoldGroupEdit(
        const std::string &method, const std::string &base_digest, const std::string &group,
        const std::function<nlohmann::ordered_json(nlohmann::ordered_json current)> &edit,
        std::optional<std::string> ticket_id = std::nullopt);
    // FG-2: at the frame boundary, every verify_fold whose runs are done is
    // judged (judgeFoldVerifyRuns). A refusal ends its ticket; a pass writes
    // folded.verified through queueFoldGroupEdit under the same ticket, with
    // the CAS of the request's base_source_digest, ahead of the other queued
    // tickets.
    void harvestFoldVerifies() noexcept;
    // EL-3: why edit_rendering_keys does not write `key` (nullopt: it does).
    // `selects_preset`: the current root's section names a pipeline preset.
    std::optional<nlohmann::ordered_json> renderingKeyRefusal(
        const std::string &key, bool selects_preset) const;
    // EL-2: the tail the three planning RPCs share: the gate and CAS, the
    // preset's own declaration of `key` refused (rendering_key_not_allowed_here),
    // then `transform` on the root's current `key` value (nullopt: absent;
    // returning nullopt removes the key), and the candidate's ticket.
    // `transform` may return a refusal instead (its second member).
    struct PlanningKeyValue {
        std::optional<nlohmann::ordered_json> value;
        std::optional<nlohmann::ordered_json> refusal;
    };
    nlohmann::ordered_json queuePlanningKeyEdit(
        std::string_view method, const std::string &base_digest,
        std::string_view key,
        const std::function<PlanningKeyValue(
            std::optional<nlohmann::ordered_json> current,
            const ResolvedRenderConfigAuthoringContext &context)> &transform);
    // EL-2: a physical fragment the runtime's linker refused as stale, as
    // physical_fragment_stale with the family `root_bytes` declares it under
    // (when the root's section holds it), or nullopt for any other error.
    std::optional<nlohmann::ordered_json> physicalFragmentStaleRefusal(
        const std::exception_ptr &error, std::string_view root_bytes,
        nlohmann::ordered_json details) const;
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
    // RR-1: currentDocumentSet with `root` in the root's place.
    RenderConfigCandidateDocumentSet documentSetOf(
        const AuthoredRenderConfigDocument &root) const;
    // RD-1: every render document the current composition can write, root
    // first. Nothing outside the project root, nothing engine://, and nothing
    // that is not a render document of the kind the composition reads it as.
    std::vector<WritableRenderDocument> writableRenderDocuments() const;
    // RL-1: the same for a candidate document set whose root is `root_bytes`
    // (writableRenderDocuments is this for the current set).
    std::vector<WritableRenderDocument> renderDocumentsOf(
        const RenderConfigCandidateDocumentSet &documents,
        std::string_view root_bytes) const;
    // RL-1: records what a committed candidate wrote in
    // known_document_digests_.
    void rememberWrittenDocuments(
        const RenderConfigCandidateDocumentSet &documents);
    // RL-1: the reload ticket's source commit: every document of the
    // candidate is still on disk with the digest it was read with. Writes
    // nothing; throws RenderConfigExternalModification otherwise.
    static void verifyReloadedDocuments(
        const RenderConfigCandidateDocumentSet &documents);
    // The entry of `writable` a request's reference names, or null.
    const WritableRenderDocument *findWritableRenderDocument(
        const std::vector<WritableRenderDocument> &writable,
        const std::string &reference) const;
    // RD-1b: the documents a root names, whether or not they are there: its
    // preset (a project:// one only) and every feature the root or that preset
    // lists. `preset_bytes` gives the preset's bytes (nullopt: none). Only
    // references that are not engine:// or fragment addresses and resolve
    // inside the project are listed; `path` may not exist.
    std::vector<WritableRenderDocument> namedRenderDocuments(
        std::string_view root_bytes,
        const std::function<std::optional<std::string>(const std::string &)>
            &preset_bytes) const;
    // RD-1b: the reference is not engine:// or a fragment address, resolves
    // inside the project, and nothing is at that path.
    bool isAbsentProjectDocument(const std::string &reference) const;
    // RD-1b: the reserved directory `path` lies under ("code/", ".pelican/",
    // ...), or nullopt. Reserved are the places the project template keeps
    // code, scenes, assets, input, UI, vendored packages and generated files,
    // and the directories project.json's basic_config paths name (except one
    // that holds the rendering config root).
    std::optional<std::string> reservedProjectLocation(
        const std::filesystem::path &path) const;
    // RD-1: render_document_not_writable for a reference that is not one of
    // writableRenderDocuments(), with the reason it is not.
    nlohmann::ordered_json renderDocumentNotWritable(
        std::string_view method, const std::string &reference) const;
    void loadManagedDocumentBaselines();
    void completeCommitTransaction(
        const std::shared_ptr<CommitTransaction> &transaction,
        RenderConfigRuntimeApplyResult applied) noexcept;

  public:
    nlohmann::ordered_json getGraphInstances(const nlohmann::json &params) const;
    // FG-1: get_fold_groups {} -> {source_digest, fold_groups: [{group, nodes,
    // kind, digest, verified?, state: "verified" | "stale" | "unverified"}]}
    // (graphfold.hpp), in group name order. Reads only.
    nlohmann::ordered_json getFoldGroups(const nlohmann::json &params) const;
    // FG-1: fold_nodes {base_source_digest, group, nodes, folded}: declares
    // (or replaces) one fold group of the root, `folded` {kind, decisions?}
    // without `verified` (only verify_fold writes it: invalid params), every
    // node a root node id; `folded: null` (nodes absent) removes the group.
    // One ticket (queueFoldGroupEdit).
    nlohmann::ordered_json foldNodes(const nlohmann::json &params);
    // FG-2: verify_fold {base_source_digest, group, frames?: N (3),
    // warmup_seconds?: S (30), tolerance?: T (0)} -> {ticket, status:
    // "accepted", base_source_digest}. The ticket is "running" while a worker
    // draws the open form twice and the decided form once (dependencies'
    // verify_fold); at a frame boundary after that it is committed (the
    // group's folded.verified = {digest, at, frames} written as one RD-1
    // ticket, the result with {group, verified}) or rejected
    // (graph.fold_verify_unstable@1, graph.fold_mismatch@1,
    // fold_verify_failed, or external_modification when the root changed
    // meanwhile). Refused at once: fold_verify_unavailable {reason},
    // fold_verify_busy {group, ticket}, gate_closed, external_modification.
    nlohmann::ordered_json verifyFold(const nlohmann::json &params);
    // AB-1: bake_audio_analysis {asset, out, settings?} -> {ticket, status:
    // "accepted", asset, out}. A worker decodes `asset` (AU-2's decoders),
    // analyzes it and writes `out` (a .ktx2) and `<out>.import.json`
    // (audioanalysis.hpp); the ticket is "running" until a frame boundary
    // after that, then committed {operation: "bake_audio_analysis", asset,
    // out, manifest, layout} or rejected (audio.format_unsupported@1,
    // audio.analysis_failed@1). Refused at once:
    // audio.analysis_settings_invalid@1 {field, reason, value?},
    // audio.analysis_busy@1 {out, ticket} while a bake writes the same out,
    // audio.analysis_unavailable@1 {reason: "built_without_audio"}.
    // Nothing runs on import: only this (or `pelican_cli audio analyze`)
    // bakes.
    nlohmann::ordered_json bakeAudioAnalysis(const nlohmann::json &params);
    // AB-1: list_derived_assets {} -> [{asset, manifest, tool: {name,
    // version}, source, derived_from: {source, tool}, stale, stale_reasons}]:
    // every readable `*.import.json` in the asset stores, in path order
    // (audioanalysisformat.hpp's DerivedAssetStatus). Reads only.
    nlohmann::ordered_json listDerivedAssets(const nlohmann::json &params) const;
    nlohmann::ordered_json editGraphInstances(const nlohmann::json &params);
    nlohmann::ordered_json editGraphDocument(const nlohmann::json &params);
    // LP-3b: a one-piece material pass of the rendering section or of a
    // forked (project) preset, rewritten as a render.material_pass with the
    // same name and place, render.target_ref nodes, a draw_loop graph.loop
    // and its body definition (materialpassdecompose.hpp) -- the section or
    // the preset and the graph in one ticket.
    nlohmann::ordered_json decomposeMaterialPass(const nlohmann::json &params);
    // GT-1: the graph templates the engine ships (engine://graphs/*.json),
    // each with its decomposes and the document itself.
    nlohmann::ordered_json getGraphTemplates(const nlohmann::json &params) const;
    // GT-1: one template taken into the project (graphtemplate.hpp): its
    // definitions and nodes into the root graph, its feature removed or
    // given pass_replacements in the rendering section or the forked
    // preset, its shader_defines added to the rendering section -- one
    // ticket, composed (with the variants) before it is accepted.
    nlohmann::ordered_json applyGraphTemplate(const nlohmann::json &params);
    nlohmann::ordered_json getGraphValues(const nlohmann::json &params) const;
    // AG-3a. Every node in one scope of the published document with what the
    // drawn frame's evaluation gave it -- the definition-scope values
    // get_graph_values cannot answer for, since that one answers per instance
    // input port. `instance` omitted or empty is the root scope.
    nlohmann::ordered_json getGraphNodeValues(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphCatalog(const nlohmann::json &params) const;
    // EL-4b / EL-4c. {status: building} | {status: ready, engine_shaders:
    // [...]} | {status: failed, error}: the engine:// shaders with what
    // reflection reads of each (engineauthoringlists.hpp), reflected on a
    // worker thread once per process, never by get_graph_catalog.
    nlohmann::ordered_json getEngineShaders(const nlohmann::json &params) const;
    // EL-5. {reference, refresh?: bool} -> {status: building} | {status:
    // ready, reference, stages: [<get_engine_shaders entry>...]} | {status:
    // failed, reference, error} (error "shader_not_found" when the stem has
    // no stage file): a project stem's stages reflected on a worker thread,
    // rebuilt when their files' bytes change or on refresh
    // (engineauthoringlists.hpp). Reads the disk only.
    nlohmann::ordered_json getShaderInterface(const nlohmann::json &params) const;
    // EL-6. {} -> {draw_sort, pass_implementation, region_replacements,
    // graph_transforms, render_strategy: [{name, source: engine | game,
    // provider_version}...]}: the provider names each render provider
    // registry resolves now, read at each call (engineauthoringlists.hpp).
    nlohmann::ordered_json getRegisteredProviders(const nlohmann::json &params) const;
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
    // EL-3: {base_source_digest, set?: {key: value}, remove?: [key]} on the
    // root's rendering section, each value whole, in one ticket. The keys are
    // draw_sort, shader_defines, snapshots, graph_transforms, render_strategy,
    // xr, multisampling and resolver_version (the last two not where the
    // section selects a preset: rendering_key_not_allowed_here). A key
    // another RPC writes is rendering_key_owned {key, method}; any other is
    // rendering_key_not_editable {key}. Values are judged by the candidate's
    // composition (with the graph variants) and the runtime's recomposition.
    nlohmann::ordered_json editRenderingKeys(const nlohmann::json &params);
    // EL-2: the planner's decisions the root's rendering section declares,
    // each through one ticket like edit_rendering_keys (the candidate's
    // composition with the graph variants, then the runtime's recomposition,
    // which compiles the target plans and judges the values).
    // {base_source_digest, graph, nodes?: {name: {serial?, isolate?} | null},
    //  resources?: {name: {no_alias?} | null}} into
    // target_planning.graphs.<graph>: null erases the name, an object's fields
    // replace the entry's (a null field erases that field). A name the
    // current composition's frame graph does not have is
    // target_planning_name_unknown {graph, kind, name} (kind "graph", "node"
    // or "resource"); a name only the authored entries still hold may be
    // erased.
    nlohmann::ordered_json editTargetPlanning(const nlohmann::json &params);
    // EL-2: {base_source_digest, family, pin: package | null, graph?}: the
    // package (vulkanTargetPlanPinPackageFromJson) replaces the family's
    // package for its graph; null erases the family's package for `graph`,
    // or every package of the family when `graph` is absent.
    nlohmann::ordered_json editVulkanPlanPins(const nlohmann::json &params);
    // EL-2: the same for vulkan_physical_fragments ({..., fragment: package
    // | null, graph?}). A package the runtime's linker finds stale is refused
    // as physical_fragment_stale {family?, graph, fingerprint, expected,
    // actual}.
    nlohmann::ordered_json putVulkanPhysicalFragment(const nlohmann::json &params);
    nlohmann::ordered_json editComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json addComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json removeComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json editRayCallableRecords(const nlohmann::json &params);
    nlohmann::ordered_json getSceneAccelerationStructures(const nlohmann::json &params) const;
    nlohmann::ordered_json editSceneAccelerationStructures(const nlohmann::json &params);
    // RTL-1a: get_ray_layout {} -> {source_digest, layout, bindings, names,
    // selectable_materials, selectable_procedural_assets,
    // unresolved_bindings}: the ray group layout and the ray bindings the
    // root names (each {reference, digest, document}, document null with
    // error and text when it is not JSON; null when the root names none),
    // the names the parsed layout declares, and what a binding may select.
    nlohmann::ordered_json getRayLayout(const nlohmann::json &params) const;
    // RTL-1a: edit_ray_layout {base_source_digest, layout?,
    // base_layout_digest?, bindings?, base_bindings_digest?,
    // procedural_records?}: each document whole (null takes the root's key
    // out and leaves the file), judged by the layout and bindings parsers at
    // once, written with the root that names it in one ticket. A document
    // that is there needs its base digest (inside edit_batch it may be left
    // out). A new one goes beside the root, named
    // project://<root dir>/ray_layout.json (ray_bindings.json).
    nlohmann::ordered_json editRayLayout(const nlohmann::json &params);
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
    // RD-1: the render documents the current composition can write, byte for
    // byte, with their digests (`references` omitted: all of them). RD-1b: a
    // named document inside the project that is not there is
    // render_document_missing.
    nlohmann::ordered_json getRenderDocuments(
        const nlohmann::json &params) const;
    // RD-1: write documents read by get_render_documents back, whole, in one
    // ticket: every document's current digest must equal its base_digest, and
    // the candidate goes through the same preflight (with variants) and
    // recomposition as any edit before anything is written.
    // RD-1b: `content: null` deletes the document and `base_digest: null`
    // says it is not there now (so it is created); not both. Besides what the
    // current composition writes, the documents the restored root (and its
    // preset) name may be created or deleted; a deletion is limited to what
    // the current or the restored composition names.
    // FR-3: edit_batch {base_source_digest, edits: [{method, params}...]}:
    // the document-writing edits of editBatchMethods(), applied in order to
    // one candidate, each to what the ones before it made, then composed and
    // preflighted once (with the graph variants) and published as one ticket
    // -- one RD-1 `documents` set, one restore. An edit's params carry no
    // base_source_digest (the batch's is the only one); a method outside the
    // list, a malformed entry or an edit's own invalid params are invalid
    // params. An edit that cannot be applied refuses the batch with its own
    // refusal and `batch_index`; a refusal of the composed candidate has no
    // batch_index. Accepted: {ticket, status: "accepted",
    // base_source_digest, methods}; the committed result carries
    // operation "edit_batch" and `methods`.
    nlohmann::ordered_json editBatch(const nlohmann::json &params);
    // FR-3: the methods edit_batch takes, in the order it lists them. Left
    // out: those that write nothing through a ticket of their own, the
    // asynchronous ones, and restore / reload. FR-4: edit_preset_pass,
    // edit_compute_task and remove_compute_task are taken; they read the
    // document they rewrite as the editor holds it (heldDocument).
    static std::vector<std::string> editBatchMethods();
    nlohmann::ordered_json restoreRenderDocuments(
        const nlohmann::json &params);
    // RL-1: reload_render_documents {}: the render documents (the root and
    // every document get_render_documents lists for it) read again from
    // disk, composed (with the graph variants) and published through one
    // ticket, as an edit is, without writing anything. The committed result
    // carries {operation: "reload_render_documents", source_digest,
    // documents: {reference: digest}, changed: [reference]} and no_change
    // when nothing differs. IA-2: it also carries changed_assets: [asset],
    // the assets (published_assets: images' initial assets, IA-3 the
    // value.sample_table tables) whose bytes on disk differ from what the
    // live generation read; one is enough to publish
    // a new generation with the same documents. A refusal leaves the live
    // generation as it was; RR-1: one of the documents (not reload_busy,
    // gate_closed, external_modification or stale_source_digest) makes what
    // it read the editor's base (holdRefusedDocuments), so a restore or an
    // edit on the disk's digest fixes it, and the reads say `live: false`.
    // While the editor holds such documents (or the engine runs its
    // fallback) a reload composes them even when nothing changed. A root
    // that is JSON but not a pelican.graph object is not held. A pending or
    // in-flight ticket, or `preview_open`, is reload_busy.
    nlohmann::ordered_json reloadRenderDocuments(
        const nlohmann::json &params, bool preview_open = false);
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
