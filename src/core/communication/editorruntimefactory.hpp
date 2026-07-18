#pragma once

#include <memory>

namespace Pelican {

class EditorCommandService;

// Creates the single production editor composition used by either the RPC
// endpoint or the interactive ImGui runtime. Deterministic drivers never
// create the interactive runtime, so the editable surface remains absent.
std::unique_ptr<EditorCommandService> makeEditorRuntimeService();

} // namespace Pelican
