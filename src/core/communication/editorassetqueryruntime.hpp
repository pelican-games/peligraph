#pragma once

#include <memory>

namespace Pelican {

class EditorCommandService;

// Builds the read-only service used by interactive editor tools.  The UI only
// receives the typed service; project inventory collection remains on this
// side of the communication boundary.
std::unique_ptr<EditorCommandService> makeInteractiveEditorAssetQueryService();

} // namespace Pelican
