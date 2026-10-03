#pragma once

#include "projectformat.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
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

// PJ-1: project.json's asset_stores written in place (edit_project_settings)
// would change how a reference already resolves, or what a store is verified
// against: a mount or root that moves, a store removed, a manifest replaced
// or removed. Only what this engine itself added in place may be taken back.
inline constexpr std::string_view projectSettingsChangeUnsupportedCode =
    "project.settings_change_unsupported@1";
// PJ-1: the asset_stores a settings edit names fail the checks the engine
// opens a project with (or name a manifest it cannot resolve).
inline constexpr std::string_view projectSettingsInvalidCode =
    "project.settings_invalid@1";

class AssetStoresChangeRefused : public std::runtime_error {
    std::string code_;
    std::string store_;
    std::string reason_;

  public:
    AssetStoresChangeRefused(std::string_view code, std::string store,
                             std::string reason);
    const std::string &code() const noexcept { return code_; }
    // Empty when the refusal is about asset_stores as a whole.
    const std::string &store() const noexcept { return store_; }
    const std::string &reason() const noexcept { return reason_; }
};

// PJ-1: what a new asset_stores declaration does to the stores in force.
// Names in declaration order of the new (added, declared) or the old
// (removed) list.
struct AssetStoresChange {
    std::vector<std::string> added_stores;
    // Only stores this engine added in place.
    std::vector<std::string> removed_stores;
    // A manifest declared where the store had none (a store added with one
    // included).
    std::vector<std::string> declared_manifests;
    // Only manifests this engine declared in place.
    std::vector<std::string> removed_manifests;

    bool empty() const noexcept {
        return added_stores.empty() && removed_stores.empty() &&
               declared_manifests.empty() && removed_manifests.empty();
    }
};

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
    // HW-1: project.json `hot_reload`, as the envelope parsed it.
    std::optional<std::vector<std::string>> hot_reload_paths;
    // PJ-1: the stores and manifests replaceDeclarations added, the only
    // ones a later declaration may take back.
    std::vector<std::string> stores_added_in_place;
    std::vector<std::string> manifests_declared_in_place;

    // The asset_stores declaration read the way setup reads it (with
    // .pelican/local.json applied), against `project_root` (canonical).
    static std::vector<AssetStoreMount> parseAssetStores(
        const std::filesystem::path &project_root,
        const nlohmann::json &declared);
    AssetStoresChange planChange(
        const std::vector<AssetStoreMount> &next) const;

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
    // HW-1: the logical paths project.json declares for the file watch;
    // nullopt when it declares none (or the resolver has no project.json).
    const std::optional<std::vector<std::string>> &hotReloadPaths() const {
        return hot_reload_paths;
    }

    // PJ-1: what replacing project.json's asset_stores with `declared` would
    // do, checked as setup checks it. A declaration that fails those checks,
    // or names a manifest that cannot be resolved where none was declared,
    // throws AssetStoresChangeRefused(projectSettingsInvalidCode); one that
    // would move a mount or root, remove a store or change a manifest this
    // engine did not add in place, throws it with
    // projectSettingsChangeUnsupportedCode. A store is added only where its
    // root is the project folder its mount names (no .pelican/local.json
    // override), so every reference under the mount resolves to the file it
    // resolved to before. Requires setup.
    AssetStoresChange planDeclarations(const nlohmann::json &declared) const;
    // PJ-1: applies planDeclarations(declared) (throwing as it does, and then
    // changing nothing) and replaces the hot_reload declaration. The caller
    // keeps every reader of the resolver off other threads meanwhile (the
    // engine calls it from an RPC handler, at a frame boundary).
    AssetStoresChange replaceDeclarations(
        const nlohmann::json &declared,
        std::optional<std::vector<std::string>> hot_reload);

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
