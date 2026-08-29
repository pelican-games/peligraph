#pragma once

#include "../loader/editorprojectiontransaction.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

PELICAN_DEFINE_HANDLE(EditorActorId, std::uint64_t)

enum class EditorGateReason : std::uint32_t {
    replay = 1U << 0U,
    golden = 1U << 1U,
    strict = 1U << 2U,
    reload_scene_transition = 1U << 3U,
    preview_lease_conflict = 1U << 4U,
};

constexpr std::uint32_t editorGateReasonBit(EditorGateReason reason) noexcept {
    return static_cast<std::uint32_t>(reason);
}

struct EditorGateObservation {
    std::uint32_t reasons = 0;
    // Incremented by the composition root when a reload or scene transition
    // passes between acceptance and execution, even if the gate is open again.
    std::uint64_t transition_epoch = 0;
};

struct EditorGateSnapshot {
    bool can_edit = true;
    bool can_preview = true;
    std::uint64_t epoch = 1;
    std::vector<std::string> reasons;
};

enum class EditorEditErrorCode : std::uint8_t {
    stale_revision,
    gate_closed,
    preview_lease_conflict,
    preview_lease_busy,
    not_lease_owner,
    ticket_not_found,
    undo_conflict,
    not_editable,
    schema_violation,
    prefab_instance_id_collision,
    unknown_component_type,
    duplicate_component,
    missing_component,
    name_conflict,
    parent_not_found,
    closure_unresolvable,
    cycle_detected,
    zero_scale,
    non_finite_transform,
    trs_unrepresentable,
    preserve_missing,
    method_unavailable,
};

std::string_view editorEditErrorCodeName(EditorEditErrorCode code) noexcept;

struct EditorLastWriterStamp {
    SceneRevision revision{};
    std::string transaction_id;
    EditorActorId actor_id{};
};

struct EditorJournalCommandRecord {
    nlohmann::ordered_json forward;
    nlohmann::ordered_json inverse;
    std::string stable_target;
    std::vector<std::string> read_set;
    std::vector<std::string> write_set;
    nlohmann::ordered_json structural_domain;
    nlohmann::ordered_json forward_postcondition;
    EditorLastWriterStamp last_writer;
};

struct EditorJournalRecord {
    std::string transaction_id;
    EditorActorId actor_id{};
    std::string actor_display_name;
    SceneRevision base_revision{};
    SceneRevision committed_revision{};
    std::vector<nlohmann::ordered_json> ordered_forward;
    std::vector<nlohmann::ordered_json> ordered_inverse;
    std::vector<AuthoringObjectId> affected_authoring_ids;
    std::optional<std::string> coalesce_key;
    std::string status = "committed";
    std::string operation_kind = "edit";
    std::optional<std::string> source_transaction_id;
    std::vector<std::string> stable_targets;
    std::vector<std::string> read_set;
    std::vector<std::string> write_set;
    std::vector<nlohmann::ordered_json> structural_domain;
    std::vector<nlohmann::ordered_json> forward_postconditions;
    std::vector<EditorJournalCommandRecord> commands;
};

nlohmann::ordered_json editorJournalJson(const EditorJournalRecord &record);

struct EditorEditExecutionRequest {
    SceneRevision base_revision{};
    std::span<const EditorProjectionCommand> commands;
    std::span<const nlohmann::ordered_json> operations;
};

// Live preview uses the same canonical commands and adapter preparation path
// as a committed edit, but the composition root supplies an ephemeral
// document target. restore_committed projects the current committed document
// with no authored commands and is used by abort/forced-abort.
struct EditorPreviewExecutionRequest {
    SceneRevision base_revision{};
    std::span<const EditorProjectionCommand> commands;
    std::span<const nlohmann::ordered_json> operations;
    bool restore_committed = false;
};

struct EditorBehaviorAttachmentIdentity {
    std::uint64_t handle = 0;
    std::uint64_t attachment_seq = 0;
};

struct EditorEditRuntimeDependencies {
    std::function<const AuthoringSceneDocument &()> document;
    std::function<std::string()> current_scene_id;
    std::function<EditorProjectionResult(const EditorEditExecutionRequest &)> execute;
    std::function<EditorProjectionResult(const EditorPreviewExecutionRequest &)>
        execute_preview;
    std::function<void()> preview_boundary;
    std::function<EditorGateObservation()> gate;
    std::function<EditorBehaviorAttachmentIdentity(
        std::uint64_t commit_seq, std::size_t command_index,
        std::size_t attachment_index)>
        allocate_behavior_attachment;
    std::function<std::optional<EditorBehaviorAttachmentIdentity>(
        AuthoringObjectId object_id, std::size_t attachment_index)>
        resolve_behavior_attachment;
    // One production frame-boundary owner drains both the scene journal and
    // the WP331 render-config ticket queue.  This avoids a second competing
    // global hook while keeping the two documents independent.
    std::function<void()> frame_boundary_extension;
    bool install_commit_hook = false;
    std::function<PrefabRegistrySnapshot()> prefab_registry;
    std::function<BindableProviderSnapshot()> bindable_provider;
};

// Owns the session-issued actor identity, the accepted ticket queue and
// JOURNAL0. JSON parsing lives here so RPC and embedded GUI adapters consume
// one validation path. executeJournalForVerification is intentionally not an
// RPC surface; it exists only for the required forward/inverse/forward gate.
class EditorEditCoordinator {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    nlohmann::ordered_json enqueueRevert(const nlohmann::json &params,
                                         bool redo);

  public:
    explicit EditorEditCoordinator(EditorEditRuntimeDependencies dependencies);
    ~EditorEditCoordinator();

    EditorEditCoordinator(const EditorEditCoordinator &) = delete;
    EditorEditCoordinator &operator=(const EditorEditCoordinator &) = delete;

    nlohmann::ordered_json openSession(const nlohmann::json &params);
    nlohmann::ordered_json resumeSession(const nlohmann::json &params);
    nlohmann::ordered_json canEdit(const nlohmann::json &params);
    nlohmann::ordered_json canPreview(const nlohmann::json &params);
    nlohmann::ordered_json enqueue(const nlohmann::json &params);
    nlohmann::ordered_json enqueueUndo(const nlohmann::json &params);
    nlohmann::ordered_json enqueueRedo(const nlohmann::json &params);
    nlohmann::ordered_json openPreview(const nlohmann::json &params);
    nlohmann::ordered_json updatePreview(const nlohmann::json &params);
    nlohmann::ordered_json commitPreview(const nlohmann::json &params);
    nlohmann::ordered_json abortPreview(const nlohmann::json &params);
    nlohmann::ordered_json getResult(const nlohmann::json &params) const;
    nlohmann::ordered_json getPreviewResult(const nlohmann::json &params) const;
    nlohmann::ordered_json queryJournal(const nlohmann::json &params) const;

    void commitPending() noexcept;
    std::vector<nlohmann::ordered_json> takeCompletedResults();
    std::vector<std::string> pendingTicketIds() const;
    bool hasOpenPreviewLease() const noexcept;
    std::uint64_t previewEpoch() const noexcept;
    // Used by replay/reload/scene-transition owners before observers resume.
    bool forceAbortPreview(std::string reason) noexcept;
    const std::vector<EditorJournalRecord> &journal() const noexcept;

    EditorProjectionResult executeJournalForVerification(
        std::string_view transaction_id, bool inverse);
};

} // namespace Pelican
