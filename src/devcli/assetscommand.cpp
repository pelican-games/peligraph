#include "assetscommand.hpp"

#include "../project/assetsmanifest.hpp"

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Pelican::DevCli {

namespace {

struct AssetStoreConfig {
    std::string name;
    std::string mount;
    std::filesystem::path root;
    std::optional<std::filesystem::path> manifest;
};

struct AssetsProjectConfig {
    std::filesystem::path root;
    std::filesystem::path cache;
    std::vector<AssetStoreConfig> stores;
};

std::string pathString(const std::filesystem::path &path) { return path.string(); }

std::filesystem::path absolutePath(const std::filesystem::path &path) {
    return path.is_absolute() ? path : std::filesystem::current_path() / path;
}

std::filesystem::path weaklyCanonicalOrThrow(const std::filesystem::path &path, std::string_view context) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        throw std::runtime_error(std::string{context} + " failed to normalize path: " + pathString(path) +
                                 " (" + ec.message() + ")");
    }
    return canonical;
}

std::string readTextFile(const std::filesystem::path &path, std::string_view context) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open " + std::string{context} + ": " + pathString(path));
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

bool pathStartsWith(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    auto root_it = root.begin();
    auto candidate_it = candidate.begin();
    for (; root_it != root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == candidate.end()) {
            return false;
        }
        auto lhs = root_it->native();
        auto rhs = candidate_it->native();
#ifdef _WIN32
        std::transform(lhs.begin(), lhs.end(), lhs.begin(), [](wchar_t ch) { return std::towlower(ch); });
        std::transform(rhs.begin(), rhs.end(), rhs.begin(), [](wchar_t ch) { return std::towlower(ch); });
#endif
        if (lhs != rhs) {
            return false;
        }
    }
    return true;
}

std::string comparablePathString(const std::filesystem::path &path) {
    auto result = path.generic_string();
#ifdef _WIN32
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return result;
}

void validatePortableRelativePath(std::string_view value, std::string_view context) {
    if (value.empty() || value.find('\\') != std::string_view::npos || value.find("://") != std::string_view::npos ||
        value.starts_with('/') || value.starts_with('\\')) {
        throw std::runtime_error(std::string{context} + " must be a portable relative path: " +
                                 std::string{value});
    }
    const std::filesystem::path path{std::string{value}};
    if (path.is_absolute() || path.has_root_name()) {
        throw std::runtime_error(std::string{context} + " must be relative: " + std::string{value});
    }
}

std::filesystem::path resolveProjectManifestPath(const std::filesystem::path &project_root,
                                                 std::string_view value,
                                                 std::string_view context) {
    validatePortableRelativePath(value, context);
    const auto path = weaklyCanonicalOrThrow(project_root / std::filesystem::path{value}, context);
    if (!pathStartsWith(project_root, path) || path == project_root) {
        throw std::runtime_error(std::string{context} + " escapes project root: " + std::string{value});
    }
    return path;
}

AssetsProjectConfig loadProjectConfig(const std::filesystem::path &project_arg) {
    auto path = weaklyCanonicalOrThrow(absolutePath(project_arg), "--project");
    std::filesystem::path project_file;
    std::filesystem::path project_root;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        project_root = path;
        project_file = project_root / "project.json";
    } else if (std::filesystem::is_regular_file(path, ec) && !ec) {
        if (path.filename() != "project.json") {
            throw std::runtime_error("--project file must be named project.json: " + pathString(path));
        }
        project_file = path;
        project_root = path.parent_path();
    } else {
        throw std::runtime_error("--project must point to a directory or project.json file: " +
                                 pathString(path));
    }
    if (!std::filesystem::is_regular_file(project_file, ec) || ec) {
        throw std::runtime_error("project.json file not found: " + pathString(project_file));
    }

    const auto project = nlohmann::json::parse(readTextFile(project_file, "project.json"));
    if (!project.is_object()) {
        throw std::runtime_error("project.json must be an object");
    }
    const auto declared_it = project.find("asset_stores");
    if (declared_it == project.end() || !declared_it->is_object() || declared_it->empty()) {
        throw std::runtime_error("project.json has no asset_stores declarations");
    }

    AssetsProjectConfig result{project_root, project_root / ".pelican" / "assets-hash-cache.json", {}};
    std::set<std::string> manifest_paths;
    for (const auto &[name, store] : declared_it->items()) {
        if (name.empty() || !store.is_object() || !store.contains("mount") ||
            !store.at("mount").is_string()) {
            throw std::runtime_error("project.json asset_stores entries require a name and string mount");
        }
        const auto mount = store.at("mount").get<std::string>();
        validatePortableRelativePath(mount, "asset store mount " + name);
        const auto normalized_mount = std::filesystem::path{mount}.lexically_normal();
        if (normalized_mount.empty() || normalized_mount == ".") {
            throw std::runtime_error("asset store mount must not resolve to project root: " + name);
        }

        for (const auto &existing : result.stores) {
            const auto existing_mount = std::filesystem::path{existing.mount};
            const auto existing_in_new = normalized_mount.lexically_relative(existing_mount);
            const auto new_in_existing = existing_mount.lexically_relative(normalized_mount);
            const auto is_descendant = [](const std::filesystem::path &relative) {
                return !relative.empty() && relative != "." && *relative.begin() != "..";
            };
            if (normalized_mount == existing_mount || is_descendant(existing_in_new) ||
                is_descendant(new_in_existing)) {
                throw std::runtime_error("asset store mounts overlap: " + existing.name + " and " + name);
            }
        }

        AssetStoreConfig parsed{
            name,
            normalized_mount.generic_string(),
            weaklyCanonicalOrThrow(project_root / std::filesystem::path{mount}, "asset store " + name),
            std::nullopt,
        };
        if (const auto manifest = store.find("manifest"); manifest != store.end()) {
            if (!manifest->is_string()) {
                throw std::runtime_error("project.json asset_stores." + name + ".manifest must be a string");
            }
            parsed.manifest = resolveProjectManifestPath(project_root, manifest->get<std::string>(),
                                                         "asset store manifest " + name);
            if (!manifest_paths.insert(comparablePathString(*parsed.manifest)).second) {
                throw std::runtime_error("asset stores must not share a manifest path: " +
                                         pathString(*parsed.manifest));
            }
        }
        result.stores.push_back(std::move(parsed));
    }

    const auto local_path = project_root / ".pelican" / "local.json";
    if (std::filesystem::is_regular_file(local_path, ec) && !ec) {
        const auto local = nlohmann::json::parse(readTextFile(local_path, ".pelican/local.json"));
        if (!local.is_object()) {
            throw std::runtime_error(".pelican/local.json must be an object");
        }
        for (const auto &[key, value] : local.items()) {
            (void)value;
            if (key != "asset_stores") {
                throw std::runtime_error(".pelican/local.json contains unsupported key: " + key);
            }
        }
        if (const auto overrides = local.find("asset_stores"); overrides != local.end()) {
            if (!overrides->is_object()) {
                throw std::runtime_error(".pelican/local.json asset_stores must be an object");
            }
            for (const auto &[name, value] : overrides->items()) {
                const auto found = std::find_if(result.stores.begin(), result.stores.end(),
                                                [&](const AssetStoreConfig &store) {
                                                    return store.name == name;
                                                });
                if (found == result.stores.end()) {
                    throw std::runtime_error(".pelican/local.json references undeclared asset store: " + name);
                }
                if (!value.is_string()) {
                    throw std::runtime_error(".pelican/local.json asset_stores." + name +
                                             " must be a path string");
                }
                auto override_path = std::filesystem::path{value.get<std::string>()};
                if (override_path.is_relative()) {
                    override_path = project_root / override_path;
                }
                found->root = weaklyCanonicalOrThrow(override_path, "asset store override " + name);
            }
        }
    }

    for (size_t i = 0; i < result.stores.size(); ++i) {
        for (size_t j = i + 1; j < result.stores.size(); ++j) {
            if (pathStartsWith(result.stores[i].root, result.stores[j].root) ||
                pathStartsWith(result.stores[j].root, result.stores[i].root)) {
                throw std::runtime_error("asset store roots overlap: " + result.stores[i].name + " and " +
                                         result.stores[j].name);
            }
        }
    }
    return result;
}

std::vector<const AssetStoreConfig *> selectedManifestStores(const AssetsProjectConfig &project,
                                                             std::string_view selected_name) {
    std::vector<const AssetStoreConfig *> result;
    for (const auto &store : project.stores) {
        if (!selected_name.empty() && store.name != selected_name) {
            continue;
        }
        if (!store.manifest) {
            if (!selected_name.empty()) {
                throw std::runtime_error("asset store has no manifest declaration: " + store.name);
            }
            continue;
        }
        result.push_back(&store);
    }
    if (!selected_name.empty() && result.empty()) {
        const auto found = std::find_if(project.stores.begin(), project.stores.end(),
                                        [&](const AssetStoreConfig &store) {
                                            return store.name == selected_name;
                                        });
        if (found == project.stores.end()) {
            throw std::runtime_error("unknown asset store: " + std::string{selected_name});
        }
    }
    if (result.empty()) {
        throw std::runtime_error("project.json has no asset store manifest declarations");
    }
    return result;
}

int runManifestCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli assets manifest");
    program.add_argument("--project").required().metavar("dir|project.json");
    program.add_argument("--store").default_value(std::string{}).metavar("name");
    program.add_argument("--full").flag().help("rehash every asset instead of using the cache");
    program.parse_args(argc, argv);

    const auto project = loadProjectConfig(program.get<std::string>("--project"));
    const auto stores = selectedManifestStores(project, program.get<std::string>("--store"));
    const auto full = program.get<bool>("--full");
    for (const auto *store : stores) {
        const auto generated = generateAssetsManifest(store->root, *store->manifest, project.cache,
                                                      store->name, full);
        std::cout << "manifest " << store->name << ": " << generated.files << " files ("
                  << generated.hashed << " hashed, " << generated.cache_hits << " cached) -> "
                  << pathString(*store->manifest) << std::endl;
    }
    return 0;
}

int runVerifyCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli assets verify");
    program.add_argument("--project").required().metavar("dir|project.json");
    program.add_argument("--store").default_value(std::string{}).metavar("name");
    program.add_argument("--full").flag().help("rehash and report content mismatches");
    program.parse_args(argc, argv);

    const auto project = loadProjectConfig(program.get<std::string>("--project"));
    const auto stores = selectedManifestStores(project, program.get<std::string>("--store"));
    const auto full = program.get<bool>("--full");
    int total_issues = 0;
    for (const auto *store : stores) {
        const auto verified = verifyAssetsManifest(store->root, *store->manifest, project.cache, store->name,
                                                   {.include_content = full,
                                                    .force_rehash = full,
                                                    .strict = false});
        for (const auto &issue : verified.issues) {
            std::cout << assetManifestIssueSeverityName(issue.severity) << " [" << store->name << "] "
                      << issue.message << std::endl;
        }
        std::cout << "verify " << store->name << ": " << verified.matched << " matched, "
                  << countAssetManifestIssues(verified, AssetManifestIssueSeverity::info) << " info, "
                  << countAssetManifestIssues(verified, AssetManifestIssueSeverity::warning)
                  << " warnings; store=" << pathString(store->root) << std::endl;
        total_issues += static_cast<int>(verified.issues.size());
    }
    return total_issues == 0 ? 0 : 1;
}

int runStatusCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli assets status");
    program.add_argument("--project").required().metavar("dir|project.json");
    program.parse_args(argc, argv);

    const auto project = loadProjectConfig(program.get<std::string>("--project"));
    int missing = 0;
    for (const auto &store : project.stores) {
        std::error_code ec;
        const bool store_exists = std::filesystem::is_directory(store.root, ec) && !ec;
        std::cout << (store_exists ? "OK" : "MISSING") << " store " << store.name << ": "
                  << pathString(store.root) << std::endl;
        missing += store_exists ? 0 : 1;
        if (store.manifest) {
            const bool manifest_exists = std::filesystem::is_regular_file(*store.manifest, ec) && !ec;
            std::cout << (manifest_exists ? "OK" : "MISSING") << " manifest " << store.name << ": "
                      << pathString(*store.manifest) << std::endl;
            missing += manifest_exists ? 0 : 1;
            if (manifest_exists) {
                const auto verified = verifyAssetsManifest(
                    store.root, *store.manifest, project.cache, store.name,
                    {.include_content = false, .force_rehash = false, .strict = false});
                for (const auto &issue : verified.issues) {
                    if (issue.kind == AssetManifestIssueKind::missing) {
                        std::cout << "MISSING asset " << store.name << ": "
                                  << pathString(store.root / std::filesystem::path{issue.file}) << std::endl;
                        ++missing;
                    } else if (issue.kind == AssetManifestIssueKind::case_mismatch) {
                        std::cout << "MISMATCH asset " << store.name << ": " << issue.message << std::endl;
                        ++missing;
                    }
                }
            }
        }
    }
    return missing == 0 ? 0 : 1;
}

} // namespace

int runAssetsCommand(int argc, char *argv[]) {
    try {
        if (argc > 1 && std::string_view{argv[1]} == "manifest") {
            return runManifestCommand(argc - 1, argv + 1);
        }
        if (argc > 1 && std::string_view{argv[1]} == "verify") {
            return runVerifyCommand(argc - 1, argv + 1);
        }
        if (argc > 1 && std::string_view{argv[1]} == "status") {
            return runStatusCommand(argc - 1, argv + 1);
        }
        std::cerr << "usage: pelican_cli assets <manifest|verify|status> --project <dir|project.json>"
                  << std::endl;
        return -1;
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        return -1;
    }
}

} // namespace Pelican::DevCli
