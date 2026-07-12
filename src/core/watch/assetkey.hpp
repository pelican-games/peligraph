#pragma once

#include <compare>
#include <filesystem>
#include <string>
#include <string_view>

namespace Pelican::watch {

// Canonical project identity. Physical paths and store roots deliberately do
// not participate: local overrides may move bytes without changing meaning.
struct AssetKey {
    std::string path;
    std::string fragment;

    auto operator<=>(const AssetKey &) const = default;
};

AssetKey makeAssetKey(std::string_view project_reference);
AssetKey makeAssetKey(std::string_view logical_mount,
                      const std::filesystem::path &relative_path,
                      std::string_view fragment = {});
std::string assetKeyString(const AssetKey &key);

} // namespace Pelican::watch

