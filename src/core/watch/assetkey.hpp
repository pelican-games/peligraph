#pragma once

#include <compare>
#include <filesystem>
#include <string>

namespace Pelican::watch {

// Asset identity at the HR0 boundary.  HR1 may add logical resource identity,
// but watcher events are always keyed by a canonical store-relative path.
struct AssetKey {
    std::string store;
    std::string path;

    auto operator<=>(const AssetKey &) const = default;
};

AssetKey makeAssetKey(std::string store, const std::filesystem::path &relative_path);
std::string assetKeyString(const AssetKey &key);

} // namespace Pelican::watch

