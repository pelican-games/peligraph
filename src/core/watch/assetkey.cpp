#include "assetkey.hpp"

#include <stdexcept>

namespace Pelican::watch {
namespace {

std::string utf8Generic(const std::filesystem::path &path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::string canonicalProjectPath(std::string_view input) {
    constexpr std::string_view scheme = "project://";
    if (input.starts_with(scheme)) input.remove_prefix(scheme.size());
    if (input.empty() || input.find('\\') != std::string_view::npos ||
        input.find("://") != std::string_view::npos) {
        throw std::runtime_error("invalid project logical reference: " + std::string{input});
    }

    const std::filesystem::path path{std::string{input}};
    if (path.is_absolute() || path.has_root_name()) {
        throw std::runtime_error("asset key must be project-relative: " + std::string{input});
    }
    const auto normalized = path.lexically_normal();
    for (const auto &component : normalized) {
        if (component == "..") {
            throw std::runtime_error("asset key escapes project root: " + std::string{input});
        }
    }
    auto result = utf8Generic(normalized);
    while (result.starts_with("./")) result.erase(0, 2);
    if (result.empty() || result == ".") {
        throw std::runtime_error("asset key path must not be empty");
    }
    return result;
}

std::string canonicalFragment(std::string_view fragment) {
    if (fragment.empty()) return {};
    if (fragment.starts_with('#')) fragment.remove_prefix(1);
    if (fragment.empty() || fragment.find('#') != std::string_view::npos ||
        fragment.find('\\') != std::string_view::npos || fragment.starts_with('/') ||
        fragment.ends_with('/')) {
        throw std::runtime_error("invalid asset key fragment: " + std::string{fragment});
    }
    return std::string{fragment};
}

} // namespace

AssetKey makeAssetKey(std::string_view project_reference) {
    const auto marker = project_reference.find('#');
    if (marker != std::string_view::npos &&
        project_reference.find('#', marker + 1) != std::string_view::npos) {
        throw std::runtime_error("asset key contains multiple fragments");
    }
    const auto path = project_reference.substr(0, marker);
    const auto fragment = marker == std::string_view::npos
                              ? std::string_view{}
                              : project_reference.substr(marker + 1);
    return {canonicalProjectPath(path), canonicalFragment(fragment)};
}

AssetKey makeAssetKey(std::string_view logical_mount,
                      const std::filesystem::path &relative_path,
                      std::string_view fragment) {
    std::filesystem::path logical_path;
    if (!logical_mount.empty()) logical_path = std::filesystem::path{std::string{logical_mount}};
    logical_path /= relative_path;
    return {canonicalProjectPath(utf8Generic(logical_path)), canonicalFragment(fragment)};
}

std::string assetKeyString(const AssetKey &key) {
    auto result = std::string{"project://"} + key.path;
    if (!key.fragment.empty()) result += "#" + key.fragment;
    return result;
}

} // namespace Pelican::watch
