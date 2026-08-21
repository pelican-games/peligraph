#include "editorrpchandlers.hpp"

#include "editorcommandservice.hpp"
#include "editorpreviewservice.hpp"
#include "rpcserver.hpp"

#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

template <class Invoke> nlohmann::json invokeEditorRpc(Invoke &&invoke) {
    try {
        return nlohmann::json(std::forward<Invoke>(invoke)());
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
    } catch (const std::invalid_argument &error) {
        throw JsonRpcHandlerError{JsonRpcErrorCodes::invalidParams, error.what()};
    }
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
    server.setHandler("get_scene_revision", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getSceneRevision(params); });
    });
    server.setHandler("get_components", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getComponents(params); });
    });
    server.setHandler("list_assets", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.listAssets(params); });
    });
    server.setHandler("export_scene_snapshot", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.exportSceneSnapshot(params); });
    });

    auto snapshot_imported = std::move(hooks.snapshot_imported);
    server.setHandler(
        "import_scene_snapshot",
        [&editor_rpc, snapshot_imported = std::move(snapshot_imported)](
            const nlohmann::json &params) {
            return invokeEditorRpc([&] {
                auto result = editor_rpc.importSceneSnapshot(params);
                snapshot_imported();
                return result;
            });
        });

    auto save_busy = std::move(hooks.save_busy);
    server.setHandler("save_scene",
                      [&editor_rpc, save_busy = std::move(save_busy)](
                          const nlohmann::json &params) {
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
    server.setHandler("undo", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.undo(params); });
    });
    server.setHandler("redo", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.redo(params); });
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
    server.setHandler("edit_render_features", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.editRenderFeatures(params); });
    });
    server.setHandler("get_render_authoring_context", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.getRenderAuthoringContext(params); });
    });
    server.setHandler("add_authored_pass", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.addAuthoredPass(params); });
    });
    server.setHandler("remove_authored_pass", [&editor_rpc](const nlohmann::json &params) {
        return invokeEditorRpc([&] { return editor_rpc.removeAuthoredPass(params); });
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
