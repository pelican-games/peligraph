#include "editorcommandservice.hpp"

#include "../loader/sceneauthoringadapter.hpp"
#include "schemavocabularyadapter.hpp"

#include "renderconfigeditor.hpp"
#include "../loader/basicconfig.hpp"
#include "../userpublic/details/behavior/registerer.hpp"

#include <algorithm>
#include <iterator>
#include <picosha2.h>
#include <type_traits>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void invalidParams(const std::string &message) {
    throw EditorCommandError{EditorCommandErrorCode::InvalidParams, message};
}

const Json &requireObjectParams(const Json &params, std::string_view method) {
    if (!params.is_object()) invalidParams(std::string{method} + " params must be an object");
    return params;
}

void requireOnlyFields(const Json &params, std::initializer_list<std::string_view> fields,
                       std::string_view method) {
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            invalidParams(std::string{method} + " unknown field: " + it.key());
        }
    }
}

std::optional<std::string> optionalString(const Json &params, std::string_view field,
                                          std::string_view method) {
    const auto found = params.find(field);
    if (found == params.end()) return std::nullopt;
    if (!found->is_string() || found->get_ref<const std::string &>().empty()) {
        invalidParams(std::string{method} + " " + std::string{field} +
                      " must be a non-empty string");
    }
    return found->get<std::string>();
}

std::uint64_t exactUnsignedInteger(const Json &value, std::string_view path) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) invalidParams(std::string{path} + " must be non-negative");
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        invalidParams(std::string{path} + " must be an integer token");
    }
    if (result > maxExactEditorJsonInteger) {
        invalidParams(std::string{path} + " exceeds 2^53-1");
    }
    return result;
}

OrderedJson schemaFieldJson(
    const StructFieldSchema &field,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    OrderedJson result{{"name", field.name}, {"type", resolver(field.type)}};
    std::visit(
        [&](const auto &range) {
            using Range = std::decay_t<decltype(range)>;
            if constexpr (!std::same_as<Range, std::monostate>) {
                result["range"] = OrderedJson::array({range.first, range.second});
            }
        },
        field.range);
    if (!field.unit.empty()) result["unit"] = field.unit;
    if (field.type == StructFieldType::Enum) {
        result["enum"] = OrderedJson::array();
        for (std::size_t index = 0; index < field.enum_value_count; ++index) {
            result["enum"].push_back(field.enum_values[index]);
        }
    }
    return result;
}

EditorCommandRpcAdapter::SchemaTypeNameResolver
schemaTypeResolverOrDefault(
    EditorCommandRpcAdapter::SchemaTypeNameResolver resolver) {
    if (!resolver) {
        resolver = [](StructFieldType type) {
            return internal::schemaTypeName(type);
        };
    }
    return resolver;
}

EditorSceneTreeRequest parseSceneTreeRequest(const Json &params) {
    constexpr auto method = "scene_tree";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"scene_id"}, method);
    return {.scene_id = optionalString(params, "scene_id", method)};
}

EditorGetComponentsRequest parseGetComponentsRequest(const Json &params) {
    constexpr auto method = "get_components";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"scene_id", "authoring_object_id", "name"}, method);
    EditorGetComponentsRequest request{.scene_id = optionalString(params, "scene_id", method),
                                       .name = optionalString(params, "name", method)};
    if (const auto found = params.find("authoring_object_id"); found != params.end()) {
        const auto value = exactUnsignedInteger(*found, "get_components authoring_object_id");
        if (value == 0) invalidParams("get_components authoring_object_id must be non-zero");
        request.authoring_object_id = AuthoringObjectId{value};
    }
    if (request.authoring_object_id.has_value() == request.name.has_value()) {
        invalidParams("get_components requires exactly one of authoring_object_id or name");
    }
    return request;
}

EditorListAssetsRequest parseListAssetsRequest(const Json &params) {
    constexpr auto method = "list_assets";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"store"}, method);
    return {.store = optionalString(params, "store", method)};
}

ExportSceneSnapshotRequestV1 parseExportSceneSnapshotRequest(const Json &params) {
    constexpr auto method = "export_scene_snapshot";
    requireObjectParams(params, method);
    requireOnlyFields(params, {"schema_version", "allow_pending"}, method);
    const auto version = params.find("schema_version");
    if (version == params.end()) invalidParams("export_scene_snapshot schema_version is required");
    ExportSceneSnapshotRequestV1 request;
    request.schema_version = exactUnsignedInteger(*version, "export_scene_snapshot schema_version");
    if (const auto allow = params.find("allow_pending"); allow != params.end()) {
        if (!allow->is_boolean()) invalidParams("export_scene_snapshot allow_pending must be a boolean");
        request.allow_pending = allow->get<bool>();
    }
    return request;
}

ImportSceneSnapshotRequestV1 parseImportSceneSnapshotRequest(const Json &params) {
    constexpr auto method = "import_scene_snapshot";
    requireObjectParams(params, method);

    // Preserve the normative validation order even when later fields are
    // malformed. A shaped request with an unsupported version must never
    // inspect or allocate its payload.
    const auto version = params.find("schema_version");
    if (version == params.end()) {
        invalidParams("import_scene_snapshot schema_version is required");
    }
    const auto schema_version =
        exactUnsignedInteger(*version, "import_scene_snapshot schema_version");
    if (schema_version != 1) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " + std::to_string(schema_version)};
    }

    requireOnlyFields(params,
                      {"schema_version", "semantic_scene_bytes", "digest",
                       "current_scene_id"},
                      method);
    const auto bytes = params.find("semantic_scene_bytes");
    if (bytes == params.end() || !bytes->is_string()) {
        invalidParams(
            "import_scene_snapshot semantic_scene_bytes must be a string");
    }
    if (bytes->get_ref<const std::string &>().size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }

    const auto digest = params.find("digest");
    if (digest == params.end() || !digest->is_object()) {
        invalidParams("import_scene_snapshot digest must be an object");
    }
    requireOnlyFields(*digest, {"algorithm", "hex"},
                      "import_scene_snapshot digest");
    const auto algorithm = digest->find("algorithm");
    const auto hex = digest->find("hex");
    if (algorithm == digest->end() || !algorithm->is_string() ||
        hex == digest->end() || !hex->is_string()) {
        invalidParams(
            "import_scene_snapshot digest requires string algorithm and hex fields");
    }

    const auto current_scene =
        optionalString(params, "current_scene_id", method);
    if (!current_scene) {
        invalidParams("import_scene_snapshot current_scene_id is required");
    }
    return ImportSceneSnapshotRequestV1{
        .schema_version = schema_version,
        .semantic_scene_bytes = bytes->get<std::string>(),
        .digest = {.algorithm = algorithm->get<std::string>(),
                   .hex = hex->get<std::string>()},
        .current_scene_id = *current_scene,
    };
}

void validateSaveSceneRequest(const Json &params) {
    constexpr auto method = "save_scene";
    requireObjectParams(params, method);
    requireOnlyFields(params, {}, method);
}

} // namespace

std::string_view editorCommandErrorCodeName(EditorCommandErrorCode code) noexcept {
    switch (code) {
    case EditorCommandErrorCode::InvalidParams: return "invalid_params";
    case EditorCommandErrorCode::SceneNotFound: return "scene_not_found";
    case EditorCommandErrorCode::ObjectNotFound: return "object_not_found";
    case EditorCommandErrorCode::UnsupportedSnapshotVersion: return "unsupported_snapshot_version";
    case EditorCommandErrorCode::SnapshotBusy: return "snapshot_busy";
    case EditorCommandErrorCode::SnapshotTooLarge: return "snapshot_too_large";
    case EditorCommandErrorCode::DigestMismatch: return "digest_mismatch";
    case EditorCommandErrorCode::SnapshotInvalid: return "snapshot_invalid";
    case EditorCommandErrorCode::ExternalModification: return "external_modification";
    case EditorCommandErrorCode::SaveBusy: return "save_busy";
    case EditorCommandErrorCode::RuntimeOnlyData: return "runtime_only_data";
    case EditorCommandErrorCode::ResolvedSceneProviderUnavailable:
        return "resolved_scene_provider_unavailable";
    case EditorCommandErrorCode::SaveUnavailable: return "save_unavailable";
    case EditorCommandErrorCode::SaveFailed: return "save_failed";
    }
    return "unknown_editor_error";
}

EditorCommandError::EditorCommandError(EditorCommandErrorCode code,
                                       const std::string &message,
                                       std::optional<std::string> detail)
    : std::runtime_error{message}, code_{code}, detail_{std::move(detail)} {}

EditorCommandService::EditorCommandService(EditorCommandServiceDependencies dependencies)
    : dependencies_{std::move(dependencies)},
      render_config_editor_{
          std::move(dependencies_.render_config_editor)} {
    if (!dependencies_.document || !dependencies_.current_scene_id) {
        throw std::invalid_argument("EditorCommandService requires document and current_scene_id providers");
    }
    if (dependencies_.edit) {
        if (render_config_editor_) {
            auto previous = std::move(
                dependencies_.edit->frame_boundary_extension);
            const auto render_config_editor =
                render_config_editor_;
            dependencies_.edit->frame_boundary_extension =
                [previous = std::move(previous),
                 render_config_editor] {
                    if (previous) previous();
                    render_config_editor->commitPending();
                };
        }
        edit_ = std::make_unique<EditorEditCoordinator>(
            std::move(*dependencies_.edit));
        dependencies_.edit.reset();
    }
    if (dependencies_.preview) {
        auto preview_dependencies = std::move(*dependencies_.preview);
        const auto runtime_snapshot =
            std::move(preview_dependencies.shared_state_snapshot);
        preview_dependencies.shared_state_snapshot =
            [this, runtime_snapshot]() -> OrderedJson {
                auto result = runtime_snapshot();
                auto journal = OrderedJson::array();
                auto pending_tickets = OrderedJson::array();
                auto preview_epoch = std::uint64_t{};
                auto open_preview_lease = false;
                if (edit_) {
                    for (const auto &record : edit_->journal()) {
                        journal.push_back(editorJournalJson(record));
                    }
                    for (const auto &ticket : edit_->pendingTicketIds()) {
                        pending_tickets.push_back(ticket);
                    }
                    preview_epoch = edit_->previewEpoch();
                    open_preview_lease = edit_->hasOpenPreviewLease();
                }
                result["journal"] = std::move(journal);
                result["editor_coordinator"] = {
                    {"pending_ticket_ids", std::move(pending_tickets)},
                    {"preview_epoch", preview_epoch},
                    {"open_preview_lease", open_preview_lease},
                };
                return result;
            };
        preview_ = std::make_unique<EditorPreviewService>(
            std::move(preview_dependencies));
        dependencies_.preview.reset();
    }
}

EditorCommandService::~EditorCommandService() = default;

const AuthoringSceneDocument &EditorCommandService::document() const {
    return dependencies_.document();
}

const ResolvedScene &EditorCommandService::resolved() const {
    if (!dependencies_.resolved_scene) {
        throw EditorCommandError{
            EditorCommandErrorCode::ResolvedSceneProviderUnavailable,
            "resolved_scene_provider_unavailable: EditorCommandService requires an explicitly wired resolved_scene provider"};
    }
    return dependencies_.resolved_scene();
}

const ResolvedSceneView &
EditorCommandService::selectScene(std::span<const ResolvedSceneView> scenes,
                                  const std::optional<std::string> &requested_scene) const {
    const auto scene_id = requested_scene.value_or(dependencies_.current_scene_id());
    const auto found = std::find_if(scenes.begin(), scenes.end(),
                                    [&](const auto &scene) { return scene.scene_id == scene_id; });
    if (found == scenes.end()) {
        throw EditorCommandError{EditorCommandErrorCode::SceneNotFound,
                                 "scene not found: " + scene_id};
    }
    return *found;
}

EditorObjectQueryResult EditorCommandService::queryObject(
    const ResolvedScene &source, const ResolvedSceneView &scene,
    const ResolvedObject &object) const {
    EditorRuntimeObjectState runtime;
    if (dependencies_.runtime_query) runtime = dependencies_.runtime_query(scene, object);
    if (!runtime.component_runtime_json.empty() &&
        runtime.component_runtime_json.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a component count mismatch");
    }
    if (!runtime.component_pending.empty() && runtime.component_pending.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a pending count mismatch");
    }
    if (!runtime.behavior_attachments.empty() &&
        runtime.behavior_attachments.size() != object.components.size()) {
        throw std::logic_error("editor runtime adapter returned a behavior attachment count mismatch");
    }

    EditorObjectQueryResult result{.scene_revision = source.revision(),
                                   .authoring_object_id = object.authoring_object_id,
                                   .declaration_index = object.authoring_object_index,
                                   .name = object.name,
                                   .parent = object.parent,
                                   .entity_id = runtime.entity_id};
    result.components.reserve(object.components.size());
    for (std::size_t index = 0; index < object.components.size(); ++index) {
        const auto &component = object.components[index];
        const auto &name = component.name;
        EditorComponentQueryResult component_result{
            .name = name,
            .component_index = component.authoring_component_index,
            .authored_json = component.source_json_exact,
            .editable = component.codec.editable,
            .codec_state = component.codec.state,
            .codec_name = std::string{component.codec.codec_name},
            .pending = !runtime.component_pending.empty() && runtime.component_pending[index],
        };
        if (!runtime.component_runtime_json.empty()) {
            component_result.runtime_json = runtime.component_runtime_json[index];
        }
        if (name == "behavior") {
            const auto stable_name = component.effective_json.value(
                "type", std::string{});
            const auto *registration =
                internal::getBehaviorRegisterer().findByName(stable_name);
            if (!runtime.behavior_attachments.empty() &&
                runtime.behavior_attachments[index]) {
                const auto &attachment = *runtime.behavior_attachments[index];
                component_result.behavior_attachment_handle = attachment.handle;
                component_result.behavior_attachment_seq = attachment.attachment_seq;
                component_result.behavior_owner = attachment.owner;
                component_result.behavior_owner_generation =
                    attachment.owner_generation;
                component_result.pending = attachment.pending;
            }
            if (registration != nullptr && !component_result.pending &&
                component.behavior_canonical_params) {
                component_result.editable = true;
                component_result.codec_state = ComponentCodecState::Registered;
                component_result.codec_name = "behavior_params";
                component_result.schema_state =
                    EditorComponentSchemaState::Available;
                component_result.schema_fields = registration->params_schema;
            } else {
                component_result.editable = false;
            }
        } else if (const auto *codec = findComponentCodec(name)) {
            component_result.schema_state = EditorComponentSchemaState::Available;
            const auto fields = codec->fieldSchema();
            component_result.schema_fields.assign(fields.begin(), fields.end());
        }
        result.components.push_back(std::move(component_result));
    }
    return result;
}

EditorSceneTreeResult EditorCommandService::sceneTree(const EditorSceneTreeRequest &request) const {
    const auto &source = resolved();
    const auto &scene = selectScene(source.scenes(), request.scene_id);
    EditorSceneTreeResult result{.scene_revision = source.revision(), .scene_id = scene.scene_id};
    result.objects.reserve(scene.objects.size());
    for (const auto &object : scene.objects) result.objects.push_back(queryObject(source, scene, object));
    return result;
}

EditorSceneRevisionResult EditorCommandService::getSceneRevision() const {
    if (edit_) synchronizePreviewWatch();
    const auto revision = resolved().revision();
    if (revision.value > maxExactEditorJsonInteger) {
        throw std::overflow_error("SceneRevision exceeds 2^53-1");
    }

    auto preview_epoch = std::uint64_t{};
    if (dependencies_.snapshot_state) {
        preview_epoch = dependencies_.snapshot_state().preview_epoch;
    }
    if (edit_) preview_epoch = std::max(preview_epoch, edit_->previewEpoch());
    if (preview_epoch > maxExactEditorJsonInteger) {
        throw std::overflow_error("preview_epoch exceeds 2^53-1");
    }

    EditorSceneRevisionResult result{
        .token = {.scene_revision = revision, .preview_epoch = preview_epoch},
    };
    if (edit_ && !edit_->journal().empty()) {
        const auto &record = edit_->journal().back();
        result.last_transaction = EditorLastTransactionResult{
            .actor_id = record.actor_id,
            .display_name = record.actor_display_name,
            .affected_authoring_ids = record.affected_authoring_ids,
        };
    }
    if (edit_ && edit_->hasOpenPreviewLease() && preview_lease_) {
        result.preview_lease = preview_lease_;
    }
    return result;
}

EditorObjectQueryResult
EditorCommandService::getComponents(const EditorGetComponentsRequest &request) const {
    if (request.authoring_object_id.has_value() == request.name.has_value()) {
        throw EditorCommandError{EditorCommandErrorCode::InvalidParams,
                                 "get_components requires exactly one object selector"};
    }
    const auto &source = resolved();
    const auto &scene = selectScene(source.scenes(), request.scene_id);
    const auto found = std::find_if(scene.objects.begin(), scene.objects.end(), [&](const auto &object) {
        if (request.authoring_object_id) return object.authoring_object_id == *request.authoring_object_id;
        return object.name && *object.name == *request.name;
    });
    if (found == scene.objects.end()) {
        throw EditorCommandError{EditorCommandErrorCode::ObjectNotFound,
                                 "object not found in scene: " + scene.scene_id};
    }
    return queryObject(source, scene, *found);
}

EditorListAssetsResult EditorCommandService::listAssets(const EditorListAssetsRequest &request) const {
    EditorListAssetsResult result;
    if (!dependencies_.assets) return result;
    for (auto asset : dependencies_.assets()) {
        if (!request.store || asset.store == *request.store) result.assets.push_back(std::move(asset));
    }
    return result;
}

ExportSceneSnapshotResponseV1
EditorCommandService::exportSceneSnapshot(const ExportSceneSnapshotRequestV1 &request) const {
    if (request.schema_version != 1) {
        throw EditorCommandError{EditorCommandErrorCode::UnsupportedSnapshotVersion,
                                 "unsupported snapshot version: " +
                                     std::to_string(request.schema_version)};
    }
    auto snapshot_state = dependencies_.snapshot_state ? dependencies_.snapshot_state()
                                                        : EditorSnapshotState{};
    if (edit_) {
        auto pending = edit_->pendingTicketIds();
        snapshot_state.pending_ticket_ids.insert(
            snapshot_state.pending_ticket_ids.end(), pending.begin(), pending.end());
        snapshot_state.open_preview_lease =
            snapshot_state.open_preview_lease || edit_->hasOpenPreviewLease();
        snapshot_state.preview_epoch =
            std::max(snapshot_state.preview_epoch, edit_->previewEpoch());
    }
    if (snapshot_state.preview_epoch > maxExactEditorJsonInteger) {
        throw std::overflow_error("preview_epoch exceeds 2^53-1");
    }
    if (!request.allow_pending &&
        (!snapshot_state.pending_ticket_ids.empty() || snapshot_state.open_preview_lease)) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotBusy,
                                 "snapshot export is busy"};
    }

    const auto &source = document();
    if (source.revision().value > maxExactEditorJsonInteger) {
        throw std::overflow_error("SceneRevision exceeds 2^53-1");
    }
    const auto current_scene = dependencies_.current_scene_id();
    if (resolved().findScene(current_scene) == nullptr) {
        throw EditorCommandError{EditorCommandErrorCode::SceneNotFound,
                                 "scene not found: " + current_scene};
    }
    auto bytes = internal::encodeAuthoringSceneSemantic(source);
    if (bytes.size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }

    auto digest = picosha2::hash256_hex_string(bytes.begin(), bytes.end());
    return ExportSceneSnapshotResponseV1{
        .schema_version = 1,
        .scene_revision = source.revision(),
        .current_scene_id = current_scene,
        .semantic_scene_bytes = std::move(bytes),
        .digest = {.algorithm = "sha256", .hex = std::move(digest)},
        .pending_ticket_ids = snapshot_state.pending_ticket_ids,
        .preview_epoch = snapshot_state.preview_epoch,
    };
}

ImportSceneSnapshotResult EditorCommandService::importSceneSnapshot(
    const ImportSceneSnapshotRequestV1 &request) {
    // SNAPSHOT0 validation order is part of the wire contract. Do not merge
    // these gates or move parsing ahead of the digest check.
    if (request.schema_version != 1) {
        throw EditorCommandError{
            EditorCommandErrorCode::UnsupportedSnapshotVersion,
            "unsupported snapshot version: " +
                std::to_string(request.schema_version)};
    }
    if (request.semantic_scene_bytes.size() > maxSceneSnapshotBytes) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotTooLarge,
                                 "snapshot exceeds 64 MiB"};
    }

    const auto actual_digest = picosha2::hash256_hex_string(
        request.semantic_scene_bytes.begin(),
        request.semantic_scene_bytes.end());
    if (request.digest.algorithm != "sha256" ||
        request.digest.hex != actual_digest) {
        throw EditorCommandError{EditorCommandErrorCode::DigestMismatch,
                                 "snapshot digest does not match semantic_scene_bytes"};
    }

    AuthoringSceneDocument validated;
    try {
        validated = AuthoringSceneDocument::load(
            request.semantic_scene_bytes, SceneRevision{1});
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotInvalid,
                                 "snapshot parse or semantic validation failed",
                                 error.what()};
    }
    if (!internal::authoringSceneContains(validated,
                                          request.current_scene_id)) {
        throw EditorCommandError{
            EditorCommandErrorCode::SceneNotFound,
            "scene not found: " + request.current_scene_id};
    }

    if (!dependencies_.import_scene_snapshot) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotInvalid,
                                 "snapshot import is unavailable",
                                 "runtime import surface is unavailable"};
    }
    try {
        const auto revision = dependencies_.import_scene_snapshot(
            request.semantic_scene_bytes, request.current_scene_id);
        if (revision.value > maxExactEditorJsonInteger) {
            throw std::overflow_error("SceneRevision exceeds 2^53-1");
        }
        return ImportSceneSnapshotResult{
            .scene_revision = revision,
            .current_scene_id = request.current_scene_id,
        };
    } catch (const EditorCommandError &) {
        throw;
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::SnapshotInvalid,
                                 "snapshot reload failed", error.what()};
    }
}

SaveSceneResult EditorCommandService::saveScene() {
    auto state = dependencies_.snapshot_state ? dependencies_.snapshot_state()
                                               : EditorSnapshotState{};
    if (edit_) {
        auto pending = edit_->pendingTicketIds();
        state.pending_ticket_ids.insert(state.pending_ticket_ids.end(),
                                        pending.begin(), pending.end());
        state.open_preview_lease =
            state.open_preview_lease || edit_->hasOpenPreviewLease();
    }
    if (!state.pending_ticket_ids.empty() || state.open_preview_lease) {
        throw EditorCommandError{EditorCommandErrorCode::SaveBusy,
                                 "scene save is busy while an edit ticket or preview is pending"};
    }
    if (!dependencies_.save_scene) {
        throw EditorCommandError{EditorCommandErrorCode::SaveUnavailable,
                                 "scene save is unavailable"};
    }

    try {
        auto result = dependencies_.save_scene();
        if (result.scene_revision.value > maxExactEditorJsonInteger) {
            throw std::overflow_error("SceneRevision exceeds 2^53-1");
        }
        return result;
    } catch (const SceneSaveError &error) {
        switch (error.code()) {
        case SceneSaveErrorCode::ExternalModification:
            throw EditorCommandError{EditorCommandErrorCode::ExternalModification,
                                     error.what()};
        case SceneSaveErrorCode::Unavailable:
            throw EditorCommandError{EditorCommandErrorCode::SaveUnavailable,
                                     error.what()};
        case SceneSaveErrorCode::IoFailure:
            throw EditorCommandError{EditorCommandErrorCode::SaveFailed,
                                     error.what()};
        }
    } catch (const EditorCommandError &) {
        throw;
    } catch (const std::exception &error) {
        throw EditorCommandError{EditorCommandErrorCode::SaveFailed,
                                 error.what()};
    }
    throw EditorCommandError{EditorCommandErrorCode::SaveFailed,
                             "scene save failed"};
}

OrderedJson EditorCommandService::openEditorSession(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->openSession(params);
    actor_display_names_.insert_or_assign(
        result.at("actor_id").get<std::uint64_t>(),
        result.at("display_name").get<std::string>());
    return result;
}

OrderedJson EditorCommandService::resumeEditorSession(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->resumeSession(params);
    actor_display_names_.insert_or_assign(
        result.at("actor_id").get<std::uint64_t>(),
        result.at("display_name").get<std::string>());
    return result;
}

OrderedJson EditorCommandService::canEdit(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->canEdit(params);
}

OrderedJson EditorCommandService::canPreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->canPreview(params);
}

OrderedJson EditorCommandService::evalPreview(const Json &params) {
    if (!preview_ || !edit_) {
        auto payload = OrderedJson::object();
        payload["method"] = "eval_preview";
        payload["reason"] = "preview_service_unavailable";
        throw EditorPreviewError{
            EditorPreviewErrorCode::method_unavailable,
            "eval_preview is unavailable",
            std::move(payload)};
    }
    return preview_->evalPreview(params, [this] {
        const auto gate = edit_->canPreview(Json::object());
        return EditorPreviewGateSnapshot{
            .can_preview = gate.at("can_preview").get<bool>(),
            .epoch = gate.at("gate_epoch").get<std::uint64_t>(),
            .reasons = gate.at("reasons").get<std::vector<std::string>>(),
        };
    });
}

OrderedJson EditorCommandService::renderPreview(const Json &params) {
    if (!preview_ || !edit_) {
        auto payload = OrderedJson::object();
        payload["method"] = "render_preview";
        payload["reason"] = "preview_service_unavailable";
        throw EditorPreviewError{
            EditorPreviewErrorCode::method_unavailable,
            "render_preview is unavailable",
            std::move(payload)};
    }
    return preview_->renderPreview(params, [this] {
        const auto gate = edit_->canPreview(Json::object());
        return EditorPreviewGateSnapshot{
            .can_preview = gate.at("can_preview").get<bool>(),
            .epoch = gate.at("gate_epoch").get<std::uint64_t>(),
            .reasons = gate.at("reasons").get<std::vector<std::string>>(),
        };
    });
}

OrderedJson EditorCommandService::edit(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->enqueue(params);
}

OrderedJson EditorCommandService::undo(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->enqueueUndo(params);
}

OrderedJson EditorCommandService::redo(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->enqueueRedo(params);
}

OrderedJson EditorCommandService::openPreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->openPreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Open);
    return result;
}

OrderedJson EditorCommandService::updatePreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->updatePreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Update);
    return result;
}

OrderedJson EditorCommandService::commitPreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->commitPreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Commit);
    return result;
}

OrderedJson EditorCommandService::abortPreview(const Json &params) {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    auto result = edit_->abortPreview(params);
    trackPreviewTransition(result, params, PreviewWatchTransitionKind::Abort);
    return result;
}

OrderedJson EditorCommandService::getRenderFeatures(
    const Json &params) const {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->getRenderFeatures(params);
}

OrderedJson EditorCommandService::listRenderFeatures(
    const Json &params) const {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->listRenderFeatures(params);
}

OrderedJson EditorCommandService::editRenderFeatures(
    const Json &params) {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->editRenderFeatures(params);
}

OrderedJson EditorCommandService::getRenderAuthoringContext(
    const Json &params) const {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->getRenderAuthoringContext(params);
}

OrderedJson EditorCommandService::addAuthoredPass(
    const Json &params) {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->addAuthoredPass(params);
}

OrderedJson EditorCommandService::removeAuthoredPass(
    const Json &params) {
    if (!render_config_editor_) {
        throw std::logic_error(
            "render config editor service is unavailable");
    }
    return render_config_editor_->removeAuthoredPass(params);
}

OrderedJson EditorCommandService::getEditResult(const Json &params) const {
    if (render_config_editor_ && params.is_object()) {
        const auto found = params.find("ticket");
        if (found != params.end() && found->is_string()) {
            const auto ticket = found->get<std::string>();
            if (ticket.starts_with("render-feature-edit-") ||
                render_config_editor_->ownsTicket(ticket)) {
                return render_config_editor_->getResult(params);
            }
        }
    }
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->getResult(params);
}

OrderedJson EditorCommandService::getPreviewResult(const Json &params) const {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    synchronizePreviewWatch();
    return edit_->getPreviewResult(params);
}

OrderedJson EditorCommandService::queryJournal(const Json &params) const {
    if (!edit_) throw std::logic_error("editor edit service is unavailable");
    return edit_->queryJournal(params);
}

std::vector<OrderedJson> EditorCommandService::takeCompletedEditResults() {
    auto result =
        edit_ ? edit_->takeCompletedResults()
              : std::vector<OrderedJson>{};
    if (render_config_editor_) {
        auto render_results =
            render_config_editor_->takeCompletedResults();
        result.insert(result.end(),
                      std::make_move_iterator(render_results.begin()),
                      std::make_move_iterator(render_results.end()));
    }
    return result;
}

void EditorCommandService::commitPendingEdits() noexcept {
    if (edit_) {
        edit_->commitPending();
        synchronizePreviewWatch();
    } else if (render_config_editor_) {
        render_config_editor_->commitPending();
    }
}

bool EditorCommandService::forceAbortPreview(std::string reason) noexcept {
    if (!edit_ || !edit_->forceAbortPreview(std::move(reason))) return false;
    preview_lease_.reset();
    return true;
}

void EditorCommandService::trackPreviewTransition(
    const OrderedJson &response, const Json &params,
    PreviewWatchTransitionKind kind) {
    synchronizePreviewWatch();
    if (response.value("status", std::string{}) != "accepted") return;
    pending_preview_watch_.push_back(PendingPreviewWatchTransition{
        .request_id = response.at("request_id").get<std::string>(),
        .ticket = response.at("ticket").get<std::string>(),
        .actor_id = EditorActorId{params.at("actor_id").get<std::uint64_t>()},
        .kind = kind,
    });
}

void EditorCommandService::synchronizePreviewWatch() const noexcept {
    try {
        for (auto current = pending_preview_watch_.begin();
             current != pending_preview_watch_.end();) {
            const auto result = edit_->getPreviewResult(
                {{"request_id", current->request_id}});
            const auto status = result.value("status", std::string{});
            if (status == "accepted") {
                ++current;
                continue;
            }
            if (current->kind == PreviewWatchTransitionKind::Open &&
                status == "open") {
                EditorPreviewLeaseResult lease{
                    .actor = {.actor_id = current->actor_id},
                    .ticket = current->ticket,
                    .state = "open",
                };
                if (const auto actor = actor_display_names_.find(
                        current->actor_id.value);
                    actor != actor_display_names_.end()) {
                    lease.actor.display_name = actor->second;
                }
                for (const auto &id : result.at("affected_authoring_ids")) {
                    lease.affected_ids.push_back(
                        AuthoringObjectId{id.get<std::uint64_t>()});
                }
                preview_lease_ = std::move(lease);
            } else if (current->kind == PreviewWatchTransitionKind::Update &&
                       status == "updated" && preview_lease_ &&
                       preview_lease_->ticket == current->ticket) {
                preview_lease_->state = "updated";
            } else if ((current->kind == PreviewWatchTransitionKind::Commit ||
                        current->kind == PreviewWatchTransitionKind::Abort) &&
                       preview_lease_ &&
                       preview_lease_->ticket == current->ticket) {
                preview_lease_.reset();
            }
            current = pending_preview_watch_.erase(current);
        }
        if (!edit_->hasOpenPreviewLease()) preview_lease_.reset();
    } catch (...) {
        if (edit_ && !edit_->hasOpenPreviewLease()) preview_lease_.reset();
    }
}

OrderedJson editorComponentQueryJson(
    const EditorComponentQueryResult &component,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    OrderedJson result{{"name", component.name},
                       {"component_index", component.component_index},
                       {"authored_json", component.authored_json}};
    if (component.runtime_json) result["runtime_json"] = *component.runtime_json;
    result["editable"] = component.editable;
    result["codec"] = OrderedJson{{"state", component.codec_state == ComponentCodecState::Registered
                                                   ? "registered"
                                                   : "missing"}};
    if (!component.codec_name.empty()) result["codec"]["name"] = component.codec_name;
    result["schema"] = OrderedJson{{"state", component.schema_state == EditorComponentSchemaState::Available
                                                    ? "available"
                                                    : "missing"}};
    if (component.schema_state == EditorComponentSchemaState::Available) {
        result["schema"]["fields"] = OrderedJson::array();
        for (const auto &field : component.schema_fields) {
            result["schema"]["fields"].push_back(
                schemaFieldJson(field, resolver));
        }
    }
    result["pending"] = component.pending;
    if (component.name == "behavior") {
        result["behavior"] = OrderedJson{
            {"attachment_index", component.component_index},
            {"attachment_handle", component.behavior_attachment_handle
                                      ? OrderedJson(*component.behavior_attachment_handle)
                                      : OrderedJson(nullptr)},
            {"attachment_seq", component.behavior_attachment_seq
                                   ? OrderedJson(*component.behavior_attachment_seq)
                                   : OrderedJson(nullptr)},
            {"owner", component.behavior_owner
                          ? OrderedJson(*component.behavior_owner)
                          : OrderedJson(nullptr)},
            {"owner_generation", component.behavior_owner_generation
                                     ? OrderedJson(*component.behavior_owner_generation)
                                     : OrderedJson(nullptr)},
            {"status", component.pending ? "pending" : "available"},
        };
    }
    return result;
}

OrderedJson editorQueryJson(const EditorComponentQueryResult &component) {
    const auto resolver = schemaTypeResolverOrDefault({});
    return editorComponentQueryJson(component, resolver);
}

OrderedJson editorQueryJson(const EditorSceneRevisionResult &revision) {
    OrderedJson result{
        {"scene_revision", revision.token.scene_revision.value},
        {"preview_epoch", revision.token.preview_epoch},
    };
    if (revision.last_transaction) {
        result["last_transaction"] = OrderedJson{
            {"actor_id", revision.last_transaction->actor_id.value},
            {"display_name", revision.last_transaction->display_name},
            {"affected_authoring_ids", OrderedJson::array()},
        };
        for (const auto id :
             revision.last_transaction->affected_authoring_ids) {
            result["last_transaction"]["affected_authoring_ids"].push_back(
                id.value);
        }
    } else {
        result["last_transaction"] = nullptr;
    }
    if (revision.preview_lease) {
        result["preview_lease"] = OrderedJson{
            {"actor",
             OrderedJson{{"actor_id", revision.preview_lease->actor.actor_id.value},
                         {"display_name",
                          revision.preview_lease->actor.display_name}}},
            {"ticket", revision.preview_lease->ticket},
            {"affected_ids", OrderedJson::array()},
            {"state", revision.preview_lease->state},
        };
        for (const auto id : revision.preview_lease->affected_ids) {
            result["preview_lease"]["affected_ids"].push_back(id.value);
        }
    } else {
        result["preview_lease"] = nullptr;
    }
    return result;
}

OrderedJson editorObjectQueryJson(
    const EditorObjectQueryResult &object,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    OrderedJson result{{"scene_revision", object.scene_revision.value},
                       {"authoring_object_id", object.authoring_object_id.value},
                       {"declaration_index", object.declaration_index}};
    if (object.name) result["name"] = *object.name;
    if (object.parent) result["parent"] = *object.parent;
    if (object.entity_id) {
        result["entity_id"] = OrderedJson{{"index", object.entity_id->index},
                                           {"gen", object.entity_id->generation}};
    }
    result["components"] = OrderedJson::array();
    for (const auto &component : object.components) {
        result["components"].push_back(
            editorComponentQueryJson(component, resolver));
    }
    return result;
}

OrderedJson editorQueryJson(const EditorObjectQueryResult &object) {
    const auto resolver = schemaTypeResolverOrDefault({});
    return editorObjectQueryJson(object, resolver);
}

OrderedJson editorSceneQueryJson(
    const EditorSceneTreeResult &scene,
    const EditorCommandRpcAdapter::SchemaTypeNameResolver &resolver) {
    OrderedJson result{{"scene_revision", scene.scene_revision.value}, {"scene_id", scene.scene_id}};
    result["objects"] = OrderedJson::array();
    for (const auto &object : scene.objects) {
        result["objects"].push_back(editorObjectQueryJson(object, resolver));
    }
    return result;
}

OrderedJson editorQueryJson(const EditorSceneTreeResult &scene) {
    const auto resolver = schemaTypeResolverOrDefault({});
    return editorSceneQueryJson(scene, resolver);
}

OrderedJson editorQueryJson(const EditorListAssetsResult &assets) {
    OrderedJson result;
    result["assets"] = OrderedJson::array();
    for (const auto &asset : assets.assets) {
        result["assets"].push_back(OrderedJson{{"id", asset.id},
                                               {"kind", asset.kind},
                                               {"path", asset.path},
                                               {"store", asset.store},
                                               {"status", asset.status}});
    }
    return result;
}

OrderedJson editorQueryJson(const ExportSceneSnapshotResponseV1 &snapshot) {
    return OrderedJson{
        {"schema_version", snapshot.schema_version},
        {"scene_revision", snapshot.scene_revision.value},
        {"current_scene_id", snapshot.current_scene_id},
        {"semantic_scene_bytes", snapshot.semantic_scene_bytes},
        {"digest", OrderedJson{{"algorithm", snapshot.digest.algorithm}, {"hex", snapshot.digest.hex}}},
        {"pending_ticket_ids", snapshot.pending_ticket_ids},
        {"preview_epoch", snapshot.preview_epoch},
    };
}

OrderedJson editorQueryJson(const ImportSceneSnapshotResult &snapshot) {
    return OrderedJson{
        {"status", "imported"},
        {"scene_revision", snapshot.scene_revision.value},
        {"current_scene_id", snapshot.current_scene_id},
    };
}

OrderedJson editorQueryJson(const SaveSceneResult &save) {
    return OrderedJson{
        {"status", "saved"},
        {"scene_revision", save.scene_revision.value},
        {"digest", OrderedJson{{"algorithm", save.digest.algorithm},
                                {"hex", save.digest.hex}}},
        {"byte_count", save.byte_count},
        {"scene_hot_reload", save.scene_hot_reload},
    };
}

EditorCommandRpcAdapter::EditorCommandRpcAdapter(
    const EditorCommandService &service, SchemaTypeNameResolver resolver)
    : service_{service},
      schema_type_name_resolver_{
          schemaTypeResolverOrDefault(std::move(resolver))} {}

EditorCommandRpcAdapter::EditorCommandRpcAdapter(
    EditorCommandService &service, SchemaTypeNameResolver resolver)
    : service_{service}, mutable_service_{&service},
      schema_type_name_resolver_{
          schemaTypeResolverOrDefault(std::move(resolver))} {}

OrderedJson EditorCommandRpcAdapter::sceneTree(const Json &params) const {
    return editorSceneQueryJson(service_.sceneTree(parseSceneTreeRequest(params)),
                                schema_type_name_resolver_);
}

OrderedJson EditorCommandRpcAdapter::getSceneRevision(const Json &params) const {
    requireObjectParams(params, "get_scene_revision");
    requireOnlyFields(params, {}, "get_scene_revision");
    return editorQueryJson(service_.getSceneRevision());
}

OrderedJson EditorCommandRpcAdapter::getComponents(const Json &params) const {
    return editorObjectQueryJson(
        service_.getComponents(parseGetComponentsRequest(params)),
        schema_type_name_resolver_);
}

OrderedJson EditorCommandRpcAdapter::listAssets(const Json &params) const {
    return editorQueryJson(service_.listAssets(parseListAssetsRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::exportSceneSnapshot(const Json &params) const {
    return editorQueryJson(service_.exportSceneSnapshot(parseExportSceneSnapshotRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::importSceneSnapshot(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return editorQueryJson(mutable_service_->importSceneSnapshot(
        parseImportSceneSnapshotRequest(params)));
}

OrderedJson EditorCommandRpcAdapter::saveScene(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    validateSaveSceneRequest(params);
    return editorQueryJson(mutable_service_->saveScene());
}

OrderedJson EditorCommandRpcAdapter::openEditorSession(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->openEditorSession(params);
}

OrderedJson EditorCommandRpcAdapter::resumeEditorSession(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->resumeEditorSession(params);
}

OrderedJson EditorCommandRpcAdapter::canEdit(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->canEdit(params);
}

OrderedJson EditorCommandRpcAdapter::canPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->canPreview(params);
}

OrderedJson EditorCommandRpcAdapter::evalPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->evalPreview(params);
}

OrderedJson EditorCommandRpcAdapter::renderPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->renderPreview(params);
}

OrderedJson EditorCommandRpcAdapter::edit(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->edit(params);
}

OrderedJson EditorCommandRpcAdapter::undo(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->undo(params);
}

OrderedJson EditorCommandRpcAdapter::redo(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->redo(params);
}

OrderedJson EditorCommandRpcAdapter::openPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->openPreview(params);
}

OrderedJson EditorCommandRpcAdapter::updatePreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->updatePreview(params);
}

OrderedJson EditorCommandRpcAdapter::commitPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->commitPreview(params);
}

OrderedJson EditorCommandRpcAdapter::abortPreview(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->abortPreview(params);
}

OrderedJson EditorCommandRpcAdapter::getRenderFeatures(
    const Json &params) const {
    return service_.getRenderFeatures(params);
}

OrderedJson EditorCommandRpcAdapter::listRenderFeatures(
    const Json &params) const {
    return service_.listRenderFeatures(params);
}

OrderedJson EditorCommandRpcAdapter::editRenderFeatures(
    const Json &params) const {
    if (!mutable_service_) {
        throw std::logic_error("editor RPC adapter is read-only");
    }
    return mutable_service_->editRenderFeatures(params);
}

OrderedJson EditorCommandRpcAdapter::getRenderAuthoringContext(
    const Json &params) const {
    return service_.getRenderAuthoringContext(params);
}

OrderedJson EditorCommandRpcAdapter::addAuthoredPass(
    const Json &params) const {
    if (mutable_service_ == nullptr) {
        throw std::logic_error(
            "add_authored_pass requires a mutable editor service");
    }
    return mutable_service_->addAuthoredPass(params);
}

OrderedJson EditorCommandRpcAdapter::removeAuthoredPass(
    const Json &params) const {
    if (mutable_service_ == nullptr) {
        throw std::logic_error(
            "remove_authored_pass requires a mutable editor service");
    }
    return mutable_service_->removeAuthoredPass(params);
}

OrderedJson EditorCommandRpcAdapter::getEditResult(const Json &params) const {
    return service_.getEditResult(params);
}

OrderedJson EditorCommandRpcAdapter::getPreviewResult(const Json &params) const {
    return service_.getPreviewResult(params);
}

OrderedJson EditorCommandRpcAdapter::queryJournal(const Json &params) const {
    return service_.queryJournal(params);
}

std::vector<OrderedJson> EditorCommandRpcAdapter::takeCompletedEditResults() const {
    if (!mutable_service_) return {};
    return mutable_service_->takeCompletedEditResults();
}

bool EditorCommandRpcAdapter::forceAbortPreview(std::string reason) const noexcept {
    return mutable_service_ &&
           mutable_service_->forceAbortPreview(std::move(reason));
}

} // namespace Pelican
