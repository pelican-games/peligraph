#pragma once

#include "../../project/projectpathresolver.hpp"
#include "../../project/shadersourceresolver.hpp"
#include "../container.hpp"

namespace Pelican {

DECLARE_MODULE(PathResolver) {
    ProjectPathResolver resolver;

    static void logDiagnostics(const PathResolutionResult &result);

  public:
    void setup(const std::filesystem::path &project_root,
               bool allow_absolute_paths);
    void setup(const std::filesystem::path &project_root,
               bool allow_absolute_paths,
               const ProjectEnvelope &project,
               std::optional<std::filesystem::path> user_dir_override =
                   std::nullopt);
    void setup(const std::filesystem::path &project_root,
               bool allow_absolute_paths,
               std::string_view project_json,
               std::optional<std::filesystem::path> user_dir_override =
                   std::nullopt);
    void resetForTesting();

    bool isSetup() const { return resolver.isSetup(); }
    const std::filesystem::path &projectRoot() const {
        return resolver.projectRoot();
    }
    std::vector<AssetStoreStatus> stores() const;

    ResolvedRef resolveProjectRef(std::string_view ref) const;
    ResolvedRef resolveCliRef(std::string_view ref) const;
    ResolvedRef resolveExistingFileReference(std::string_view ref) const;
    std::filesystem::path resolveExistingFile(std::string_view ref) const;
    std::string normalizedReference(std::string_view ref) const;
    ShaderSourceOpenResolution resolveShaderSourceOpenReference(
        std::string_view stem, ShaderSourceStage stage) const;
    std::string loadText(std::string_view ref) const;
    std::vector<std::byte> loadBytes(std::string_view ref) const;
};

} // namespace Pelican
