#pragma once

#include <memory>
#include <vector>
#include <string_view>

namespace Pelican {

class EditorCommandService;
class PathResolver;
class ProjectBasicConfig;
struct EditorAssetQueryResult;

// list_assets' entries from the engine's published asset_data. EB-1:
// `authored_asset_data_json` is the document that copy was made from (empty:
// none); each entry's `path` is its spelling there, the file it resolved to
// `resolved_path`. Without it a resolved path inside the project is listed
// project-relative. A materials[] entry is listed once per material its
// document holds (id: the material's name), or by its path with status
// "missing" / "invalid" when the document does not read.
std::vector<EditorAssetQueryResult> collectEditorAssets(
    std::string_view asset_data_json, const PathResolver &path_resolver,
    std::string_view authored_asset_data_json = {});

// EB-1: collectEditorAssets of the engine's published asset_data, with the
// project file as the authored document when the published copy was made
// from it.
std::vector<EditorAssetQueryResult> collectProjectEditorAssets(
    const ProjectBasicConfig &project_config, PathResolver &path_resolver);

// Builds the read-only service used by interactive editor tools.  The UI only
// receives the typed service; project inventory collection remains on this
// side of the communication boundary.
std::unique_ptr<EditorCommandService> makeInteractiveEditorAssetQueryService();

} // namespace Pelican
