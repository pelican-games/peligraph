#pragma once

#include "projectformat.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Pelican {

struct EngineResourceId {
    std::string id;
};

enum class AssetFragmentAddressKind {
    name,
    full_path,
};

struct AssetFragmentRef {
    std::string kind;
    std::string path;
    AssetFragmentAddressKind address_kind = AssetFragmentAddressKind::name;
};

struct ParsedPathRef {
    std::string path;
    std::optional<AssetFragmentRef> fragment;
};

struct ResolvedPathFragment {
    std::filesystem::path path;
    AssetFragmentRef fragment;
};

struct ResolvedEngineFragment {
    EngineResourceId resource;
    AssetFragmentRef fragment;
};

using ResolvedRef =
    std::variant<std::filesystem::path, EngineResourceId,
                 ResolvedPathFragment, ResolvedEngineFragment>;

struct AssetStoreStatus {
    std::string name;
    std::string mount;
    std::filesystem::path root;
    std::optional<std::filesystem::path> manifest;
    std::optional<std::string> manifest_error;
};

struct PathResolutionResult {
    ResolvedRef reference;
    std::vector<std::string> warnings;
};

using EngineResourceTextLoader =
    std::function<std::string(std::string_view)>;

ParsedPathRef parsePathReference(std::string_view ref);
std::string normalizedResolvedReferenceKey(const ResolvedRef &reference);

class ProjectPathResolver {
    bool configured = false;
    std::filesystem::path project_root_abs;
    std::optional<std::filesystem::path> user_root_abs;
    std::optional<std::string> project_id;
    bool allow_absolute_paths = false;

    struct AssetStoreMount {
        std::string name;
        std::string mount;
        std::filesystem::path logical_mount;
        std::filesystem::path root_abs;
        std::optional<std::filesystem::path> manifest_abs;
        std::optional<std::string> manifest_error;
    };
    std::vector<AssetStoreMount> asset_stores;

    void setupImpl(
        const std::filesystem::path &project_root,
        bool allow_absolute,
        const ProjectEnvelope *project,
        std::optional<std::filesystem::path> user_dir_override);
    PathResolutionResult resolveRef(std::string_view ref,
                                    bool cli_origin) const;

  public:
    void setup(const std::filesystem::path &project_root,
               bool allow_absolute);
    void setup(const std::filesystem::path &project_root,
               bool allow_absolute,
               const ProjectEnvelope &project,
               std::optional<std::filesystem::path> user_dir_override =
                   std::nullopt);
    void reset();

    bool isSetup() const { return configured; }
    const std::filesystem::path &projectRoot() const {
        return project_root_abs;
    }
    std::vector<AssetStoreStatus> stores() const;

    PathResolutionResult resolveProjectRef(std::string_view ref) const;
    PathResolutionResult resolveCliRef(std::string_view ref) const;
    ResolvedRef resolveExistingFileReference(std::string_view ref) const;
    std::filesystem::path resolveExistingFile(std::string_view ref) const;
    std::string normalizedReference(std::string_view ref) const;
    std::string loadText(
        std::string_view ref,
        const EngineResourceTextLoader &load_engine_resource = {}) const;
    std::vector<std::byte> loadBytes(
        std::string_view ref,
        const EngineResourceTextLoader &load_engine_resource = {}) const;
};

} // namespace Pelican
