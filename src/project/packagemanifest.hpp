#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// X-1: a package is a directory with a `pelican.package` manifest.  It is the
// one carrier that can leave the engine tree: the author writes it anywhere,
// and installing it copies the whole directory into
// `<project>/packages/<namespace>/`.  Nothing in the engine resolves a package
// outside a project, so "where it came from" stops mattering the moment it is
// vendored.
inline constexpr std::string_view packageManifestSchema = "pelican.package";
inline constexpr int packageManifestVersion = 1;
inline constexpr std::string_view packageManifestFile = "package.json";
inline constexpr std::string_view packageVendorDirectory = "packages";
inline constexpr std::string_view packageProjectKey = "packages";

// Carries the rejection code, so neither the RPC nor the CLI has to re-derive
// a reason from a message.
class PackageError : public std::runtime_error {
    std::string code_;
    std::vector<std::string> names_;

  public:
    PackageError(std::string code, const std::string &message,
                 std::vector<std::string> names = {});
    const std::string &code() const noexcept { return code_; }
    // The offenders the message enumerates (missing dependencies, references
    // that still hold the package).  Empty for rejections that name nothing.
    const std::vector<std::string> &names() const noexcept { return names_; }
};

struct PackageDependency {
    std::string name_space;
    std::string package_version;
};

struct PackageProvides {
    // Relative paths inside the package directory.
    std::vector<std::string> features;
    std::vector<std::string> graphs;
    std::vector<std::string> components;
    bool code = false;
};

struct PackageRequirements {
    std::string engine_min_version;
    // Only packages that ship code declare an SDK epoch.
    std::optional<int> sdk_abi;
    std::vector<std::string> capabilities;
};

struct PackageManifest {
    std::string name_space;
    std::string package_version;
    std::string description;
    PackageRequirements requirements;
    std::vector<PackageDependency> dependencies;
    PackageProvides provides;
};

// One entry of `project.json.packages`.
struct InstalledPackage {
    std::string name_space;
    std::string package_version;
    std::string sha256;
};

// The SDK epoch this engine loads game code with.  `requires.sdk_abi` is
// checked against it at install time; the real gate stays the epoch anchor the
// loader resolves when the DLL is loaded.
int packageSdkAbiVersion() noexcept;

// `package_manifest_invalid` for anything malformed, `package_namespace_invalid`
// for a namespace that is not dot-separated identifiers, `package_path_invalid`
// for a provided path that is absolute, escaping, backslashed or schemed.
PackageManifest parsePackageManifestJson(const nlohmann::json &manifest);

// Adds `package_manifest_missing` when the directory holds no package.json.
PackageManifest readPackageManifest(const std::filesystem::path &package_dir);

// `<namespace>.<authored name>`: the name the vendored copy of a feature
// document carries.  Every place that compares feature names reads the
// document's own `name`, so the namespace has to live there and not only in
// the catalog.
std::string qualifiedPackageFeatureName(std::string_view name_space,
                                        std::string_view authored_name);

// Digest over the installed bytes: every regular file under the directory, in
// sorted relative-path order.  Recomputed by `package list`, so the digest
// recorded in project.json is checked by a production path and not only
// written.
std::string packageTreeDigest(const std::filesystem::path &package_dir);

std::vector<InstalledPackage>
readInstalledPackagesJson(const nlohmann::json &project);
std::vector<InstalledPackage>
readInstalledPackages(const std::filesystem::path &project_root);

struct PackageInstallOptions {
    std::string engine_version = "0.1.0";
    // Left unset, the engine's own epoch is used.
    std::optional<int> sdk_abi;
};

struct PackageInstallResult {
    PackageManifest manifest;
    std::filesystem::path destination;
    std::string sha256;
    // `project://packages/<ns>/features/<file>` for each provided feature.
    std::vector<std::string> feature_references;
    // The namespaced names those documents carry after the copy.
    std::vector<std::string> feature_names;
};

// Copies the package into `<project_root>/packages/<namespace>/` and records
// it in project.json.  Every check runs before a byte is copied; the copy
// itself is a staging directory renamed into place.
PackageInstallResult installPackage(const std::filesystem::path &source_dir,
                                    const std::filesystem::path &project_root,
                                    const PackageInstallOptions &options = {});

// Removes the vendored directory and the project.json entry.  `in_use` names
// the references that still point into the package; a non-empty list is
// `package_in_use` and nothing is removed.
void uninstallPackage(const std::filesystem::path &project_root,
                      std::string_view name_space,
                      const std::vector<std::string> &in_use);

// The reference prefix a document uses to reach into one installed package.
std::string packageReferencePrefix(std::string_view name_space);

// Every provided feature of one installed package, as
// `{reference, name, bytes}`.  Throws PackageError for the package as a whole
// (missing manifest, unreadable directory); a single unreadable document is
// reported through `error` instead, because the catalog must never fail to
// build.
struct PackageFeatureDocument {
    std::string reference;
    std::string name;
    std::string bytes;
    // Empty when the document loaded and carries the namespaced name.
    std::string error;
};

std::vector<PackageFeatureDocument>
readPackageFeatureDocuments(const std::filesystem::path &project_root,
                            std::string_view name_space);

} // namespace Pelican
