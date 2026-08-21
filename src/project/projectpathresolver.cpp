#include "projectpathresolver.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view engine_scheme = "engine://";
constexpr std::string_view project_scheme = "project://";
constexpr std::string_view user_scheme = "user://";

bool startsWithSlashRoot(std::string_view ref) {
    return ref.starts_with("\\\\") || ref.starts_with("//");
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string unsupportedScheme(std::string_view ref) {
    const auto pos = ref.find("://");
    if (pos == std::string_view::npos) {
        return {};
    }
    return std::string{ref.substr(0, pos)};
}

std::string pathString(const std::filesystem::path &path) {
    return path.string();
}

std::string readBinaryFile(const std::filesystem::path &path) {
    const auto display_path = pathString(path);
    std::ifstream file{path, std::ios_base::binary | std::ios_base::ate};
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + display_path);
    }

    const auto size = file.tellg();
    if (size < 0) {
        throw std::runtime_error("Failed to get file size: " + display_path);
    }

    const auto file_size = static_cast<std::streamsize>(size);
    std::string data(static_cast<std::size_t>(file_size), '\0');
    file.seekg(0);
    file.read(data.data(), file_size);
    if (!file && file_size > 0) {
        throw std::runtime_error("Failed to read file: " + display_path);
    }
    return data;
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

std::filesystem::path canonicalDirectoryOrThrow(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        throw std::runtime_error("PathResolver project root does not exist: " + pathString(path));
    }
    if (!std::filesystem::is_directory(path, ec) || ec) {
        throw std::runtime_error("PathResolver project root is not a directory: " + pathString(path));
    }

    const auto canonical = std::filesystem::canonical(path, ec);
    if (ec) {
        throw std::runtime_error("PathResolver failed to canonicalize project root: " + pathString(path) +
                                 " (" + ec.message() + ")");
    }
    return canonical;
}

using PathString = std::filesystem::path::string_type;

PathString comparableComponent(const std::filesystem::path &component) {
    auto value = component.native();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](std::filesystem::path::value_type ch) {
        if constexpr (std::is_same_v<std::filesystem::path::value_type, wchar_t>) {
            return static_cast<std::filesystem::path::value_type>(std::towlower(ch));
        } else {
            return static_cast<std::filesystem::path::value_type>(
                std::tolower(static_cast<unsigned char>(ch)));
        }
    });
#endif
    return value;
}

std::vector<PathString> pathComponents(const std::filesystem::path &path) {
    std::vector<PathString> components;
    for (const auto &component : path) {
        components.push_back(comparableComponent(component));
    }
    return components;
}

bool isWithinRoot(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    const auto root_components = pathComponents(root);
    const auto candidate_components = pathComponents(candidate);
    if (candidate_components.size() < root_components.size()) {
        return false;
    }
    for (size_t i = 0; i < root_components.size(); ++i) {
        if (root_components[i] != candidate_components[i]) {
            return false;
        }
    }
    return true;
}

struct RefPathComponents {
    std::vector<std::filesystem::path> raw;
    std::vector<PathString> comparable;
};

RefPathComponents refPathComponents(const std::filesystem::path &path) {
    RefPathComponents result;
    for (const auto &component : path) {
        if (component.empty() || component == ".") {
            continue;
        }
        result.raw.push_back(component);
        result.comparable.push_back(comparableComponent(component));
    }
    return result;
}

std::vector<PathString> normalizedRelativeComponents(const std::filesystem::path &path) {
    return refPathComponents(path.lexically_normal()).comparable;
}

bool startsWithComponents(const std::vector<PathString> &value, const std::vector<PathString> &prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (value[i] != prefix[i]) {
            return false;
        }
    }
    return true;
}

bool componentsOverlap(const std::vector<PathString> &lhs, const std::vector<PathString> &rhs) {
    return startsWithComponents(lhs, rhs) || startsWithComponents(rhs, lhs);
}

std::filesystem::path tailPath(const RefPathComponents &components, size_t prefix_size) {
    std::filesystem::path result;
    for (size_t i = prefix_size; i < components.raw.size(); ++i) {
        result /= components.raw[i];
    }
    return result;
}

bool hasAbsoluteSyntax(const std::filesystem::path &path, std::string_view ref) {
    return path.is_absolute() || path.has_root_name() || startsWithSlashRoot(ref);
}

std::string stripProjectScheme(std::string_view ref) {
    if (startsWith(ref, project_scheme)) {
        return std::string{ref.substr(project_scheme.size())};
    }
    return std::string{ref};
}

std::string refForMessage(std::string_view ref) {
    return ref.empty() ? std::string{"<empty>"} : std::string{ref};
}

void validateRefSyntax(std::string_view ref) {
    if (ref.empty()) {
        throw std::runtime_error("Empty project path references are not allowed");
    }
    if (ref.find('\\') != std::string_view::npos) {
        throw std::runtime_error("Backslash path separators are not allowed in project references: " +
                                 refForMessage(ref));
    }
}

bool isFragmentKindChar(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

bool isAllAsciiDigits(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char ch) {
               return std::isdigit(static_cast<unsigned char>(ch)) != 0;
           });
}

std::string unsupportedFragmentMessage(const AssetFragmentRef &fragment) {
    return "Unsupported asset fragment kind: " + fragment.kind;
}

ResolvedRef attachFragment(ResolvedRef resolved, const AssetFragmentRef &fragment) {
    if (auto *path = std::get_if<std::filesystem::path>(&resolved)) {
        return ResolvedPathFragment{*path, fragment};
    }
    if (auto *engine = std::get_if<EngineResourceId>(&resolved)) {
        return ResolvedEngineFragment{*engine, fragment};
    }
    return resolved;
}

PathResolutionResult resolvedReference(ResolvedRef resolved) {
    return PathResolutionResult{.reference = std::move(resolved)};
}

std::filesystem::path absolutePath(const std::filesystem::path &path) {
    if (path.is_absolute()) {
        return path;
    }
    return std::filesystem::current_path() / path;
}

std::filesystem::path defaultUserRoot(const std::string &project_name) {
#ifdef _WIN32
    const char *appdata = std::getenv("APPDATA");
    if (appdata == nullptr || std::string_view{appdata}.empty()) {
        throw std::runtime_error("APPDATA is required to resolve user:// references");
    }
    return std::filesystem::path{appdata} / "pelican" / project_name;
#else
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && std::string_view{xdg}.size() > 0) {
        return std::filesystem::path{xdg} / "pelican" / project_name;
    }
    const char *home = std::getenv("HOME");
    if (home == nullptr || std::string_view{home}.empty()) {
        throw std::runtime_error("HOME is required to resolve user:// references");
    }
    return std::filesystem::path{home} / ".local" / "share" / "pelican" / project_name;
#endif
}

std::string normalizedGenericRelativeString(const std::filesystem::path &path) {
    auto normalized = path.lexically_normal().generic_string();
    if (normalized == ".") {
        normalized.clear();
    }
    return normalized;
}

void rejectUnexpectedLocalKeys(const nlohmann::json &local) {
    if (!local.is_object()) {
        throw std::runtime_error(".pelican/local.json must be an object");
    }
    for (const auto &[key, value] : local.items()) {
        (void)value;
        if (key != "asset_stores") {
            throw std::runtime_error(".pelican/local.json contains unsupported key: " + key);
        }
    }
}

} // namespace

ParsedPathRef parsePathReference(std::string_view ref) {
    const auto marker = ref.find('#');
    if (marker == std::string_view::npos) {
        return ParsedPathRef{std::string{ref}, std::nullopt};
    }
    if (ref.find('#', marker + 1) != std::string_view::npos) {
        throw std::runtime_error("Ambiguous asset fragment reference contains multiple '#': " +
                                 refForMessage(ref));
    }

    const auto fragment_text = ref.substr(marker + 1);
    const auto separator = fragment_text.find('/');
    if (separator == std::string_view::npos || separator == 0 ||
        separator == fragment_text.size() - 1) {
        throw std::runtime_error("Invalid asset fragment reference: expected #kind/path in " +
                                 refForMessage(ref));
    }

    const auto kind = fragment_text.substr(0, separator);
    if (!std::all_of(kind.begin(), kind.end(), isFragmentKindChar)) {
        throw std::runtime_error("Invalid asset fragment kind in reference: " + refForMessage(ref));
    }

    const auto path = fragment_text.substr(separator + 1);
    if (path.find('\\') != std::string_view::npos || path.find("//") != std::string_view::npos ||
        path.starts_with('/') || path.ends_with('/') || isAllAsciiDigits(path)) {
        throw std::runtime_error("Invalid asset fragment path in reference: " + refForMessage(ref));
    }

    AssetFragmentRef fragment{
        .kind = std::string{kind},
        .path = std::string{path},
        .address_kind = path.find('/') == std::string_view::npos ? AssetFragmentAddressKind::name
                                                                  : AssetFragmentAddressKind::full_path,
    };
    return ParsedPathRef{std::string{ref.substr(0, marker)}, std::move(fragment)};
}

std::string normalizedResolvedReferenceKey(const ResolvedRef &reference) {
    const auto path_key = [](const std::filesystem::path &path) {
        const auto generic = path.generic_u8string();
        return std::string{"file:"} +
               std::string{reinterpret_cast<const char *>(generic.data()),
                           generic.size()};
    };
    return std::visit(
        [&](const auto &resolved) -> std::string {
            using T = std::decay_t<decltype(resolved)>;
            if constexpr (std::is_same_v<T, std::filesystem::path>) {
                return path_key(resolved);
            } else if constexpr (std::is_same_v<T, EngineResourceId>) {
                return "engine://" + resolved.id;
            } else if constexpr (std::is_same_v<T, ResolvedPathFragment>) {
                return path_key(resolved.path) + "#" +
                       resolved.fragment.kind + "/" +
                       resolved.fragment.path;
            } else {
                return "engine://" + resolved.resource.id + "#" +
                       resolved.fragment.kind + "/" +
                       resolved.fragment.path;
            }
        },
        reference);
}

void ProjectPathResolver::setup(const std::filesystem::path &project_root,
                                bool allow_absolute) {
    setupImpl(project_root, allow_absolute, nullptr, std::nullopt);
}

void ProjectPathResolver::setup(
    const std::filesystem::path &project_root, bool allow_absolute,
    const ProjectEnvelope &project,
    std::optional<std::filesystem::path> user_dir_override) {
    setupImpl(project_root, allow_absolute, &project,
              std::move(user_dir_override));
}

void ProjectPathResolver::setupImpl(
    const std::filesystem::path &project_root, bool allow_absolute,
    const ProjectEnvelope *project,
    std::optional<std::filesystem::path> user_dir_override) {
    if (configured) {
        throw std::runtime_error("PathResolver setup called more than once");
    }

    project_root_abs = canonicalDirectoryOrThrow(project_root);
    allow_absolute_paths = allow_absolute;
    asset_stores.clear();
    project_id.reset();
    user_root_abs.reset();

    if (project != nullptr) {
        project_id = project->name;
        if (project_id) {
            const auto root = user_dir_override ? absolutePath(*user_dir_override) : defaultUserRoot(*project_id);
            user_root_abs = weaklyCanonicalOrThrow(root, "PathResolver user root");
        } else if (user_dir_override) {
            throw std::runtime_error("--user-dir requires project.json name for user:// resolution");
        }
    }

    if (project != nullptr) {
        const auto &declared = project->asset_stores;
        if (!declared.is_object()) {
            throw std::runtime_error("project.json asset_stores must be an object");
        }

        std::set<std::string> declared_names;
        std::vector<AssetStoreMount> parsed_stores;
        for (const auto &[name, store] : declared.items()) {
            if (name.empty()) {
                throw std::runtime_error("project.json asset_stores store names must not be empty");
            }
            if (!declared_names.insert(name).second) {
                throw std::runtime_error("duplicate asset store declaration: " + name);
            }
            if (!store.is_object()) {
                throw std::runtime_error("project.json asset_stores." + name + " must be an object");
            }
            if (!store.contains("mount") || !store.at("mount").is_string()) {
                throw std::runtime_error("project.json asset_stores." + name + ".mount must be a string");
            }

            const auto mount = store.at("mount").get<std::string>();
            validateRefSyntax(mount);
            const std::filesystem::path mount_path{mount};
            if (hasAbsoluteSyntax(mount_path, mount)) {
                throw std::runtime_error("asset store mount must be relative: " + name);
            }
            const auto normalized_mount = mount_path.lexically_normal();
            const auto mount_components = normalizedRelativeComponents(normalized_mount);
            if (mount_components.empty()) {
                throw std::runtime_error("asset store mount must not resolve to project root: " + name);
            }

            for (const auto &existing : parsed_stores) {
                if (componentsOverlap(mount_components, normalizedRelativeComponents(existing.logical_mount))) {
                    throw std::runtime_error("asset store mounts overlap: " + existing.name + " and " +
                                             name);
                }
            }

            std::optional<std::filesystem::path> manifest_abs;
            std::optional<std::string> manifest_error;
            if (const auto manifest = store.find("manifest"); manifest != store.end()) {
                try {
                    if (!manifest->is_string()) {
                        throw std::runtime_error("project.json asset_stores." + name +
                                                 ".manifest must be a string");
                    }
                    const auto manifest_ref = manifest->get<std::string>();
                    validateRefSyntax(manifest_ref);
                    const std::filesystem::path manifest_path{manifest_ref};
                    if (hasAbsoluteSyntax(manifest_path, manifest_ref)) {
                        throw std::runtime_error("asset store manifest must be project-relative: " + name);
                    }
                    manifest_abs = weaklyCanonicalOrThrow(project_root_abs / manifest_path,
                                                          "PathResolver asset store manifest " + name);
                    if (*manifest_abs == project_root_abs || !isWithinRoot(project_root_abs, *manifest_abs)) {
                        throw std::runtime_error("asset store manifest escapes project root: " + name);
                    }
                } catch (const std::exception &err) {
                    manifest_abs.reset();
                    manifest_error = err.what();
                }
            }

            parsed_stores.push_back(AssetStoreMount{
                .name = name,
                .mount = normalizedGenericRelativeString(normalized_mount),
                .logical_mount = normalized_mount,
                .root_abs = weaklyCanonicalOrThrow(project_root_abs / normalized_mount,
                                                   "PathResolver asset store " + name),
                .manifest_abs = std::move(manifest_abs),
                .manifest_error = std::move(manifest_error),
            });
        }

        const auto local_path = project_root_abs / ".pelican" / "local.json";
        std::error_code ec;
        if (std::filesystem::is_regular_file(local_path, ec) && !ec) {
            const auto local = nlohmann::json::parse(readBinaryFile(local_path));
            rejectUnexpectedLocalKeys(local);
            if (local.contains("asset_stores")) {
                const auto &overrides = local.at("asset_stores");
                if (!overrides.is_object()) {
                    throw std::runtime_error(".pelican/local.json asset_stores must be an object");
                }
                for (const auto &[name, value] : overrides.items()) {
                    const auto match = std::find_if(parsed_stores.begin(), parsed_stores.end(),
                                                    [&](const AssetStoreMount &store) {
                                                        return store.name == name;
                                                    });
                    if (match == parsed_stores.end()) {
                        throw std::runtime_error(".pelican/local.json references undeclared asset store: " +
                                                 name);
                    }
                    if (!value.is_string()) {
                        throw std::runtime_error(".pelican/local.json asset_stores." + name +
                                                 " must be a path string");
                    }
                    auto override_path = std::filesystem::path{value.get<std::string>()};
                    if (override_path.is_relative()) {
                        override_path = project_root_abs / override_path;
                    }
                    match->root_abs = weaklyCanonicalOrThrow(override_path,
                                                             "PathResolver asset store override " + name);
                }
            }
        }

        for (size_t i = 0; i < parsed_stores.size(); ++i) {
            for (size_t j = i + 1; j < parsed_stores.size(); ++j) {
                if (isWithinRoot(parsed_stores[i].root_abs, parsed_stores[j].root_abs) ||
                    isWithinRoot(parsed_stores[j].root_abs, parsed_stores[i].root_abs)) {
                    throw std::runtime_error("asset store roots overlap: " + parsed_stores[i].name +
                                             " and " + parsed_stores[j].name);
                }
            }
        }

        asset_stores = std::move(parsed_stores);
    } else {
        const auto local_path = project_root_abs / ".pelican" / "local.json";
        std::error_code ec;
        if (std::filesystem::is_regular_file(local_path, ec) && !ec) {
            const auto local = nlohmann::json::parse(readBinaryFile(local_path));
            rejectUnexpectedLocalKeys(local);
            if (local.contains("asset_stores") && !local.at("asset_stores").empty()) {
                throw std::runtime_error(".pelican/local.json asset_stores requires project.json asset_stores declarations");
            }
        }
    }

    configured = true;
}

void ProjectPathResolver::reset() {
    configured = false;
    project_root_abs.clear();
    user_root_abs.reset();
    project_id.reset();
    allow_absolute_paths = false;
    asset_stores.clear();
}

std::vector<AssetStoreStatus> ProjectPathResolver::stores() const {
    std::vector<AssetStoreStatus> result;
    result.reserve(asset_stores.size());
    for (const auto &store : asset_stores) {
        result.push_back(AssetStoreStatus{
            .name = store.name,
            .mount = store.mount,
            .root = store.root_abs,
            .manifest = store.manifest_abs,
            .manifest_error = store.manifest_error,
        });
    }
    return result;
}

PathResolutionResult
ProjectPathResolver::resolveProjectRef(std::string_view ref) const {
    return resolveRef(ref, false);
}

PathResolutionResult
ProjectPathResolver::resolveCliRef(std::string_view ref) const {
    return resolveRef(ref, true);
}

PathResolutionResult
ProjectPathResolver::resolveRef(std::string_view ref, bool cli_origin) const {
    validateRefSyntax(ref);
    const auto parsed = parsePathReference(ref);
    validateRefSyntax(parsed.path);

    if (startsWith(parsed.path, engine_scheme)) {
        ResolvedRef resolved = EngineResourceId{std::string{parsed.path.substr(engine_scheme.size())}};
        if (parsed.fragment) {
            resolved = attachFragment(std::move(resolved), *parsed.fragment);
        }
        return resolvedReference(std::move(resolved));
    }

    if (const auto scheme = unsupportedScheme(parsed.path);
        !scheme.empty() && scheme != "project" && scheme != "user") {
        throw std::runtime_error("Unsupported path scheme in project reference: " + scheme);
    }

    if (startsWith(parsed.path, user_scheme)) {
        if (!configured) {
            throw std::runtime_error("PathResolver setup must be called before resolving user:// paths");
        }
        if (!user_root_abs) {
            throw std::runtime_error("user:// references require project.json name");
        }

        const auto user_ref = std::string{parsed.path.substr(user_scheme.size())};
        if (!user_ref.empty()) {
            validateRefSyntax(user_ref);
        }
        const std::filesystem::path user_path{user_ref};
        if (hasAbsoluteSyntax(user_path, user_ref)) {
            throw std::runtime_error("Absolute user:// path references are not allowed: " +
                                     refForMessage(ref));
        }

        auto resolved_path = weaklyCanonicalOrThrow(*user_root_abs / user_path,
                                                    "PathResolver user reference");
        if (!isWithinRoot(*user_root_abs, resolved_path)) {
            throw std::runtime_error("user:// path escapes user root: " + refForMessage(ref) +
                                     " resolved to " + pathString(resolved_path) +
                                     " outside " + pathString(*user_root_abs));
        }

        ResolvedRef resolved = resolved_path;
        if (parsed.fragment) {
            resolved = attachFragment(std::move(resolved), *parsed.fragment);
        }
        return resolvedReference(std::move(resolved));
    }

    const auto ref_string = stripProjectScheme(parsed.path);
    validateRefSyntax(ref_string);
    const std::filesystem::path ref_path{ref_string};
    if (hasAbsoluteSyntax(ref_path, ref_string)) {
        if (!cli_origin) {
            throw std::runtime_error("Absolute project path references are not allowed: " +
                                     refForMessage(ref));
        }
        if (!allow_absolute_paths) {
            throw std::runtime_error("Absolute CLI path references require --allow-absolute-paths: " +
                                     refForMessage(ref));
        }
        if (!ref_path.is_absolute() && ref_path.has_root_name()) {
            throw std::runtime_error("Drive-relative paths are not supported: " + refForMessage(ref));
        }

        const auto canonical = weaklyCanonicalOrThrow(ref_path, "PathResolver CLI reference");
        ResolvedRef resolved = canonical;
        if (parsed.fragment) {
            resolved = attachFragment(std::move(resolved), *parsed.fragment);
        }
        auto result = resolvedReference(std::move(resolved));
        result.warnings.push_back(
            "absolute CLI path reference accepted: " + pathString(canonical));
        return result;
    }

    if (!configured) {
        throw std::runtime_error("PathResolver setup must be called before resolving project paths");
    }

    const auto ref_components = refPathComponents(ref_path);
    for (const auto &store : asset_stores) {
        const auto mount_components = normalizedRelativeComponents(store.logical_mount);
        if (!startsWithComponents(ref_components.comparable, mount_components)) {
            continue;
        }

        const auto relative_inside_store = tailPath(ref_components, mount_components.size());
        const auto canonical = weaklyCanonicalOrThrow(store.root_abs / relative_inside_store,
                                                      "PathResolver asset store reference");
        if (!isWithinRoot(store.root_abs, canonical)) {
            throw std::runtime_error("Asset store path escapes mount root '" + store.name + "': " +
                                     refForMessage(ref) + " resolved to " + pathString(canonical) +
                                     " outside " + pathString(store.root_abs));
        }

        ResolvedRef resolved = canonical;
        if (parsed.fragment) {
            resolved = attachFragment(std::move(resolved), *parsed.fragment);
        }
        return resolvedReference(std::move(resolved));
    }

    const auto joined = project_root_abs / ref_path;
    const auto canonical = weaklyCanonicalOrThrow(joined, "PathResolver project reference");
    if (!isWithinRoot(project_root_abs, canonical)) {
        throw std::runtime_error("Project path escapes project root: " + refForMessage(ref) +
                                 " resolved to " + pathString(canonical) +
                                 " outside " + pathString(project_root_abs));
    }
    ResolvedRef resolved = canonical;
    if (parsed.fragment) {
        resolved = attachFragment(std::move(resolved), *parsed.fragment);
    }
    return resolvedReference(std::move(resolved));
}

std::filesystem::path
ProjectPathResolver::resolveExistingFile(std::string_view ref) const {
    const auto resolved = resolveExistingFileReference(ref);
    if (const auto fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
        throw std::runtime_error(unsupportedFragmentMessage(fragment->fragment));
    }

    return std::get<std::filesystem::path>(resolved);
}

std::string ProjectPathResolver::normalizedReference(
    std::string_view ref) const {
    return normalizedResolvedReferenceKey(resolveProjectRef(ref).reference);
}

ResolvedRef ProjectPathResolver::resolveExistingFileReference(
    std::string_view ref) const {
    const auto resolved = resolveProjectRef(ref).reference;
    if (const auto engine_id = std::get_if<EngineResourceId>(&resolved)) {
        throw std::runtime_error("resolveExistingFile does not accept engine resources: engine://" +
                                 engine_id->id);
    }
    if (const auto fragment = std::get_if<ResolvedEngineFragment>(&resolved)) {
        throw std::runtime_error(unsupportedFragmentMessage(fragment->fragment));
    }

    const auto &path = std::holds_alternative<ResolvedPathFragment>(resolved)
                           ? std::get<ResolvedPathFragment>(resolved).path
                           : std::get<std::filesystem::path>(resolved);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        throw std::runtime_error("File not found: " + pathString(path));
    }
    return resolved;
}

std::string ProjectPathResolver::loadText(
    std::string_view ref,
    const EngineResourceTextLoader &load_engine_resource) const {
    const auto resolved = resolveProjectRef(ref).reference;
    if (const auto engine_id = std::get_if<EngineResourceId>(&resolved)) {
        if (!load_engine_resource) {
            throw std::runtime_error(
                "engine resource loader is not configured: engine://" +
                engine_id->id);
        }
        return load_engine_resource(engine_id->id);
    }
    if (const auto fragment = std::get_if<ResolvedPathFragment>(&resolved)) {
        throw std::runtime_error(unsupportedFragmentMessage(fragment->fragment));
    }
    if (const auto fragment = std::get_if<ResolvedEngineFragment>(&resolved)) {
        throw std::runtime_error(unsupportedFragmentMessage(fragment->fragment));
    }
    return readBinaryFile(std::get<std::filesystem::path>(resolved));
}

std::vector<std::byte> ProjectPathResolver::loadBytes(
    std::string_view ref,
    const EngineResourceTextLoader &load_engine_resource) const {
    const auto text = loadText(ref, load_engine_resource);
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

} // namespace Pelican
