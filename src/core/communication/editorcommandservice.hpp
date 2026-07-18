#pragma once

#include "editorjournal.hpp"
#include "../loader/authoringscenedocument.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::uint64_t maxExactEditorJsonInteger = UINT64_C(9007199254740991);
inline constexpr std::size_t maxSceneSnapshotBytes = std::size_t{64} * 1024U * 1024U;

enum class EditorCommandErrorCode : std::uint8_t {
    InvalidParams,
    SceneNotFound,
    ObjectNotFound,
    UnsupportedSnapshotVersion,
    SnapshotBusy,
    SnapshotTooLarge,
};

std::string_view editorCommandErrorCodeName(EditorCommandErrorCode code) noexcept;

class EditorCommandError : public std::runtime_error {
    EditorCommandErrorCode code_;

  public:
    EditorCommandError(EditorCommandErrorCode code, const std::string &message);
    EditorCommandErrorCode code() const noexcept { return code_; }
};

enum class EditorComponentSchemaState : std::uint8_t {
    Available,
    Missing,
};

struct EditorComponentQueryResult {
    std::string name;
    nlohmann::json authored_json;
    std::optional<nlohmann::ordered_json> runtime_json;
    bool editable = false;
    ComponentCodecState codec_state = ComponentCodecState::Missing;
    std::string codec_name;
    EditorComponentSchemaState schema_state = EditorComponentSchemaState::Missing;
    std::vector<StructFieldSchema> schema_fields;
    bool pending = false;
};

struct EditorObjectQueryResult {
    SceneRevision scene_revision{};
    AuthoringObjectId authoring_object_id{};
    std::optional<std::string> name;
    std::optional<std::string> parent;
    std::optional<GameObjectId> entity_id;
    std::vector<EditorComponentQueryResult> components;
};

struct EditorSceneTreeRequest {
    std::optional<std::string> scene_id;
};

struct EditorSceneTreeResult {
    SceneRevision scene_revision{};
    std::string scene_id;
    std::vector<EditorObjectQueryResult> objects;
};

struct EditorGetComponentsRequest {
    std::optional<std::string> scene_id;
    std::optional<AuthoringObjectId> authoring_object_id;
    std::optional<std::string> name;
};

struct EditorAssetQueryResult {
    std::string id;
    std::string kind;
    std::string path;
    std::string store;
    std::string status;
};

struct EditorListAssetsRequest {
    std::optional<std::string> store;
};

struct EditorListAssetsResult {
    std::vector<EditorAssetQueryResult> assets;
};

struct ExportSceneSnapshotRequestV1 {
    std::uint64_t schema_version = 1;
    bool allow_pending = false;
};

struct DigestV1 {
    std::string algorithm;
    std::string hex;
};

struct ExportSceneSnapshotResponseV1 {
    std::uint64_t schema_version = 1;
    SceneRevision scene_revision{};
    std::string current_scene_id;
    std::string semantic_scene_bytes;
    DigestV1 digest;
    std::vector<std::string> pending_ticket_ids;
    std::uint64_t preview_epoch = 0;
};

struct EditorRuntimeObjectState {
    std::optional<GameObjectId> entity_id;
    std::vector<std::optional<nlohmann::ordered_json>> component_runtime_json;
    std::vector<bool> component_pending;
};

struct EditorSnapshotState {
    std::vector<std::string> pending_ticket_ids;
    bool open_preview_lease = false;
    std::uint64_t preview_epoch = 0;
};

struct EditorCommandServiceDependencies {
    std::function<const AuthoringSceneDocument &()> document;
    std::function<std::string()> current_scene_id;
    std::function<EditorRuntimeObjectState(const AuthoringSceneView &, const AuthoringObjectView &)> runtime_query;
    std::function<std::vector<EditorAssetQueryResult>()> assets;
    std::function<EditorSnapshotState()> snapshot_state;
    std::optional<EditorEditRuntimeDependencies> edit;
};

class EditorCommandService {
    EditorCommandServiceDependencies dependencies_;
    std::unique_ptr<EditorEditCoordinator> edit_;

    const AuthoringSceneDocument &document() const;
    const AuthoringSceneView &selectScene(const std::vector<AuthoringSceneView> &scenes,
                                          const std::optional<std::string> &requested_scene) const;
    EditorObjectQueryResult queryObject(const AuthoringSceneDocument &document,
                                        const AuthoringSceneView &scene,
                                        const AuthoringObjectView &object) const;

  public:
    explicit EditorCommandService(EditorCommandServiceDependencies dependencies);
    ~EditorCommandService();

    EditorSceneTreeResult sceneTree(const EditorSceneTreeRequest &request = {}) const;
    EditorObjectQueryResult getComponents(const EditorGetComponentsRequest &request) const;
    EditorListAssetsResult listAssets(const EditorListAssetsRequest &request = {}) const;
    ExportSceneSnapshotResponseV1 exportSceneSnapshot(const ExportSceneSnapshotRequestV1 &request) const;

    nlohmann::ordered_json openEditorSession(const nlohmann::json &params);
    nlohmann::ordered_json resumeEditorSession(const nlohmann::json &params);
    nlohmann::ordered_json canEdit(const nlohmann::json &params);
    nlohmann::ordered_json canPreview(const nlohmann::json &params);
    nlohmann::ordered_json edit(const nlohmann::json &params);
    nlohmann::ordered_json undo(const nlohmann::json &params);
    nlohmann::ordered_json redo(const nlohmann::json &params);
    nlohmann::ordered_json openPreview(const nlohmann::json &params);
    nlohmann::ordered_json updatePreview(const nlohmann::json &params);
    nlohmann::ordered_json commitPreview(const nlohmann::json &params);
    nlohmann::ordered_json abortPreview(const nlohmann::json &params);
    nlohmann::ordered_json getEditResult(const nlohmann::json &params) const;
    nlohmann::ordered_json getPreviewResult(const nlohmann::json &params) const;
    nlohmann::ordered_json queryJournal(const nlohmann::json &params) const;
    std::vector<nlohmann::ordered_json> takeCompletedEditResults();
    void commitPendingEdits() noexcept;
    bool forceAbortPreview(std::string reason) noexcept;
    EditorEditCoordinator *editCoordinator() noexcept { return edit_.get(); }
    const EditorEditCoordinator *editCoordinator() const noexcept { return edit_.get(); }
};

class EditorCommandRpcAdapter {
    const EditorCommandService &service_;
    EditorCommandService *mutable_service_ = nullptr;

  public:
    explicit EditorCommandRpcAdapter(const EditorCommandService &service) : service_{service} {}
    explicit EditorCommandRpcAdapter(EditorCommandService &service)
        : service_{service}, mutable_service_{&service} {}

    nlohmann::ordered_json sceneTree(const nlohmann::json &params) const;
    nlohmann::ordered_json getComponents(const nlohmann::json &params) const;
    nlohmann::ordered_json listAssets(const nlohmann::json &params) const;
    nlohmann::ordered_json exportSceneSnapshot(const nlohmann::json &params) const;
    nlohmann::ordered_json openEditorSession(const nlohmann::json &params) const;
    nlohmann::ordered_json resumeEditorSession(const nlohmann::json &params) const;
    nlohmann::ordered_json canEdit(const nlohmann::json &params) const;
    nlohmann::ordered_json canPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json edit(const nlohmann::json &params) const;
    nlohmann::ordered_json undo(const nlohmann::json &params) const;
    nlohmann::ordered_json redo(const nlohmann::json &params) const;
    nlohmann::ordered_json openPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json updatePreview(const nlohmann::json &params) const;
    nlohmann::ordered_json commitPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json abortPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json getEditResult(const nlohmann::json &params) const;
    nlohmann::ordered_json getPreviewResult(const nlohmann::json &params) const;
    nlohmann::ordered_json queryJournal(const nlohmann::json &params) const;
    std::vector<nlohmann::ordered_json> takeCompletedEditResults() const;
    bool forceAbortPreview(std::string reason) const noexcept;
};

// The ImGui WP consumes the same typed service. This fake is deliberately kept
// free of ImGui headers so equivalence is testable in the CPU-only suite.
class EditorCommandImGuiFakeAdapter {
    const EditorCommandService &service_;

  public:
    explicit EditorCommandImGuiFakeAdapter(const EditorCommandService &service) : service_{service} {}

    EditorSceneTreeResult sceneTree(const EditorSceneTreeRequest &request = {}) const {
        return service_.sceneTree(request);
    }
    EditorObjectQueryResult getComponents(const EditorGetComponentsRequest &request) const {
        return service_.getComponents(request);
    }
    EditorListAssetsResult listAssets(const EditorListAssetsRequest &request = {}) const {
        return service_.listAssets(request);
    }
    ExportSceneSnapshotResponseV1 exportSceneSnapshot(const ExportSceneSnapshotRequestV1 &request) const {
        return service_.exportSceneSnapshot(request);
    }
};

nlohmann::ordered_json editorQueryJson(const EditorComponentQueryResult &component);
nlohmann::ordered_json editorQueryJson(const EditorObjectQueryResult &object);
nlohmann::ordered_json editorQueryJson(const EditorSceneTreeResult &scene);
nlohmann::ordered_json editorQueryJson(const EditorListAssetsResult &assets);
nlohmann::ordered_json editorQueryJson(const ExportSceneSnapshotResponseV1 &snapshot);

} // namespace Pelican
