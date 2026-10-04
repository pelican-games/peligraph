#include "editorrpchandlers.hpp"

#include "editorcommandservice.hpp"
#include "editorpreviewservice.hpp"
#include "rpcserver.hpp"
#include "schemavocabularyadapter.hpp"

#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

template <class Invoke>
auto invokeEditorRpc(Invoke &&invoke) -> decltype(invoke()) {
    try {
        return std::forward<Invoke>(invoke)();
    } catch (const EditorPreviewError &error) {
        const auto invalid = error.code() == EditorPreviewErrorCode::schema_violation ||
                             error.code() == EditorPreviewErrorCode::capture_schema_violation;
        nlohmann::json data = error.payload();
        data["code"] = editorPreviewErrorCodeName(error.code());
        throw JsonRpcHandlerError{
            invalid ? JsonRpcErrorCodes::invalidParams
                    : JsonRpcErrorCodes::applicationError,
            error.what(), std::move(data)};
    } catch (const EditorCommandError &error) {
        const auto rpc_code = error.code() == EditorCommandErrorCode::InvalidParams
                                  ? JsonRpcErrorCodes::invalidParams
                                  : JsonRpcErrorCodes::applicationError;
        nlohmann::json data{{"code", editorCommandErrorCodeName(error.code())}};
        if (error.detail()) data["detail"] = *error.detail();
        throw JsonRpcHandlerError{rpc_code, error.what(), std::move(data)};
    } catch (const internal::SchemaTypeMappingError &error) {
        throw JsonRpcHandlerError{
            JsonRpcErrorCodes::applicationError, error.what(),
            nlohmann::json{{"code", internal::schemaTypeMappingErrorCodeName(
                                       error.code())}}};
    } catch (const std::invalid_argument &error) {
        throw JsonRpcHandlerError{JsonRpcErrorCodes::invalidParams, error.what()};
    }
}

// A resolution failure is reported as a "resolution_error" payload built by
// resolutionFailureJson, whose key order carries meaning for the reader; a
// response that embeds one anywhere must reach the wire with its order intact.
// Only the editor knows that, which is why the predicate lives here and the
// ordered editor handlers are registered with it rather than the transport
// inspecting results for the key.
bool embedsResolutionError(const nlohmann::ordered_json &value) {
    if (value.is_object()) {
        if (value.contains("resolution_error")) return true;
        for (const auto &entry : value) {
            if (embedsResolutionError(entry)) return true;
        }
    } else if (value.is_array()) {
        for (const auto &entry : value) {
            if (embedsResolutionError(entry)) return true;
        }
    }
    return false;
}

} // namespace

void configureEditorRpcHandlers(RpcServer &server,
                                EditorCommandRpcAdapter &editor_rpc,
                                EditorRpcHandlerHooks hooks) {
    if (!hooks.snapshot_imported || !hooks.save_busy) {
        throw std::invalid_argument(
            "configureEditorRpcHandlers requires lifecycle hooks");
    }

    server.setHandler("scene_tree", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.sceneTree(params); });
    });
    server.setHandler("scene_outline", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.sceneOutline(params); });
    });
    server.setHandler("get_scene_revision", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getSceneRevision(params); });
    });
    server.setHandler("get_component_add_candidates", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getComponentAddCandidates(params); });
    });
    server.setOrderedHandler("get_components", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getComponents(params); });
    }, embedsResolutionError);
    server.setOrderedHandler("get_ray_occurrences", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRayOccurrences(params); });
    }, embedsResolutionError);
    server.setOrderedHandler("get_ray_table", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRayTable(params); });
    }, embedsResolutionError);
    server.setOrderedHandler("resolve_ray_occurrence", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.resolveRayOccurrence(params); });
    }, embedsResolutionError);
    server.setHandler("list_assets", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.listAssets(params); });
    });
    server.setHandler("export_scene_snapshot", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.exportSceneSnapshot(params); });
    });

    auto snapshot_imported = std::move(hooks.snapshot_imported);
    auto refuse_during_play_session =
        std::move(hooks.refuse_during_play_session);
    server.setHandler(
        "import_scene_snapshot",
        [&editor_rpc, snapshot_imported = std::move(snapshot_imported),
         refuse_during_play_session](const nlohmann::json &params) {
            if (refuse_during_play_session)
                refuse_during_play_session("import_scene_snapshot");
            return invokeEditorRpc([&] {
                auto result = editor_rpc.importSceneSnapshot(params);
                snapshot_imported();
                return result;
            });
        });

    auto save_busy = std::move(hooks.save_busy);
    server.setHandler("save_scene",
                      [&editor_rpc, save_busy = std::move(save_busy),
                       refuse_during_play_session](
                          const nlohmann::json &params) {
        if (refuse_during_play_session)
            refuse_during_play_session("save_scene");
        return invokeEditorRpc([&] {
            if (save_busy()) {
                throw EditorCommandError{
                    EditorCommandErrorCode::SaveBusy,
                    "scene save is busy while runtime transform updates are pending",
                };
            }
            return editor_rpc.saveScene(params);
        });
    });

    server.setHandler("open_editor_session", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.openEditorSession(params); });
    });
    server.setHandler("resume_editor_session", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.resumeEditorSession(params); });
    });
    server.setHandler("can_edit", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.canEdit(params); });
    });
    server.setHandler("can_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.canPreview(params); });
    });
    server.setHandler("eval_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.evalPreview(params); });
    });
    server.setHandler("render_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.renderPreview(params); });
    });
    server.setHandler("edit", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.edit(params); });
    });
    server.setHandler("set_prefab_parameter",
                      [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc(
            [&] { return editor_rpc.setPrefabParameter(params); });
    });
    server.setHandler("unset_prefab_parameter",
                      [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc(
            [&] { return editor_rpc.unsetPrefabParameter(params); });
    });
    server.setHandler("unpack_prefab",
                      [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc(
            [&] { return editor_rpc.unpackPrefab(params); });
    });
    server.setHandler("undo", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.undo(params); });
    });
    server.setHandler("redo", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.redo(params); });
    });
    // MT-2: a scene object's own materials (editorcommandservice.hpp).
    server.setHandler("set_object_material", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.setObjectMaterial(params); });
    });
    server.setHandler("get_object_materials", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getObjectMaterials(params); });
    });
    server.setHandler("open_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.openPreview(params); });
    });
    server.setHandler("update_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.updatePreview(params); });
    });
    server.setHandler("commit_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.commitPreview(params); });
    });
    server.setHandler("abort_preview", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.abortPreview(params); });
    });
    server.setHandler("get_render_features", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRenderFeatures(params); });
    });
    server.setHandler("list_render_features", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.listRenderFeatures(params); });
    });
    server.setHandler("export_render_feature", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.exportRenderFeature(params); });
    });
    server.setHandler("import_render_feature", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.importRenderFeature(params); });
    });
    server.setHandler("install_package", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.installPackage(params); });
    });
    server.setHandler("uninstall_package", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.uninstallPackage(params); });
    });
    server.setHandler("edit_render_features", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editRenderFeatures(params); });
    });
    server.setHandler("get_render_authoring_context", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRenderAuthoringContext(params); });
    });
    server.setHandler("fork_render_pipeline", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.forkRenderPipeline(params); });
    });
    server.setHandler("edit_preset_pass", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editPresetPass(params); });
    });
    server.setHandler("edit_region_replacement", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editRegionReplacement(params); });
    });
    server.setHandler("get_compute_task_authoring", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getComputeTaskAuthoring(params); });
    });
    server.setHandler("get_frame_node_order", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getFrameNodeOrder(params); });
    });
    server.setHandler("edit_frame_node_order", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editFrameNodeOrder(params); });
    });
    server.setHandler("set_material_route_default", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.setMaterialRouteDefault(params); });
    });
    server.setHandler("edit_rendering_keys", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editRenderingKeys(params); });
    });
    // EL-2: the planner's decisions the rendering root declares.
    server.setHandler("edit_target_planning", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editTargetPlanning(params); });
    });
    server.setHandler("edit_vulkan_plan_pins", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editVulkanPlanPins(params); });
    });
    server.setHandler("put_vulkan_physical_fragment", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.putVulkanPhysicalFragment(params); });
    });
    // FG-1: fold groups of the root graph.
    server.setHandler("get_fold_groups", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getFoldGroups(params); });
    });
    server.setHandler("fold_nodes", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.foldNodes(params); });
    });
    // FG-2: draws the open and the decided form of a fold group and marks it.
    server.setHandler("verify_fold", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.verifyFold(params); });
    });
    // AB-1: bake a sound's analysis into a table (a ticket), and list the
    // assets an .import.json says were derived, stale or not.
    server.setHandler("bake_audio_analysis", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.bakeAudioAnalysis(params); });
    });
    server.setHandler("list_derived_assets", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.listDerivedAssets(params); });
    });
    server.setHandler("get_graph_instances", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getGraphInstances(params); });
    });
    server.setHandler("edit_graph_instances", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editGraphInstances(params); });
    });
    server.setHandler("edit_graph_document", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editGraphDocument(params); });
    });
    server.setHandler("decompose_material_pass", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.decomposeMaterialPass(params); });
    });
    server.setHandler("get_graph_templates", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getGraphTemplates(params); });
    });
    server.setHandler("apply_graph_template", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.applyGraphTemplate(params); });
    });
    server.setHandler("get_graph_values", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getGraphValues(params); });
    });
    server.setHandler("get_graph_node_values", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getGraphNodeValues(params); });
    });
    server.setHandler("get_graph_catalog", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getGraphCatalog(params); });
    });
    server.setHandler("get_engine_shaders", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getEngineShaders(params); });
    });
    server.setHandler("get_shader_interface", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getShaderInterface(params); });
    });
    // EL-6: the provider names Studio's draw_sort / render_strategy /
    // graph_transforms combos offer, the game DLL's included.
    server.setHandler("get_registered_providers", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRegisteredProviders(params); });
    });
    server.setHandler("edit_compute_task", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editComputeTask(params); });
    });
    server.setHandler("get_procedural_assets", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getProceduralAssets(params); });
    });
    server.setHandler("edit_procedural_asset", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editProceduralAsset(params); });
    });
    server.setHandler("add_compute_task", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.addComputeTask(params); });
    });
    server.setHandler("remove_compute_task", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.removeComputeTask(params); });
    });
    server.setHandler("get_ray_callable_records", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRayCallableRecords(params); });
    });
    server.setHandler("edit_ray_callable_records", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editRayCallableRecords(params); });
    });
    server.setHandler("get_scene_acceleration_structures", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getSceneAccelerationStructures(params); });
    });
    server.setHandler("edit_scene_acceleration_structures", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editSceneAccelerationStructures(params); });
    });
    // RTL-1a: the ray group layout and ray bindings documents the root names.
    server.setHandler("get_ray_layout", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRayLayout(params); });
    });
    server.setHandler("edit_ray_layout", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editRayLayout(params); });
    });
    server.setHandler("add_authored_pass", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.addAuthoredPass(params); });
    });
    server.setHandler("remove_authored_pass", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.removeAuthoredPass(params); });
    });
    server.setHandler("edit_render_targets", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editRenderTargets(params); });
    });
    // FR-3: several document edits applied in order to one candidate,
    // composed once, one ticket.
    server.setHandler("edit_batch", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editBatch(params); });
    });
    // RD-1: read the render documents whole, and write them back under CAS
    // (Studio's undo of render edits).
    server.setHandler("get_render_documents", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRenderDocuments(params); });
    });
    server.setHandler("restore_render_documents", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.restoreRenderDocuments(params); });
    });
    // RL-1: take in render documents changed on disk outside the editor
    // (the root and what it reads), through one ticket; nothing is written.
    server.setHandler("reload_render_documents", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.reloadRenderDocuments(params); });
    });
    server.setHandler("get_edit_result", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getEditResult(params); });
    });
    server.setHandler("get_preview_result", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getPreviewResult(params); });
    });
    server.setHandler("query_journal", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.queryJournal(params); });
    });
}

} // namespace Pelican
