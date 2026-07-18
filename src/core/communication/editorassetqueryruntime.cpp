#include "editorassetqueryruntime.hpp"

#include "editorcommandservice.hpp"
#include "../container.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <variant>

namespace Pelican {

namespace {

bool pathIsWithin(const std::filesystem::path &path, const std::filesystem::path &root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

std::vector<EditorAssetQueryResult> collectEditorAssets(const ProjectBasicConfig &project_config,
                                                        const PathResolver &path_resolver) {
    std::vector<EditorAssetQueryResult> result;
    const auto asset_data = nlohmann::json::parse(project_config.assetDataJson());
    const auto stores = path_resolver.stores();
    if (!asset_data.is_object()) return result;

    for (auto category = asset_data.begin(); category != asset_data.end(); ++category) {
        if (!category.value().is_array()) continue;
        auto kind = category.key();
        if (kind.size() > 1 && kind.back() == 's') kind.pop_back();

        for (const auto &entry : category.value()) {
            if (!entry.is_object() || !entry.contains("name") ||
                !entry.at("name").is_string() || !entry.contains("path") ||
                !entry.at("path").is_string()) {
                continue;
            }

            const auto path_text = entry.at("path").get<std::string>();
            std::filesystem::path resolved_path{path_text};
            bool exists = false;
            if (resolved_path.is_absolute()) {
                exists = std::filesystem::exists(resolved_path);
            } else {
                try {
                    const auto resolved = path_resolver.resolveExistingFileReference(path_text);
                    if (const auto *path = std::get_if<std::filesystem::path>(&resolved)) {
                        resolved_path = *path;
                        exists = std::filesystem::exists(resolved_path);
                    } else if (const auto *fragment =
                                   std::get_if<ResolvedPathFragment>(&resolved)) {
                        resolved_path = fragment->path;
                        exists = std::filesystem::exists(resolved_path);
                    } else {
                        exists = true;
                    }
                } catch (const std::exception &) {
                    exists = false;
                }
            }

            std::string store = "project";
            if (resolved_path.is_absolute()) {
                for (const auto &candidate : stores) {
                    if (pathIsWithin(resolved_path.lexically_normal(),
                                     candidate.root.lexically_normal())) {
                        store = candidate.name;
                        break;
                    }
                }
            }

            result.push_back(EditorAssetQueryResult{
                .id = entry.at("name").get<std::string>(),
                .kind = kind,
                .path = path_text,
                .store = std::move(store),
                .status = exists ? "loaded" : "missing",
            });
        }
    }
    return result;
}

} // namespace

std::unique_ptr<EditorCommandService> makeInteractiveEditorAssetQueryService() {
    auto &project_config = GET_MODULE(ProjectBasicConfig);
    auto &path_resolver = GET_MODULE(PathResolver);
    return std::make_unique<EditorCommandService>(EditorCommandServiceDependencies{
        .document = [&project_config]() -> const AuthoringSceneDocument & {
            return project_config.sceneDocument();
        },
        .current_scene_id = [&project_config] { return project_config.defaultSceneId(); },
        .assets = [&project_config, &path_resolver] {
            return collectEditorAssets(project_config, path_resolver);
        },
    });
}

} // namespace Pelican
