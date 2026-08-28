#include "pathresolver.hpp"

#include "../log.hpp"
#include "engineresources.hpp"

#include <sstream>
#include <utility>

namespace Pelican {
namespace {

EngineResourceTextLoader engineResourceLoader() {
    return [](std::string_view id) {
        return std::string{engineResourceOrThrow(id)};
    };
}

} // namespace

void PathResolver::logDiagnostics(const PathResolutionResult &result) {
    for (const auto &warning : result.warnings) {
        LOG_WARNING(logger, "{}", warning);
    }
}

void PathResolver::setup(const std::filesystem::path &project_root,
                         bool allow_absolute_paths) {
    resolver.setup(project_root, allow_absolute_paths);
}

void PathResolver::setup(
    const std::filesystem::path &project_root, bool allow_absolute_paths,
    const ProjectEnvelope &project,
    std::optional<std::filesystem::path> user_dir_override) {
    resolver.setup(project_root, allow_absolute_paths, project,
                   std::move(user_dir_override));

    const auto asset_stores = resolver.stores();
    if (asset_stores.empty()) {
        return;
    }
    std::ostringstream summary;
    for (std::size_t i = 0; i < asset_stores.size(); ++i) {
        if (i > 0) {
            summary << ", ";
        }
        summary << asset_stores[i].name << "="
                << asset_stores[i].root.string();
    }
    LOG_INFO(logger, "asset stores resolved: {}", summary.str());
}

void PathResolver::setup(
    const std::filesystem::path &project_root, bool allow_absolute_paths,
    std::string_view project_json,
    std::optional<std::filesystem::path> user_dir_override) {
    const auto project = parseProjectEnvelopeText(project_json);
    setup(project_root, allow_absolute_paths, project.envelope,
          std::move(user_dir_override));
}

void PathResolver::resetForTesting() {
    resolver.reset();
}

std::vector<AssetStoreStatus> PathResolver::stores() const {
    return resolver.stores();
}

ResolvedRef PathResolver::resolveProjectRef(std::string_view ref) const {
    auto result = resolver.resolveProjectRef(ref);
    logDiagnostics(result);
    return std::move(result.reference);
}

ResolvedRef PathResolver::resolveCliRef(std::string_view ref) const {
    auto result = resolver.resolveCliRef(ref);
    logDiagnostics(result);
    return std::move(result.reference);
}

ResolvedRef
PathResolver::resolveExistingFileReference(std::string_view ref) const {
    return resolver.resolveExistingFileReference(ref);
}

std::filesystem::path
PathResolver::resolveExistingFile(std::string_view ref) const {
    return resolver.resolveExistingFile(ref);
}

std::string PathResolver::normalizedReference(std::string_view ref) const {
    return resolver.normalizedReference(ref);
}

ShaderSourceOpenResolution
PathResolver::resolveShaderSourceOpenReference(
    std::string_view stem, ShaderSourceStage stage) const {
    return Pelican::resolveShaderSourceOpenReference(
        resolver, stem, stage,
        [](std::string_view id) {
            if (engineResource(id)) {
                return true;
            }
            std::string spirv{id};
            spirv += ".spv";
            return engineResource(spirv).has_value();
        });
}

std::string PathResolver::loadText(std::string_view ref) const {
    return resolver.loadText(ref, engineResourceLoader());
}

std::vector<std::byte> PathResolver::loadBytes(std::string_view ref) const {
    return resolver.loadBytes(ref, engineResourceLoader());
}

} // namespace Pelican
