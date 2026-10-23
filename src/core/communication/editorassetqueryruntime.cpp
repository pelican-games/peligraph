#include "editorassetqueryruntime.hpp"

#include "editorcommandservice.hpp"
#include "../container.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../model/engineprimitives.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace Pelican {

namespace {

bool pathIsWithin(const std::filesystem::path &path, const std::filesystem::path &root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && *relative.begin() != "..";
}

// GP-5b: an engine primitive has no file and no project store. It is listed
// under the store "engine" with the status "builtin" whether it is the bare
// reference or a model declared in asset_data with that reference as its path.
constexpr std::string_view engine_store = "engine";
constexpr std::string_view builtin_status = "builtin";

bool isKnownEnginePrimitive(std::string_view reference) {
    if (!isEnginePrimitiveReference(reference)) return false;
    const auto names = enginePrimitiveNames();
    return std::find(names.begin(), names.end(), reference.substr(enginePrimitivePrefix.size())) !=
           names.end();
}

std::string fileText(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("cannot read " + path.generic_string());
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// EB-1: the spelling asset_data writes for entry `index` of `category`, when
// the authored document is the one the published copy was made from (the
// published copy holds a model's path resolved, an absolute path).
std::optional<std::string> authoredPath(const nlohmann::json &authored, const std::string &category,
                                        std::size_t index, const std::string &name) {
    if (!authored.is_object()) return std::nullopt;
    const auto list = authored.find(category);
    if (list == authored.end() || !list->is_array() || index >= list->size()) return std::nullopt;
    const auto &entry = list->at(index);
    if (!entry.is_object() || entry.value("name", std::string{}) != name || !entry.contains("path") ||
        !entry.at("path").is_string())
        return std::nullopt;
    return entry.at("path").get<std::string>();
}

// EB-1: a resolved file inside the project, project-relative; nullopt when
// it lies outside.
std::optional<std::string> projectRelativeOf(const std::filesystem::path &file, const PathResolver &path_resolver) {
    if (!path_resolver.isSetup()) return std::nullopt;
    const auto root = path_resolver.projectRoot().lexically_normal();
    const auto normal = file.lexically_normal();
    if (!pathIsWithin(normal, root)) return std::nullopt;
    const auto relative = normal.lexically_relative(root).generic_u8string();
    return std::string{relative.begin(), relative.end()};
}

} // namespace

std::vector<EditorAssetQueryResult> collectProjectEditorAssets(const ProjectBasicConfig &project_config,
                                                               PathResolver &path_resolver) {
    const auto published = project_config.assetDataJson();
    std::string authored;
    try {
        auto raw = path_resolver.loadText(project_config.assetDataReference());
        // Only the document the engine's copy was made from says how each
        // published path was written.
        if (project_config.prepareAssetDataJson(raw, path_resolver) == published) authored = std::move(raw);
    } catch (const std::exception &) {
    }
    return collectEditorAssets(published, path_resolver, authored);
}

std::vector<EditorAssetQueryResult> collectEditorAssets(std::string_view asset_data_json,
                                                        const PathResolver &path_resolver,
                                                        std::string_view authored_asset_data_json) {
    std::vector<EditorAssetQueryResult> result;
    const auto asset_data = nlohmann::json::parse(asset_data_json);
    const auto authored = authored_asset_data_json.empty()
                              ? nlohmann::json{}
                              : nlohmann::json::parse(authored_asset_data_json, nullptr, false);
    const auto stores = path_resolver.stores();

    // GP-5b: the primitives are always there, first, so a model picker built
    // from this list offers them ahead of the project's own models. The id is
    // the reference itself: that is the value simplemodelview.model takes.
    for (const auto name : enginePrimitiveNames()) {
        auto reference = std::string{enginePrimitivePrefix} + std::string{name};
        result.push_back(EditorAssetQueryResult{.id = reference,
                                                .kind = "model",
                                                .path = reference,
                                                .store = std::string{engine_store},
                                                .status = std::string{builtin_status}});
    }
    if (!asset_data.is_object()) return result;

    for (auto category = asset_data.begin(); category != asset_data.end(); ++category) {
        if (!category.value().is_array()) continue;
        auto kind = category.key();
        if (kind.size() > 1 && kind.back() == 's') kind.pop_back();

        for (std::size_t index = 0; index < category.value().size(); ++index) {
            const auto &entry = category.value().at(index);
            // EB-1: a material entry is {path} alone; its materials are
            // listed by the names its document holds.
            if (category.key() == "materials" && entry.is_object() && entry.contains("path") &&
                entry.at("path").is_string()) {
                const auto path_text = entry.at("path").get<std::string>();
                EditorAssetQueryResult listed{.id = path_text, .kind = "material", .path = path_text,
                                              .store = "project", .status = "missing"};
                std::filesystem::path file;
                try {
                    file = path_resolver.resolveExistingFile(path_text);
                } catch (const std::exception &) {
                    result.push_back(std::move(listed));
                    continue;
                }
                listed.resolved_path = file.generic_string();
                for (const auto &candidate : stores) {
                    if (pathIsWithin(file.lexically_normal(), candidate.root.lexically_normal())) {
                        listed.store = candidate.name;
                        break;
                    }
                }
                std::vector<std::string> names;
                bool readable = false;
                try {
                    const auto document = nlohmann::json::parse(fileText(file));
                    if (document.is_object() && document.value("schema", std::string{}) == "pelican.material" &&
                        document.contains("materials") && document.at("materials").is_array()) {
                        readable = true;
                        for (const auto &material : document.at("materials"))
                            if (material.is_object() && material.contains("name") && material.at("name").is_string())
                                names.push_back(material.at("name").get<std::string>());
                    }
                } catch (const std::exception &) {
                }
                if (!readable) {
                    listed.status = "invalid";
                    result.push_back(std::move(listed));
                    continue;
                }
                listed.status = "loaded";
                for (const auto &name : names) {
                    auto material = listed;
                    material.id = name;
                    result.push_back(std::move(material));
                }
                continue;
            }
            if (!entry.is_object() || !entry.contains("name") || !entry.at("name").is_string()) {
                continue;
            }

            if (category.key() == "procedural_geometry") {
                result.push_back({.id = entry.at("name").get<std::string>(),
                    .kind = "procedural_geometry", .store = "project", .status = "declared"});
                continue;
            }
            if (!entry.contains("path") || !entry.at("path").is_string()) continue;

            const auto path_text = entry.at("path").get<std::string>();
            // GP-5b: a model declared with an engine primitive as its path is
            // generated, not read, so the file resolver below would call it
            // missing. An unknown primitive name does not resolve: missing.
            if (category.key() == "models" && isEnginePrimitiveReference(path_text)) {
                result.push_back(EditorAssetQueryResult{
                    .id = entry.at("name").get<std::string>(),
                    .kind = kind,
                    .path = path_text,
                    .store = std::string{engine_store},
                    .status = isKnownEnginePrimitive(path_text) ? std::string{builtin_status}
                                                                : std::string{"missing"},
                });
                continue;
            }
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

            // EB-1: `path` as asset_data writes it; the published copy holds
            // a model's path resolved, which goes to resolved_path.
            const auto name = entry.at("name").get<std::string>();
            std::string listed_path = path_text;
            if (const auto written = authoredPath(authored, category.key(), index, name)) {
                listed_path = *written;
            } else if (std::filesystem::path{path_text}.is_absolute()) {
                if (const auto relative = projectRelativeOf(std::filesystem::path{path_text}, path_resolver))
                    listed_path = *relative;
            }
            std::optional<std::string> resolved;
            if (resolved_path.is_absolute()) resolved = resolved_path.generic_string();
            result.push_back(EditorAssetQueryResult{
                .id = name,
                .kind = kind,
                .path = std::move(listed_path),
                .store = std::move(store),
                .status = exists ? "loaded" : "missing",
                .resolved_path = std::move(resolved),
            });
        }
    }
    return result;
}

std::unique_ptr<EditorCommandService> makeInteractiveEditorAssetQueryService() {
    auto &project_config = GET_MODULE(ProjectBasicConfig);
    auto &path_resolver = GET_MODULE(PathResolver);
    return std::make_unique<EditorCommandService>(EditorCommandServiceDependencies{
        .document = [&project_config]() -> const AuthoringSceneDocument & {
            return project_config.sceneDocument();
        },
        .resolved_scene = [&project_config]() -> const ResolvedScene & {
            return project_config.resolvedScene();
        },
        .current_scene_id = [&project_config] { return project_config.defaultSceneId(); },
        .assets = [&project_config, &path_resolver] {
            return collectProjectEditorAssets(project_config, path_resolver);
        },
    });
}

} // namespace Pelican
