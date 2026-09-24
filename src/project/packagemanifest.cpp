#include "packagemanifest.hpp"

#include "projectformat.hpp"

// The SDK epoch a game DLL imports from its host.  Install-time `sdk_abi` is
// an early, named rejection for the gate the loader already enforces; it is
// not a second gate.
#include "../core/userpublic/gamelogic.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>
#include <picosha2.h>
#include <set>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Pelican {
namespace {

using Json = nlohmann::json;

constexpr std::string_view render_feature_schema = "pelican.render_feature";
constexpr int render_feature_version = 1;

[[noreturn]] void reject(std::string code, const std::string &message,
                         std::vector<std::string> names = {}) {
    throw PackageError(std::move(code), message, std::move(names));
}

bool isIdentifierSegment(std::string_view value) {
    if (value.empty()) return false;
    const auto head = static_cast<unsigned char>(value.front());
    if (std::isalpha(head) == 0 && value.front() != '_') return false;
    return std::all_of(value.begin() + 1, value.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
    });
}

bool isNamespace(std::string_view value) {
    if (value.empty()) return false;
    std::size_t begin = 0;
    while (true) {
        const auto dot = value.find('.', begin);
        const auto segment = value.substr(
            begin, dot == std::string_view::npos ? std::string_view::npos
                                                 : dot - begin);
        if (!isIdentifierSegment(segment)) return false;
        if (dot == std::string_view::npos) return true;
        begin = dot + 1;
    }
}

bool isDottedNumericVersion(std::string_view value, std::size_t parts) {
    std::size_t count = 1;
    if (value.empty()) return false;
    bool digit_in_part = false;
    for (const char ch : value) {
        if (ch == '.') {
            if (!digit_in_part) return false;
            digit_in_part = false;
            ++count;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return false;
        digit_in_part = true;
    }
    return digit_in_part && count == parts;
}

void requireObject(const Json &value, std::string_view context) {
    if (!value.is_object()) {
        reject("package_manifest_invalid",
               std::string{context} + " must be an object");
    }
}

void requireOnlyKeys(const Json &object,
                     std::initializer_list<std::string_view> allowed,
                     std::string_view context) {
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (std::find(allowed.begin(), allowed.end(), field.key()) ==
            allowed.end()) {
            reject("package_manifest_invalid",
                   std::string{context} + " has an unknown key: " + field.key());
        }
    }
}

std::string requireStringKey(const Json &object, std::string_view key,
                             std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        reject("package_manifest_invalid", std::string{context} +
                                               " requires a non-empty string " +
                                               std::string{key});
    }
    return found->get<std::string>();
}

// Package-relative paths are held to `pelican.import` strictness: forward
// slashes, relative, no scheme and no component that leaves the package.
std::string requireRelativePath(const Json &value, std::string_view context) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty()) {
        reject("package_manifest_invalid",
               std::string{context} + " entries must be non-empty strings");
    }
    auto path = value.get<std::string>();
    if (path.find('\\') != std::string::npos) {
        reject("package_path_invalid", std::string{context} +
                                           " must use forward slashes: " + path);
    }
    if (path.find("://") != std::string::npos) {
        reject("package_path_invalid",
               std::string{context} + " must not use a URI scheme: " + path);
    }
    if (path.front() == '/') {
        reject("package_path_invalid",
               std::string{context} + " must be relative: " + path);
    }
    const std::filesystem::path parsed{path};
    if (parsed.is_absolute() || parsed.has_root_name()) {
        reject("package_path_invalid",
               std::string{context} + " must be relative: " + path);
    }
    for (const auto &component : parsed) {
        if (component == "..") {
            reject("package_path_invalid",
                   std::string{context} + " must not escape the package: " +
                       path);
        }
    }
    return path;
}

std::vector<std::string> parsePathArray(const Json &object,
                                        std::string_view key,
                                        std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end()) return {};
    if (!found->is_array()) {
        reject("package_manifest_invalid",
               std::string{context} + " " + std::string{key} +
                   " must be an array");
    }
    std::vector<std::string> paths;
    std::set<std::string> seen;
    for (const auto &entry : *found) {
        auto path =
            requireRelativePath(entry, std::string{context} + " " +
                                           std::string{key});
        if (!seen.insert(path).second) {
            reject("package_manifest_invalid",
                   std::string{context} + " " + std::string{key} +
                       " lists the same path twice: " + path);
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

std::string readBytes(const std::filesystem::path &path,
                      const std::string &code) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        reject(code, "could not open package file: " + path.string());
    }
    return std::string{std::istreambuf_iterator<char>{input},
                       std::istreambuf_iterator<char>{}};
}

void writeBytes(const std::filesystem::path &path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        reject("package_write_failed",
               "could not write package file: " + path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        reject("package_write_failed",
               "could not finish package file: " + path.string());
    }
}

std::string temporaryName(std::string_view name) {
#ifdef _WIN32
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(getpid());
#endif
    return std::string{name} + ".tmp-" + std::to_string(pid);
}

std::vector<std::filesystem::path>
sortedRelativeFiles(const std::filesystem::path &root) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::recursive_directory_iterator it{root, error}, end;
         it != end; it.increment(error)) {
        if (error) break;
        if (!it->is_regular_file(error) || error) continue;
        files.push_back(
            std::filesystem::relative(it->path(), root, error));
    }
    std::ranges::sort(files, [](const auto &lhs, const auto &rhs) {
        return lhs.generic_string() < rhs.generic_string();
    });
    return files;
}

// Copies one directory tree, skipping version-control metadata: a package that
// travels as a repository still vendors as content.
void copyPackageTree(const std::filesystem::path &source,
                     const std::filesystem::path &destination) {
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) {
        reject("package_write_failed",
               "could not create the package staging directory: " +
                   destination.string() + ": " + error.message());
    }
    for (std::filesystem::recursive_directory_iterator it{source, error}, end;
         it != end; it.increment(error)) {
        if (error) {
            reject("package_write_failed",
                   "could not read the package source: " + source.string() +
                       ": " + error.message());
        }
        const auto name = it->path().filename().string();
        if (name == ".git") {
            it.disable_recursion_pending();
            continue;
        }
        const auto relative =
            std::filesystem::relative(it->path(), source, error);
        if (error) {
            reject("package_write_failed",
                   "could not resolve a package source path: " +
                       it->path().string());
        }
        const auto target = destination / relative;
        if (it->is_directory(error)) {
            std::filesystem::create_directories(target, error);
            if (error) {
                reject("package_write_failed",
                       "could not create " + target.string());
            }
            continue;
        }
        if (!it->is_regular_file(error) || error) continue;
        std::filesystem::create_directories(target.parent_path(), error);
        std::filesystem::copy_file(
            it->path(), target,
            std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            reject("package_write_failed",
                   "could not copy " + it->path().string() + " to " +
                       target.string() + ": " + error.message());
        }
    }
}

Json parseFeatureDocument(const std::string &bytes,
                          const std::string &relative_path) {
    Json document;
    try {
        document = Json::parse(bytes);
    } catch (const Json::exception &error) {
        reject("package_feature_invalid",
               "package feature " + relative_path + " is not valid JSON: " +
                   error.what());
    }
    if (!document.is_object() ||
        document.value("schema", std::string{}) !=
            std::string{render_feature_schema} ||
        document.value("version", 0) != render_feature_version) {
        reject("package_feature_invalid",
               "package feature " + relative_path +
                   " must be a pelican.render_feature version 1 document");
    }
    const auto name = document.find("name");
    if (name == document.end() || !name->is_string() ||
        name->get_ref<const std::string &>().empty()) {
        reject("package_feature_invalid",
               "package feature " + relative_path + " requires a string name");
    }
    if (!isIdentifierSegment(name->get_ref<const std::string &>())) {
        reject("package_feature_invalid",
               "package feature " + relative_path +
                   " name must be one identifier; the namespace is added on "
                   "install: " +
                   name->get<std::string>());
    }
    return document;
}

std::filesystem::path projectManifestPath(
    const std::filesystem::path &project_root) {
    return project_root / "project.json";
}

} // namespace

PackageError::PackageError(std::string code, const std::string &message,
                           std::vector<std::string> names)
    : std::runtime_error(message), code_{std::move(code)},
      names_{std::move(names)} {}

int packageSdkAbiVersion() noexcept {
    return static_cast<int>(gameLogicAbiVersion);
}

std::string qualifiedPackageFeatureName(std::string_view name_space,
                                        std::string_view authored_name) {
    return std::string{name_space} + "." + std::string{authored_name};
}

std::string packageReferencePrefix(std::string_view name_space) {
    return "project://" + std::string{packageVendorDirectory} + "/" +
           std::string{name_space} + "/";
}

PackageManifest parsePackageManifestJson(const Json &manifest) {
    requireObject(manifest, "package.json");
    if (manifest.value("schema", std::string{}) !=
        std::string{packageManifestSchema}) {
        reject("package_manifest_invalid",
               "package.json schema must be pelican.package");
    }
    if (!manifest.contains("version") ||
        !manifest.at("version").is_number_integer() ||
        manifest.at("version").get<int>() != packageManifestVersion) {
        reject("package_manifest_invalid",
               "package.json version must be exactly 1");
    }
    requireOnlyKeys(manifest,
                    {"schema", "version", "namespace", "package_version",
                     "description", "requires", "dependencies", "provides"},
                    "package.json");

    PackageManifest result;
    result.name_space = requireStringKey(manifest, "namespace", "package.json");
    if (!isNamespace(result.name_space)) {
        reject("package_namespace_invalid",
               "package namespace must be dot-separated identifiers: " +
                   result.name_space);
    }
    result.package_version =
        requireStringKey(manifest, "package_version", "package.json");
    if (!isDottedNumericVersion(result.package_version, 3)) {
        reject("package_manifest_invalid",
               "package_version must be X.Y.Z: " + result.package_version);
    }
    if (const auto description = manifest.find("description");
        description != manifest.end()) {
        if (!description->is_string()) {
            reject("package_manifest_invalid",
                   "package.json description must be a string");
        }
        result.description = description->get<std::string>();
    }

    const auto requirements = manifest.find("requires");
    if (requirements == manifest.end()) {
        reject("package_manifest_invalid", "package.json requires a requires "
                                           "object with engine_min_version");
    }
    requireObject(*requirements, "package.json requires");
    requireOnlyKeys(*requirements,
                    {"engine_min_version", "sdk_abi", "capabilities"},
                    "package.json requires");
    result.requirements.engine_min_version =
        requireStringKey(*requirements, "engine_min_version",
                         "package.json requires");
    if (!isDottedNumericVersion(result.requirements.engine_min_version, 3)) {
        reject("package_manifest_invalid",
               "requires.engine_min_version must be X.Y.Z: " +
                   result.requirements.engine_min_version);
    }
    if (const auto abi = requirements->find("sdk_abi");
        abi != requirements->end()) {
        if (!abi->is_number_integer() || abi->get<int>() < 0) {
            reject("package_manifest_invalid",
                   "requires.sdk_abi must be a nonnegative integer");
        }
        result.requirements.sdk_abi = abi->get<int>();
    }
    if (const auto capabilities = requirements->find("capabilities");
        capabilities != requirements->end()) {
        if (!capabilities->is_array()) {
            reject("package_manifest_invalid",
                   "requires.capabilities must be an array");
        }
        for (const auto &capability : *capabilities) {
            if (!capability.is_string() ||
                capability.get_ref<const std::string &>().empty()) {
                reject("package_manifest_invalid",
                       "requires.capabilities must hold names");
            }
            result.requirements.capabilities.push_back(
                capability.get<std::string>());
        }
    }

    if (const auto dependencies = manifest.find("dependencies");
        dependencies != manifest.end()) {
        if (!dependencies->is_array()) {
            reject("package_manifest_invalid",
                   "package.json dependencies must be an array");
        }
        std::set<std::string> seen;
        for (const auto &entry : *dependencies) {
            requireObject(entry, "package.json dependencies entry");
            requireOnlyKeys(entry, {"namespace", "package_version"},
                            "package.json dependencies entry");
            PackageDependency dependency;
            dependency.name_space = requireStringKey(
                entry, "namespace", "package.json dependencies entry");
            if (!isNamespace(dependency.name_space)) {
                reject("package_namespace_invalid",
                       "dependency namespace must be dot-separated "
                       "identifiers: " +
                           dependency.name_space);
            }
            dependency.package_version = requireStringKey(
                entry, "package_version", "package.json dependencies entry");
            if (!isDottedNumericVersion(dependency.package_version, 3)) {
                reject("package_manifest_invalid",
                       "dependency package_version must be X.Y.Z: " +
                           dependency.package_version);
            }
            if (!seen.insert(dependency.name_space).second) {
                reject("package_manifest_invalid",
                       "package.json depends on the same namespace twice: " +
                           dependency.name_space);
            }
            result.dependencies.push_back(std::move(dependency));
        }
    }

    if (const auto provides = manifest.find("provides");
        provides != manifest.end()) {
        requireObject(*provides, "package.json provides");
        requireOnlyKeys(*provides, {"features", "graphs", "components", "code"},
                        "package.json provides");
        result.provides.features =
            parsePathArray(*provides, "features", "package.json provides");
        result.provides.graphs =
            parsePathArray(*provides, "graphs", "package.json provides");
        result.provides.components =
            parsePathArray(*provides, "components", "package.json provides");
        if (const auto code = provides->find("code"); code != provides->end()) {
            if (!code->is_boolean()) {
                reject("package_manifest_invalid",
                       "provides.code must be a boolean");
            }
            result.provides.code = code->get<bool>();
        }
    }
    if (result.provides.code && !result.requirements.sdk_abi) {
        reject("package_manifest_invalid",
               "a package that provides code must declare requires.sdk_abi");
    }
    return result;
}

PackageManifest readPackageManifest(const std::filesystem::path &package_dir) {
    std::error_code error;
    if (!std::filesystem::is_directory(package_dir, error) || error) {
        reject("package_manifest_missing",
               "the package source is not a directory: " +
                   package_dir.string());
    }
    const auto manifest_path = package_dir / packageManifestFile;
    if (!std::filesystem::is_regular_file(manifest_path, error) || error) {
        reject("package_manifest_missing",
               "no package.json in " + package_dir.string());
    }
    const auto bytes = readBytes(manifest_path, "package_manifest_missing");
    Json manifest;
    try {
        manifest = Json::parse(bytes);
    } catch (const Json::exception &parse_error) {
        reject("package_manifest_invalid",
               std::string{"package.json is not valid JSON: "} +
                   parse_error.what());
    }
    return parsePackageManifestJson(manifest);
}

std::string packageTreeDigest(const std::filesystem::path &package_dir) {
    picosha2::hash256_one_by_one hasher;
    hasher.init();
    const auto append = [&hasher](std::string_view text) {
        hasher.process(text.begin(), text.end());
    };
    for (const auto &relative : sortedRelativeFiles(package_dir)) {
        const auto path = relative.generic_string();
        const auto bytes =
            readBytes(package_dir / relative, "package_manifest_invalid");
        append(path);
        append("\n");
        append(std::to_string(bytes.size()));
        append("\n");
        append(bytes);
        append("\n");
    }
    hasher.finish();
    return picosha2::get_hash_hex_string(hasher);
}

std::vector<InstalledPackage> readInstalledPackagesJson(const Json &project) {
    std::vector<InstalledPackage> packages;
    if (!project.is_object()) return packages;
    const auto entries = project.find(packageProjectKey);
    if (entries == project.end() || !entries->is_array()) return packages;
    for (const auto &entry : *entries) {
        if (!entry.is_object()) continue;
        InstalledPackage installed;
        installed.name_space = entry.value("namespace", std::string{});
        installed.package_version =
            entry.value("package_version", std::string{});
        installed.sha256 = entry.value("sha256", std::string{});
        if (installed.name_space.empty()) continue;
        packages.push_back(std::move(installed));
    }
    return packages;
}

std::vector<InstalledPackage>
readInstalledPackages(const std::filesystem::path &project_root) {
    const auto path = projectManifestPath(project_root);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error) {
        reject("package_manifest_invalid",
               "no project.json in " + project_root.string());
    }
    const auto bytes = readBytes(path, "package_manifest_invalid");
    Json project;
    try {
        project = Json::parse(bytes);
    } catch (const Json::exception &parse_error) {
        reject("package_manifest_invalid",
               std::string{"project.json is not valid JSON: "} +
                   parse_error.what());
    }
    return readInstalledPackagesJson(project);
}

std::vector<PackageFeatureDocument>
readPackageFeatureDocuments(const std::filesystem::path &project_root,
                            std::string_view name_space) {
    const auto package_dir =
        project_root / packageVendorDirectory / std::string{name_space};
    const auto manifest = readPackageManifest(package_dir);
    const auto prefix = packageReferencePrefix(name_space);
    std::vector<PackageFeatureDocument> documents;
    for (const auto &relative : manifest.provides.features) {
        PackageFeatureDocument document;
        document.reference = prefix + relative;
        try {
            document.bytes = readBytes(package_dir / relative,
                                       "package_feature_invalid");
            const auto parsed = Json::parse(document.bytes);
            if (!parsed.is_object() ||
                parsed.value("schema", std::string{}) !=
                    std::string{render_feature_schema}) {
                throw PackageError(
                    "package_feature_invalid",
                    "not a pelican.render_feature document: " + relative);
            }
            const auto name = parsed.find("name");
            if (name == parsed.end() || !name->is_string() ||
                name->get_ref<const std::string &>().empty()) {
                throw PackageError("package_feature_invalid",
                                   "the document has no name: " + relative);
            }
            document.name = name->get<std::string>();
            const auto expected_prefix = std::string{name_space} + ".";
            if (!document.name.starts_with(expected_prefix)) {
                throw PackageError(
                    "package_feature_name_unqualified",
                    "the installed document name does not carry its "
                    "namespace: " +
                        document.name);
            }
        } catch (const std::exception &error) {
            document.error = error.what();
            if (document.name.empty()) {
                document.name = std::string{name_space} + "." + relative;
            }
        }
        documents.push_back(std::move(document));
    }
    return documents;
}

PackageInstallResult installPackage(const std::filesystem::path &source_dir,
                                    const std::filesystem::path &project_root,
                                    const PackageInstallOptions &options) {
    std::error_code error;
    if (!std::filesystem::is_directory(project_root, error) || error) {
        reject("package_manifest_invalid",
               "the project is not a directory: " + project_root.string());
    }
    // Every check runs on the source, before a byte is copied.
    const auto manifest = readPackageManifest(source_dir);

    if (compareProjectVersions(manifest.requirements.engine_min_version,
                               options.engine_version) > 0) {
        reject("package_engine_too_new",
               "package " + manifest.name_space + " requires engine " +
                   manifest.requirements.engine_min_version +
                   ", which is newer than this engine (" +
                   options.engine_version + ")");
    }
    if (manifest.requirements.sdk_abi) {
        const auto expected =
            options.sdk_abi ? *options.sdk_abi : packageSdkAbiVersion();
        if (*manifest.requirements.sdk_abi != expected) {
            reject("package_sdk_abi_mismatch",
                   "package " + manifest.name_space + " declares SDK ABI " +
                       std::to_string(*manifest.requirements.sdk_abi) +
                       "; this engine loads game code with ABI " +
                       std::to_string(expected));
        }
    }
    // PG-1: `requires.capabilities` speaks the project's `requires` vocabulary.
    // Every name is checked before the first unavailable one is reported, the
    // same order a project's `requires` is read in.
    const auto known = knownProjectRequirements();
    for (const auto &capability : manifest.requirements.capabilities) {
        if (std::find(known.begin(), known.end(), capability) == known.end()) {
            std::string vocabulary;
            for (const auto &name : known) {
                if (!vocabulary.empty()) vocabulary += ", ";
                vocabulary += name;
            }
            reject("package.capability_unknown@1",
                   "package " + manifest.name_space + " requires capability '" +
                       capability + "', which this engine does not know; known "
                       "capabilities: " + vocabulary,
                   {capability});
        }
    }
    for (const auto &capability : manifest.requirements.capabilities) {
        if (const auto code = projectRequirementUnavailableCode(capability);
            !code.empty()) {
            reject(std::string{code},
                   "package " + manifest.name_space + " requires capability '" +
                       capability + "', but this build has " +
                       std::string{projectRequirementOption(capability)} + "=OFF",
                   {capability});
        }
    }

    const auto installed = readInstalledPackages(project_root);
    const auto destination =
        project_root / packageVendorDirectory / manifest.name_space;
    for (const auto &entry : installed) {
        if (entry.name_space == manifest.name_space) {
            reject("package_already_installed",
                   "package " + manifest.name_space +
                       " is already installed at version " +
                       entry.package_version);
        }
    }
    if (std::filesystem::exists(destination, error)) {
        reject("package_already_installed",
               "the package directory already exists: " + destination.string());
    }

    std::vector<std::string> missing;
    for (const auto &dependency : manifest.dependencies) {
        const auto found = std::ranges::find_if(
            installed, [&](const InstalledPackage &entry) {
                return entry.name_space == dependency.name_space;
            });
        if (found == installed.end()) {
            missing.push_back(dependency.name_space + " " +
                              dependency.package_version);
        }
    }
    if (!missing.empty()) {
        std::string names;
        for (const auto &name : missing) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        reject("package_dependency_missing",
               "package " + manifest.name_space +
                   " needs packages this project does not have: " + names,
               missing);
    }

    // The catalog enumerator throws on a document with no name and on a
    // duplicate name.  A package whose documents would make it throw is
    // refused here, so a broken package never reaches the set the editor
    // service builds at construction.
    std::vector<std::string> feature_names;
    std::set<std::string> seen_names;
    for (const auto &relative : manifest.provides.features) {
        const auto path = source_dir / relative;
        if (!std::filesystem::is_regular_file(path, error) || error) {
            reject("package_feature_invalid",
                   "package feature is missing: " + relative);
        }
        const auto document =
            parseFeatureDocument(readBytes(path, "package_feature_invalid"),
                                 relative);
        auto qualified = qualifiedPackageFeatureName(
            manifest.name_space, document.at("name").get<std::string>());
        if (!seen_names.insert(qualified).second) {
            reject("package_feature_invalid",
                   "package " + manifest.name_space +
                       " provides the same feature name twice: " + qualified);
        }
        feature_names.push_back(std::move(qualified));
    }

    const auto staging =
        destination.parent_path() / temporaryName(manifest.name_space);
    std::filesystem::remove_all(staging, error);
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        reject("package_write_failed",
               "could not create " + destination.parent_path().string());
    }

    PackageInstallResult result;
    result.manifest = manifest;
    result.destination = destination;
    result.feature_names = feature_names;
    try {
        copyPackageTree(source_dir, staging);
        // The namespace lives in the document, not only in the catalog: the
        // compose dedupe, `requires` matching and the `hdr` comparison all
        // read this field.
        for (std::size_t index = 0;
             index < manifest.provides.features.size(); ++index) {
            const auto &relative = manifest.provides.features[index];
            const auto path = staging / relative;
            auto document = nlohmann::ordered_json::parse(
                readBytes(path, "package_feature_invalid"));
            document["name"] = feature_names[index];
            writeBytes(path, document.dump(2) + "\n");
            result.feature_references.push_back(
                packageReferencePrefix(manifest.name_space) + relative);
        }
        const auto staged_digest = packageTreeDigest(staging);
        std::filesystem::rename(staging, destination, error);
        if (error) {
            reject("package_write_failed",
                   "could not publish the package directory: " +
                       destination.string() + ": " + error.message());
        }
        result.sha256 = packageTreeDigest(destination);
        if (result.sha256 != staged_digest) {
            reject("package_digest_mismatch",
                   "the installed package does not match the bytes that were "
                   "staged: " +
                       destination.string());
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }

    // project.json records what is installed.  The write is a full-document
    // rewrite of an ordered parse, so every other key keeps its place.
    const auto manifest_path = projectManifestPath(project_root);
    const auto project_bytes =
        readBytes(manifest_path, "package_manifest_invalid");
    nlohmann::ordered_json project;
    try {
        project = nlohmann::ordered_json::parse(project_bytes);
    } catch (const Json::exception &parse_error) {
        reject("package_manifest_invalid",
               std::string{"project.json is not valid JSON: "} +
                   parse_error.what());
    }
    std::vector<nlohmann::ordered_json> entries;
    if (project.contains(std::string{packageProjectKey}) &&
        project.at(std::string{packageProjectKey}).is_array()) {
        for (const auto &entry : project.at(std::string{packageProjectKey})) {
            entries.push_back(entry);
        }
    }
    entries.push_back(nlohmann::ordered_json{
        {"namespace", manifest.name_space},
        {"package_version", manifest.package_version},
        {"sha256", result.sha256},
    });
    std::ranges::sort(entries, [](const nlohmann::ordered_json &lhs,
                                  const nlohmann::ordered_json &rhs) {
        return lhs.value("namespace", std::string{}) <
               rhs.value("namespace", std::string{});
    });
    auto recorded = nlohmann::ordered_json::array();
    for (auto &entry : entries) recorded.push_back(std::move(entry));
    project[std::string{packageProjectKey}] = std::move(recorded);
    const auto staged_manifest =
        manifest_path.parent_path() / temporaryName("project.json");
    writeBytes(staged_manifest, project.dump(2) + "\n");
    std::filesystem::rename(staged_manifest, manifest_path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(staged_manifest, ignored);
        reject("package_write_failed",
               "could not record the package in project.json: " +
                   error.message());
    }
    return result;
}

void uninstallPackage(const std::filesystem::path &project_root,
                      std::string_view name_space,
                      const std::vector<std::string> &in_use) {
    if (!isNamespace(name_space)) {
        reject("package_namespace_invalid",
               "package namespace must be dot-separated identifiers: " +
                   std::string{name_space});
    }
    const auto installed = readInstalledPackages(project_root);
    const auto found =
        std::ranges::find_if(installed, [&](const InstalledPackage &entry) {
            return entry.name_space == name_space;
        });
    const auto destination =
        project_root / packageVendorDirectory / std::string{name_space};
    std::error_code error;
    if (found == installed.end() &&
        !std::filesystem::exists(destination, error)) {
        reject("package_not_installed",
               "this project has no package " + std::string{name_space});
    }
    if (!in_use.empty()) {
        std::string names;
        for (const auto &reference : in_use) {
            if (!names.empty()) names += ", ";
            names += reference;
        }
        reject("package_in_use",
               "package " + std::string{name_space} +
                   " is still referenced by: " + names,
               in_use);
    }

    std::filesystem::remove_all(destination, error);
    if (error) {
        reject("package_write_failed",
               "could not remove " + destination.string() + ": " +
                   error.message());
    }

    const auto manifest_path = projectManifestPath(project_root);
    const auto project_bytes =
        readBytes(manifest_path, "package_manifest_invalid");
    nlohmann::ordered_json project;
    try {
        project = nlohmann::ordered_json::parse(project_bytes);
    } catch (const Json::exception &parse_error) {
        reject("package_manifest_invalid",
               std::string{"project.json is not valid JSON: "} +
                   parse_error.what());
    }
    if (project.contains(std::string{packageProjectKey}) &&
        project.at(std::string{packageProjectKey}).is_array()) {
        auto entries = nlohmann::ordered_json::array();
        for (const auto &entry :
             project.at(std::string{packageProjectKey})) {
            if (entry.is_object() &&
                entry.value("namespace", std::string{}) == name_space) {
                continue;
            }
            entries.push_back(entry);
        }
        project[std::string{packageProjectKey}] = std::move(entries);
    }
    const auto staged_manifest =
        manifest_path.parent_path() / temporaryName("project.json");
    writeBytes(staged_manifest, project.dump(2) + "\n");
    std::filesystem::rename(staged_manifest, manifest_path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(staged_manifest, ignored);
        reject("package_write_failed",
               "could not update project.json: " + error.message());
    }
}

} // namespace Pelican
