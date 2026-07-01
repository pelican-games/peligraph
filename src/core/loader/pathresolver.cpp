#include "pathresolver.hpp"

#include "../log.hpp"
#include "engineresources.hpp"
#include "fileio.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cwctype>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace Pelican {

namespace {

constexpr std::string_view engine_scheme = "engine://";
constexpr std::string_view project_scheme = "project://";

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

} // namespace

void PathResolver::setup(const std::filesystem::path &project_root, bool allow_absolute) {
    if (configured) {
        throw std::runtime_error("PathResolver setup called more than once");
    }

    project_root_abs = canonicalDirectoryOrThrow(project_root);
    allow_absolute_paths = allow_absolute;
    configured = true;
}

void PathResolver::resetForTesting() {
    configured = false;
    project_root_abs.clear();
    allow_absolute_paths = false;
}

ResolvedRef PathResolver::resolveProjectRef(std::string_view ref) const {
    return resolveRef(ref, false);
}

ResolvedRef PathResolver::resolveCliRef(std::string_view ref) const {
    return resolveRef(ref, true);
}

ResolvedRef PathResolver::resolveRef(std::string_view ref, bool cli_origin) const {
    if (startsWith(ref, engine_scheme)) {
        return EngineResourceId{std::string{ref.substr(engine_scheme.size())}};
    }

    if (const auto scheme = unsupportedScheme(ref); !scheme.empty() && scheme != "project") {
        throw std::runtime_error("Unsupported path scheme in project reference: " + scheme);
    }

    const auto ref_string = stripProjectScheme(ref);
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
        LOG_WARNING(logger, "absolute CLI path reference accepted: {}", pathString(canonical));
        return canonical;
    }

    if (!configured) {
        throw std::runtime_error("PathResolver setup must be called before resolving project paths");
    }

    const auto joined = project_root_abs / ref_path;
    const auto canonical = weaklyCanonicalOrThrow(joined, "PathResolver project reference");
    if (!isWithinRoot(project_root_abs, canonical)) {
        throw std::runtime_error("Project path escapes project root: " + refForMessage(ref) +
                                 " resolved to " + pathString(canonical) +
                                 " outside " + pathString(project_root_abs));
    }
    return canonical;
}

std::filesystem::path PathResolver::resolveExistingFile(std::string_view ref) const {
    const auto resolved = resolveProjectRef(ref);
    if (const auto engine_id = std::get_if<EngineResourceId>(&resolved)) {
        throw std::runtime_error("resolveExistingFile does not accept engine resources: engine://" +
                                 engine_id->id);
    }

    const auto path = std::get<std::filesystem::path>(resolved);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        throw std::runtime_error("File not found: " + pathString(path));
    }
    return path;
}

std::string PathResolver::loadText(std::string_view ref) const {
    const auto resolved = resolveProjectRef(ref);
    if (const auto engine_id = std::get_if<EngineResourceId>(&resolved)) {
        return engineResourceOrThrow(engine_id->id);
    }
    return readBinaryFile(pathString(std::get<std::filesystem::path>(resolved)));
}

std::vector<std::byte> PathResolver::loadBytes(std::string_view ref) const {
    const auto text = loadText(ref);
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

} // namespace Pelican
