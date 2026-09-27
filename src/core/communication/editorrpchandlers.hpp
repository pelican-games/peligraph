#pragma once

#include <functional>
#include <string_view>

namespace Pelican {

class EditorCommandRpcAdapter;
class RpcServer;

struct EditorRpcHandlerHooks {
    std::function<void()> snapshot_imported;
    std::function<bool()> save_busy;
    // PM-2. Called with the method's name before save_scene and
    // import_scene_snapshot do anything; it throws the refusal while a play
    // session is running. Optional.
    std::function<void(std::string_view method)> refuse_during_play_session;
};

// Registers only the editor/authoring RPC surface. Transport concerns and
// engine-domain handlers remain owned by RpcServer/EngineRpcEndpoint.
void configureEditorRpcHandlers(RpcServer &server,
                                EditorCommandRpcAdapter &editor_rpc,
                                EditorRpcHandlerHooks hooks);

} // namespace Pelican
