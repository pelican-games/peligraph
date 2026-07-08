#pragma once

#include "../container.hpp"

#include <cstddef>
#include <filesystem>
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
    std::variant<std::filesystem::path, EngineResourceId, ResolvedPathFragment, ResolvedEngineFragment>;

struct AssetStoreStatus {
    std::string name;
    std::string mount;
    std::filesystem::path root;
};

ParsedPathRef parsePathReference(std::string_view ref);

DECLARE_MODULE(PathResolver) {
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
    };
    std::vector<AssetStoreMount> asset_stores;

    ResolvedRef resolveRef(std::string_view ref, bool cli_origin) const;

  public:
    void setup(const std::filesystem::path &project_root_abs, bool allow_absolute_paths);
    void setup(const std::filesystem::path &project_root_abs, bool allow_absolute_paths,
               std::string_view project_json,
               std::optional<std::filesystem::path> user_dir_override = std::nullopt);
    void resetForTesting();

    bool isSetup() const { return configured; }
    std::vector<AssetStoreStatus> stores() const;

    ResolvedRef resolveProjectRef(std::string_view ref) const;
    ResolvedRef resolveCliRef(std::string_view ref) const;
    std::filesystem::path resolveExistingFile(std::string_view ref) const;
    std::string loadText(std::string_view ref) const;
    std::vector<std::byte> loadBytes(std::string_view ref) const;
};

} // namespace Pelican
