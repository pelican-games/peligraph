#pragma once

#include <functional>

namespace Pelican {

class EditorCommandRpcAdapter;
class RpcServer;

struct EditorRpcHandlerHooks {
    std::function<void()> snapshot_imported;
    std::function<bool()> save_busy;
};

// Registers only the editor/authoring RPC surface. Transport concerns and
// engine-domain handlers remain owned by RpcServer/EngineRpcEndpoint.
void configureEditorRpcHandlers(RpcServer &server,
                                EditorCommandRpcAdapter &editor_rpc,
                                EditorRpcHandlerHooks hooks);

} // namespace Pelican
