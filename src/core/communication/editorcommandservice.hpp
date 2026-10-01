#pragma once

#include "editorjournal.hpp"
#include "editorpreviewservice.hpp"
#include "../loader/resolvedscene.hpp"
#include "../loader/sceneload.hpp"
#include "../model/occurrenceidentity.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <schema.hpp>

namespace Pelican {

class RenderConfigEditorService;

inline constexpr std::uint64_t maxExactEditorJsonInteger = UINT64_C(9007199254740991);
inline constexpr std::size_t maxSceneSnapshotBytes = std::size_t{64} * 1024U * 1024U;

enum class EditorCommandErrorCode : std::uint8_t {
    InvalidParams,
    SceneNotFound,
    ObjectNotFound,
    UnsupportedSnapshotVersion,
    SnapshotBusy,
    SnapshotTooLarge,
    DigestMismatch,
    SnapshotInvalid,
    ExternalModification,
    SaveBusy,
    RuntimeOnlyData,
    ResolvedSceneProviderUnavailable,
    SaveUnavailable,
    SaveFailed,
    PrefabPersistenceUnsupported,
    PrefabGeneratedReadOnly,
    PrefabDependencyMismatch,
};

std::string_view editorCommandErrorCodeName(EditorCommandErrorCode code) noexcept;

class EditorCommandError : public std::runtime_error {
    EditorCommandErrorCode code_;
    std::optional<std::string> detail_;

  public:
    EditorCommandError(EditorCommandErrorCode code, const std::string &message,
                       std::optional<std::string> detail = std::nullopt);
    EditorCommandErrorCode code() const noexcept { return code_; }
    const std::optional<std::string> &detail() const noexcept { return detail_; }
};

enum class EditorComponentSchemaState : std::uint8_t {
    Available,
    Missing,
};

struct EditorComponentQueryResult {
    std::string name;
    std::size_t component_index = 0;
    nlohmann::json authored_json;
    std::optional<nlohmann::ordered_json> runtime_json;
    bool editable = false;
    ComponentCodecState codec_state = ComponentCodecState::Missing;
    std::string codec_name;
    EditorComponentSchemaState schema_state = EditorComponentSchemaState::Missing;
    std::vector<StructFieldSchema> schema_fields;
    std::optional<Schema::Node> schema_node;
    bool pending = false;
    std::optional<std::uint64_t> behavior_attachment_handle;
    std::optional<std::uint64_t> behavior_attachment_seq;
    std::optional<std::uint64_t> behavior_owner;
    std::optional<std::uint32_t> behavior_owner_generation;
    std::optional<GeneratedComponentOrigin> generated;
    std::optional<nlohmann::ordered_json> generated_resolved_json;
    std::optional<StoredResolutionFailure> resolution_failure;
};

struct EditorObjectQueryResult {
    SceneRevision scene_revision{};
    AuthoringObjectId authoring_object_id{};
    std::size_t declaration_index = 0;
    std::optional<std::string> name;
    std::optional<std::string> parent;
    std::optional<GameObjectId> entity_id;
    std::vector<EditorComponentQueryResult> components;
    std::optional<ResolvedPrefabInstance> prefab_instance;
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

struct DigestV1 {
    std::string algorithm;
    std::string hex;
};

struct SaveSceneResult {
    SceneRevision scene_revision{};
    DigestV1 digest;
    std::size_t byte_count = 0;
    bool scene_hot_reload = false;
};

struct ExportSceneSnapshotRequestV2 {
    std::uint64_t schema_version = 2;
    bool allow_pending = false;
};

struct ExportSceneSnapshotResponseV2 {
    std::uint64_t schema_version = 2;
    SceneRevision scene_revision{};
    std::string current_scene_id;
    std::string semantic_scene_bytes;
    DigestV1 digest;
    std::vector<std::string> pending_ticket_ids;
    std::uint64_t preview_epoch = 0;
    PrefabResolutionClosure prefab_closure;
};

struct ImportSceneSnapshotRequestV2 {
    std::uint64_t schema_version = 2;
    std::string semantic_scene_bytes;
    DigestV1 digest;
    std::string current_scene_id;
    PrefabResolutionClosure prefab_closure;
};

struct ImportSceneSnapshotResult {
    SceneRevision scene_revision{};
    std::string current_scene_id;
    std::vector<CommittedWithEffectFailure::EffectError> effect_errors;
    SceneLoadDiagnostics diagnostics;
};

struct EditorRuntimeBehaviorAttachmentState {
    std::uint64_t handle = 0;
    std::uint64_t attachment_seq = 0;
    std::uint64_t owner = 0;
    std::uint32_t owner_generation = 0;
    bool pending = false;
    bool active = false;
};

struct EditorRuntimeObjectState {
    std::optional<GameObjectId> entity_id;
    std::vector<std::optional<nlohmann::ordered_json>> component_runtime_json;
    std::vector<bool> component_pending;
    std::vector<std::optional<EditorRuntimeBehaviorAttachmentState>> behavior_attachments;
};

struct EditorSnapshotState {
    std::vector<std::string> pending_ticket_ids;
    bool open_preview_lease = false;
    std::uint64_t preview_epoch = 0;
};

struct EditorWatchToken {
    SceneRevision scene_revision{};
    std::uint64_t preview_epoch = 0;

    friend bool operator==(const EditorWatchToken &, const EditorWatchToken &) = default;
};

struct EditorWatchActor {
    EditorActorId actor_id{};
    std::string display_name;
};

struct EditorLastTransactionResult {
    EditorActorId actor_id{};
    std::string display_name;
    std::vector<AuthoringObjectId> affected_authoring_ids;
};

struct EditorPreviewLeaseResult {
    EditorWatchActor actor;
    std::string ticket;
    std::vector<AuthoringObjectId> affected_ids;
    std::string state;
};

struct EditorSceneRevisionResult {
    EditorWatchToken token;
    std::optional<EditorLastTransactionResult> last_transaction;
    std::optional<EditorPreviewLeaseResult> preview_lease;
};

struct RaySceneRowSnapshot;
class ProceduralAssetEditorService;

struct EditorRayOccurrencesResult {
    std::vector<RayOccurrenceCatalogEntry> entries;
    std::shared_ptr<const RaySceneRowSnapshot> plan;
    std::uint64_t authoring_object_id = 0;
};

struct EditorCommandServiceDependencies {
    std::function<const AuthoringSceneDocument &()> document;
    std::function<const ResolvedScene &()> resolved_scene;
    std::function<std::string()> current_scene_id;
    std::function<EditorRuntimeObjectState(const ResolvedSceneView &,
                                           const ResolvedObject &)> runtime_query;
    std::function<std::vector<EditorAssetQueryResult>()> assets;
    std::function<EditorSnapshotState()> snapshot_state;
    std::optional<EditorEditRuntimeDependencies> edit;
    std::optional<EditorPreviewServiceDependencies> preview;
    std::shared_ptr<RenderConfigEditorService> render_config_editor;
    std::shared_ptr<ProceduralAssetEditorService> procedural_asset_editor;
    std::function<SceneImportResult(std::string_view, std::string_view,
                                   std::uint64_t)>
        import_scene_snapshot_versioned;
    std::function<SaveSceneResult()> save_scene;
    std::function<EditorRayOccurrencesResult(std::uint64_t)>
        ray_occurrences;
    std::function<RayOccurrenceResolution(std::string_view)>
        resolve_ray_occurrence;
};

class EditorCommandService {
    enum class PreviewWatchTransitionKind : std::uint8_t {
        Open,
        Update,
        Commit,
        Abort,
    };

    struct PendingPreviewWatchTransition {
        std::string request_id;
        std::string ticket;
        EditorActorId actor_id{};
        PreviewWatchTransitionKind kind = PreviewWatchTransitionKind::Open;
    };

    EditorCommandServiceDependencies dependencies_;
    std::shared_ptr<RenderConfigEditorService> render_config_editor_;
    std::shared_ptr<ProceduralAssetEditorService> procedural_asset_editor_;
    std::unique_ptr<EditorEditCoordinator> edit_;
    std::unique_ptr<EditorPreviewService> preview_;
    std::unordered_map<std::uint64_t, std::string> actor_display_names_;
    mutable std::optional<EditorPreviewLeaseResult> preview_lease_;
    mutable std::vector<PendingPreviewWatchTransition> pending_preview_watch_;
    const AuthoringSceneDocument &document() const;
    const ResolvedScene &resolved() const;
    const ResolvedSceneView &selectScene(
        std::span<const ResolvedSceneView> scenes,
        const std::optional<std::string> &requested_scene) const;
    EditorObjectQueryResult queryObject(const ResolvedScene &document,
                                        const ResolvedSceneView &scene,
                                        const ResolvedObject &object) const;
    void trackPreviewTransition(const nlohmann::ordered_json &response,
                                const nlohmann::json &params,
                                PreviewWatchTransitionKind kind);
    void synchronizePreviewWatch() const noexcept;
    void validateGeneratedSelectors(const nlohmann::json &params,
                                    std::string_view method) const;

  public:
    explicit EditorCommandService(EditorCommandServiceDependencies dependencies);
    ~EditorCommandService();

    EditorSceneTreeResult sceneTree(const EditorSceneTreeRequest &request = {}) const;
    EditorSceneRevisionResult getSceneRevision() const;
    EditorObjectQueryResult getComponents(const EditorGetComponentsRequest &request) const;
    nlohmann::ordered_json getComponentAddCandidates(const EditorGetComponentsRequest &request) const;
    EditorRayOccurrencesResult getRayOccurrences(
        const EditorGetComponentsRequest &request) const;
    RayOccurrenceResolution resolveRayOccurrence(
        std::string_view occurrence_id) const;
    EditorListAssetsResult listAssets(const EditorListAssetsRequest &request = {}) const;
    ExportSceneSnapshotResponseV2 exportSceneSnapshotV2(
        const ExportSceneSnapshotRequestV2 &request) const;
    ImportSceneSnapshotResult importSceneSnapshotV2(
        const ImportSceneSnapshotRequestV2 &request);
    SaveSceneResult saveScene();

    nlohmann::ordered_json openEditorSession(const nlohmann::json &params);
    nlohmann::ordered_json resumeEditorSession(const nlohmann::json &params);
    nlohmann::ordered_json canEdit(const nlohmann::json &params);
    nlohmann::ordered_json canPreview(const nlohmann::json &params);
    nlohmann::ordered_json evalPreview(const nlohmann::json &params);
    nlohmann::ordered_json renderPreview(const nlohmann::json &params);
    nlohmann::ordered_json edit(const nlohmann::json &params);
    nlohmann::ordered_json undo(const nlohmann::json &params);
    nlohmann::ordered_json redo(const nlohmann::json &params);
    nlohmann::ordered_json openPreview(const nlohmann::json &params);
    nlohmann::ordered_json updatePreview(const nlohmann::json &params);
    nlohmann::ordered_json commitPreview(const nlohmann::json &params);
    nlohmann::ordered_json abortPreview(const nlohmann::json &params);
    nlohmann::ordered_json getRenderFeatures(const nlohmann::json &params) const;
    nlohmann::ordered_json listRenderFeatures(const nlohmann::json &params) const;
    nlohmann::ordered_json editRenderFeatures(const nlohmann::json &params);
    nlohmann::ordered_json exportRenderFeature(const nlohmann::json &params);
    nlohmann::ordered_json importRenderFeature(const nlohmann::json &params);
    nlohmann::ordered_json installPackage(const nlohmann::json &params);
    nlohmann::ordered_json uninstallPackage(const nlohmann::json &params);
    nlohmann::ordered_json getRayCallableRecords(const nlohmann::json &params) const;
    nlohmann::ordered_json getSceneAccelerationStructures(const nlohmann::json &params) const;
    nlohmann::ordered_json forkRenderPipeline(const nlohmann::json &params);
    nlohmann::ordered_json editPresetPass(const nlohmann::json &params);
    nlohmann::ordered_json editRegionReplacement(const nlohmann::json &params);
    nlohmann::ordered_json getComputeTaskAuthoring(const nlohmann::json &params) const;
    nlohmann::ordered_json getFrameNodeOrder(const nlohmann::json &params) const;
    nlohmann::ordered_json editFrameNodeOrder(const nlohmann::json &params);
    nlohmann::ordered_json setMaterialRouteDefault(const nlohmann::json &params);
    nlohmann::ordered_json editRenderingKeys(const nlohmann::json &params);
    nlohmann::ordered_json editTargetPlanning(const nlohmann::json &params);
    nlohmann::ordered_json editVulkanPlanPins(const nlohmann::json &params);
    nlohmann::ordered_json putVulkanPhysicalFragment(const nlohmann::json &params);
    nlohmann::ordered_json getFoldGroups(const nlohmann::json &params) const;
    nlohmann::ordered_json foldNodes(const nlohmann::json &params);
    nlohmann::ordered_json getGraphInstances(const nlohmann::json &params) const;
    nlohmann::ordered_json editGraphInstances(const nlohmann::json &params);
    nlohmann::ordered_json editGraphDocument(const nlohmann::json &params);
    nlohmann::ordered_json decomposeMaterialPass(const nlohmann::json &params);
    nlohmann::ordered_json getGraphTemplates(const nlohmann::json &params) const;
    nlohmann::ordered_json applyGraphTemplate(const nlohmann::json &params);
    nlohmann::ordered_json getGraphValues(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphNodeValues(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphCatalog(const nlohmann::json &params) const;
    nlohmann::ordered_json getEngineShaders(const nlohmann::json &params) const;
    nlohmann::ordered_json editComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json addComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json removeComputeTask(const nlohmann::json &params);
    nlohmann::ordered_json editRayCallableRecords(const nlohmann::json &params);
    nlohmann::ordered_json editSceneAccelerationStructures(const nlohmann::json &params);
    nlohmann::ordered_json getRenderAuthoringContext(
        const nlohmann::json &params) const;
    nlohmann::ordered_json addAuthoredPass(const nlohmann::json &params);
    nlohmann::ordered_json removeAuthoredPass(const nlohmann::json &params);
    nlohmann::ordered_json editRenderTargets(const nlohmann::json &params);
    nlohmann::ordered_json getRenderDocuments(const nlohmann::json &params) const;
    nlohmann::ordered_json restoreRenderDocuments(const nlohmann::json &params);
    nlohmann::ordered_json getEditResult(const nlohmann::json &params) const;
    nlohmann::ordered_json getPreviewResult(const nlohmann::json &params) const;
    nlohmann::ordered_json queryJournal(const nlohmann::json &params) const;
    std::vector<nlohmann::ordered_json> takeCompletedEditResults();
    void commitPendingEdits() noexcept;
    nlohmann::ordered_json getProceduralAssets(const nlohmann::json &params) const;
    nlohmann::ordered_json editProceduralAsset(const nlohmann::json &params);
    bool forceAbortPreview(std::string reason);
    EditorEditCoordinator *editCoordinator() noexcept { return edit_.get(); }
    const EditorEditCoordinator *editCoordinator() const noexcept { return edit_.get(); }
    bool usesPrefabs() const { return document().usesPrefabs(); }
};

class EditorCommandRpcAdapter {
    const EditorCommandService &service_;
    EditorCommandService *mutable_service_ = nullptr;
    std::function<std::string(StructFieldType)> schema_type_name_resolver_;

  public:
    using SchemaTypeNameResolver =
        std::function<std::string(StructFieldType)>;

    explicit EditorCommandRpcAdapter(
        const EditorCommandService &service,
        SchemaTypeNameResolver resolver = {});
    explicit EditorCommandRpcAdapter(
        EditorCommandService &service,
        SchemaTypeNameResolver resolver = {});

    nlohmann::ordered_json sceneTree(const nlohmann::json &params) const;
    nlohmann::ordered_json getSceneRevision(const nlohmann::json &params) const;
    nlohmann::ordered_json getComponents(const nlohmann::json &params) const;
    nlohmann::ordered_json getComponentAddCandidates(const nlohmann::json &params) const;
    nlohmann::ordered_json getRayOccurrences(const nlohmann::json &params) const;
    nlohmann::ordered_json resolveRayOccurrence(const nlohmann::json &params) const;
    nlohmann::ordered_json listAssets(const nlohmann::json &params) const;
    nlohmann::ordered_json exportSceneSnapshot(const nlohmann::json &params) const;
    nlohmann::ordered_json importSceneSnapshot(const nlohmann::json &params) const;
    nlohmann::ordered_json saveScene(const nlohmann::json &params) const;
    nlohmann::ordered_json openEditorSession(const nlohmann::json &params) const;
    nlohmann::ordered_json resumeEditorSession(const nlohmann::json &params) const;
    nlohmann::ordered_json canEdit(const nlohmann::json &params) const;
    nlohmann::ordered_json canPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json evalPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json renderPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json edit(const nlohmann::json &params) const;
    nlohmann::ordered_json setPrefabParameter(
        const nlohmann::json &params) const;
    nlohmann::ordered_json unsetPrefabParameter(
        const nlohmann::json &params) const;
    nlohmann::ordered_json unpackPrefab(
        const nlohmann::json &params) const;
    nlohmann::ordered_json undo(const nlohmann::json &params) const;
    nlohmann::ordered_json redo(const nlohmann::json &params) const;
    nlohmann::ordered_json openPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json updatePreview(const nlohmann::json &params) const;
    nlohmann::ordered_json commitPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json abortPreview(const nlohmann::json &params) const;
    nlohmann::ordered_json getRenderFeatures(const nlohmann::json &params) const;
    nlohmann::ordered_json listRenderFeatures(const nlohmann::json &params) const;
    nlohmann::ordered_json editRenderFeatures(const nlohmann::json &params) const;
    nlohmann::ordered_json exportRenderFeature(const nlohmann::json &params) const;
    nlohmann::ordered_json importRenderFeature(const nlohmann::json &params) const;
    nlohmann::ordered_json installPackage(const nlohmann::json &params) const;
    nlohmann::ordered_json uninstallPackage(const nlohmann::json &params) const;
    nlohmann::ordered_json getRayCallableRecords(const nlohmann::json &params) const;
    nlohmann::ordered_json getSceneAccelerationStructures(const nlohmann::json &params) const;
    nlohmann::ordered_json forkRenderPipeline(const nlohmann::json &params) const;
    nlohmann::ordered_json editPresetPass(const nlohmann::json &params) const;
    nlohmann::ordered_json editRegionReplacement(const nlohmann::json &params) const;
    nlohmann::ordered_json getComputeTaskAuthoring(const nlohmann::json &params) const;
    nlohmann::ordered_json editComputeTask(const nlohmann::json &params) const;
    nlohmann::ordered_json getProceduralAssets(const nlohmann::json &params) const;
    nlohmann::ordered_json editProceduralAsset(const nlohmann::json &params) const;
    nlohmann::ordered_json getFrameNodeOrder(const nlohmann::json &params) const;
    nlohmann::ordered_json editFrameNodeOrder(const nlohmann::json &params) const;
    nlohmann::ordered_json setMaterialRouteDefault(const nlohmann::json &params) const;
    nlohmann::ordered_json editRenderingKeys(const nlohmann::json &params) const;
    nlohmann::ordered_json editTargetPlanning(const nlohmann::json &params) const;
    nlohmann::ordered_json editVulkanPlanPins(const nlohmann::json &params) const;
    nlohmann::ordered_json putVulkanPhysicalFragment(const nlohmann::json &params) const;
    nlohmann::ordered_json getFoldGroups(const nlohmann::json &params) const;
    nlohmann::ordered_json foldNodes(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphInstances(const nlohmann::json &params) const;
    nlohmann::ordered_json editGraphInstances(const nlohmann::json &params) const;
    nlohmann::ordered_json editGraphDocument(const nlohmann::json &params) const;
    nlohmann::ordered_json decomposeMaterialPass(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphTemplates(const nlohmann::json &params) const;
    nlohmann::ordered_json applyGraphTemplate(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphValues(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphNodeValues(const nlohmann::json &params) const;
    nlohmann::ordered_json getGraphCatalog(const nlohmann::json &params) const;
    nlohmann::ordered_json getEngineShaders(const nlohmann::json &params) const;
    nlohmann::ordered_json addComputeTask(const nlohmann::json &params) const;
    nlohmann::ordered_json removeComputeTask(const nlohmann::json &params) const;
    nlohmann::ordered_json editRayCallableRecords(const nlohmann::json &params) const;
    nlohmann::ordered_json editSceneAccelerationStructures(const nlohmann::json &params) const;
    nlohmann::ordered_json getRenderAuthoringContext(
        const nlohmann::json &params) const;
    nlohmann::ordered_json addAuthoredPass(
        const nlohmann::json &params) const;
    nlohmann::ordered_json removeAuthoredPass(
        const nlohmann::json &params) const;
    nlohmann::ordered_json editRenderTargets(
        const nlohmann::json &params) const;
    nlohmann::ordered_json getRenderDocuments(
        const nlohmann::json &params) const;
    nlohmann::ordered_json restoreRenderDocuments(
        const nlohmann::json &params) const;
    nlohmann::ordered_json getEditResult(const nlohmann::json &params) const;
    nlohmann::ordered_json getPreviewResult(const nlohmann::json &params) const;
    nlohmann::ordered_json queryJournal(const nlohmann::json &params) const;
    std::vector<nlohmann::ordered_json> takeCompletedEditResults() const;
    bool forceAbortPreview(std::string reason) const;
};

// The ImGui WP consumes the same typed service. This fake is deliberately kept
// free of ImGui headers so equivalence is testable in the CPU-only suite.
class EditorCommandImGuiFakeAdapter {
    const EditorCommandService &service_;
    EditorCommandService *mutable_service_ = nullptr;

  public:
    explicit EditorCommandImGuiFakeAdapter(const EditorCommandService &service) : service_{service} {}
    explicit EditorCommandImGuiFakeAdapter(EditorCommandService &service)
        : service_{service}, mutable_service_{&service} {}

    EditorSceneTreeResult sceneTree(const EditorSceneTreeRequest &request = {}) const {
        return service_.sceneTree(request);
    }
    EditorSceneRevisionResult getSceneRevision() const {
        return service_.getSceneRevision();
    }
    EditorObjectQueryResult getComponents(const EditorGetComponentsRequest &request) const {
        return service_.getComponents(request);
    }
    EditorListAssetsResult listAssets(const EditorListAssetsRequest &request = {}) const {
        return service_.listAssets(request);
    }
    ExportSceneSnapshotResponseV2 exportSceneSnapshotV2(
        const ExportSceneSnapshotRequestV2 &request) const {
        return service_.exportSceneSnapshotV2(request);
    }
    SaveSceneResult saveScene() const {
        if (!mutable_service_) {
            throw std::logic_error("editor ImGui fake adapter is read-only");
        }
        return mutable_service_->saveScene();
    }
};

nlohmann::ordered_json editorQueryJson(const EditorComponentQueryResult &component);
nlohmann::ordered_json editorQueryJson(const EditorSceneRevisionResult &revision);
nlohmann::ordered_json editorQueryJson(const EditorObjectQueryResult &object);
nlohmann::ordered_json editorQueryJson(const EditorSceneTreeResult &scene);
nlohmann::ordered_json editorQueryJson(const EditorListAssetsResult &assets);
nlohmann::ordered_json editorQueryJson(const ExportSceneSnapshotResponseV2 &snapshot);
nlohmann::ordered_json editorQueryJson(const ImportSceneSnapshotResult &snapshot);
nlohmann::ordered_json editorQueryJson(const SaveSceneResult &save);

} // namespace Pelican
