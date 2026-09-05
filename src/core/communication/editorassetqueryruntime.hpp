#pragma once

#include <memory>
#include <vector>
#include <string_view>

namespace Pelican {

class EditorCommandService;
class PathResolver;
struct EditorAssetQueryResult;

std::vector<EditorAssetQueryResult> collectEditorAssets(
    std::string_view asset_data_json, const PathResolver &path_resolver);

// Builds the read-only service used by interactive editor tools.  The UI only
// receives the typed service; project inventory collection remains on this
// side of the communication boundary.
std::unique_ptr<EditorCommandService> makeInteractiveEditorAssetQueryService();

} // namespace Pelican
