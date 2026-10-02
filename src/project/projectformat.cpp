#include "projectformat.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

constexpr std::string_view project_schema = "pelican.project";
constexpr int supported_project_version = 1;

std::vector<int> parseVersion(std::string_view version) {
    std::vector<int> parts;
    std::string token;
    std::istringstream stream{std::string{version}};
    while (std::getline(stream, token, '.')) {
        if (token.empty()) {
            throw std::runtime_error("invalid version string: " +
                                     std::string{version});
        }
        size_t parsed_chars = 0;
        const int value = std::stoi(token, &parsed_chars, 10);
        if (parsed_chars != token.size() || value < 0) {
            throw std::runtime_error("invalid version string: " +
                                     std::string{version});
        }
        parts.push_back(value);
    }
    return parts;
}

std::optional<std::string> optionalNonEmptyString(const nlohmann::json &object,
                                                  std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }
    auto value = found->get<std::string>();
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

int compareProjectVersions(std::string_view lhs, std::string_view rhs) {
    auto lhs_parts = parseVersion(lhs);
    auto rhs_parts = parseVersion(rhs);
    const auto count = std::max(lhs_parts.size(), rhs_parts.size());
    lhs_parts.resize(count, 0);
    rhs_parts.resize(count, 0);
    for (size_t i = 0; i < count; ++i) {
        if (lhs_parts[i] < rhs_parts[i]) {
            return -1;
        }
        if (lhs_parts[i] > rhs_parts[i]) {
            return 1;
        }
    }
    return 0;
}

namespace {

// One row per build feature a project may name. `available` is decided at
// compile time by the same macro the engine's own code reads, so a project
// cannot be told a module is present by a build that left it out.
struct ProjectRequirement {
    std::string_view name;
    std::string_view option;
    // The name an OFF build already fails under, so requiring a module up
    // front reuses that name instead of inventing a third one.
    std::string_view unavailable_code;
    bool available;
};

constexpr ProjectRequirement project_requirements[] = {
#define PELICAN_PROJECT_REQUIREMENT(name, option, code) \
    {name, #option, code, static_cast<bool>(option)},
#include "projectrequirements.def"
#undef PELICAN_PROJECT_REQUIREMENT
};

const ProjectRequirement *findProjectRequirement(std::string_view name) {
    for (const auto &requirement : project_requirements) {
        if (requirement.name == name) {
            return &requirement;
        }
    }
    return nullptr;
}

} // namespace

std::vector<std::string> knownProjectRequirements() {
    std::vector<std::string> names;
    for (const auto &requirement : project_requirements) {
        names.emplace_back(requirement.name);
    }
    return names;
}

std::string_view projectRequirementOption(std::string_view requirement) {
    const auto *found = findProjectRequirement(requirement);
    return found ? found->option : std::string_view{};
}

std::string_view projectRequirementUnavailableCode(std::string_view requirement) {
    const auto *found = findProjectRequirement(requirement);
    return found != nullptr && !found->available ? found->unavailable_code
                                                 : std::string_view{};
}

std::vector<std::string>
parseProjectRequirements(const nlohmann::json &project) {
    std::vector<std::string> requirements;
    if (!project.is_object() || !project.contains("requires")) {
        return requirements;
    }
    const auto &declared = project.at("requires");
    if (!declared.is_array()) {
        throw std::runtime_error(
            "project.requires_unknown@1: project.json requires must be an "
            "array of build feature names");
    }
    for (const auto &entry : declared) {
        if (!entry.is_string() ||
            entry.get_ref<const std::string &>().empty()) {
            throw std::runtime_error(
                "project.requires_unknown@1: project.json requires must name "
                "build features as nonempty strings");
        }
        auto name = entry.get<std::string>();
        if (findProjectRequirement(name) == nullptr) {
            std::string known;
            for (const auto &requirement : project_requirements) {
                if (!known.empty()) {
                    known += ", ";
                }
                known += std::string{requirement.name};
            }
            throw std::runtime_error(
                "project.requires_unknown@1: project.json requires '" + name +
                "', which this engine does not know; known requirements: " +
                known);
        }
        requirements.push_back(std::move(name));
    }
    return requirements;
}

std::optional<std::string>
unsatisfiedProjectRequirement(const nlohmann::json &project) {
    for (const auto &name : parseProjectRequirements(project)) {
        const auto *requirement = findProjectRequirement(name);
        if (requirement != nullptr && !requirement->available) {
            return name;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>>
parseProjectHotReload(const nlohmann::json &project) {
    if (!project.is_object() || !project.contains("hot_reload")) {
        return std::nullopt;
    }
    const auto refuse = [](const std::string &message) {
        return std::runtime_error(std::string{projectHotReloadInvalidCode} +
                                  ": project.json " + message);
    };
    const auto &declared = project.at("hot_reload");
    if (!declared.is_array()) {
        throw refuse("hot_reload must be an array of project-relative paths");
    }
    std::vector<std::string> paths;
    for (std::size_t index = 0; index < declared.size(); ++index) {
        const auto entry_name = "hot_reload[" + std::to_string(index) + "]";
        const auto &entry = declared.at(index);
        if (!entry.is_string()) {
            throw refuse(entry_name + " must be a string");
        }
        const auto &raw = entry.get_ref<const std::string &>();
        const auto quoted = entry_name + " '" + raw + "'";
        const bool drive = raw.size() >= 2 && raw[1] == ':' &&
                           std::isalpha(static_cast<unsigned char>(raw[0])) != 0;
        if (!raw.empty() && (raw.front() == '/' || raw.front() == '\\' ||
                             drive || raw.find("://") != std::string::npos)) {
            throw refuse(quoted +
                         " must be a path relative to the project root");
        }
        std::string normalized;
        std::size_t start = 0;
        while (start <= raw.size()) {
            const auto end = raw.find_first_of("/\\", start);
            const auto component = raw.substr(
                start, end == std::string::npos ? std::string::npos : end - start);
            if (component == "..") {
                throw refuse(quoted + " must not contain '..'");
            }
            if (!component.empty() && component != ".") {
                if (!normalized.empty()) normalized += '/';
                normalized += component;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        if (normalized.empty()) {
            throw refuse(entry_name +
                         " is empty; name a folder or file under the "
                         "project root");
        }
        if (const auto found = std::find(paths.begin(), paths.end(), normalized);
            found != paths.end()) {
            throw refuse(quoted + " repeats hot_reload[" +
                         std::to_string(found - paths.begin()) + "] '" +
                         normalized + "'");
        }
        paths.push_back(std::move(normalized));
    }
    return paths;
}

ProjectEnvelopeParseResult
parseProjectEnvelopeJson(const nlohmann::json &project,
                         const ProjectEnvelopeParseOptions &options) {
    if (!project.is_object()) {
        throw std::runtime_error("project.json must be an object");
    }
    if (project.value("schema", std::string{}) != project_schema) {
        throw std::runtime_error("project.json schema is not supported");
    }
    if (!project.contains("version") ||
        !project.at("version").is_number_integer()) {
        throw std::runtime_error("project.json requires numeric version");
    }
    if (project.at("version").get<int>() != supported_project_version) {
        throw std::runtime_error("project.json version must be exactly 1");
    }

    ProjectEnvelopeParseResult result;
    result.envelope.name = optionalNonEmptyString(project, "name");
    result.envelope.saved_by_engine =
        optionalNonEmptyString(project, "saved_by_engine");
    // A project that names a module this build left out stops here, with the
    // name the animation module already uses when a scene needs it. Opening
    // such a project is not a graph problem to be discovered node by node.
    result.envelope.requirements = parseProjectRequirements(project);
    if (const auto unsatisfied = unsatisfiedProjectRequirement(project)) {
        const auto *requirement = findProjectRequirement(*unsatisfied);
        throw std::runtime_error(
            std::string{requirement->unavailable_code} + ": this build has " +
            std::string{requirement->option} +
            "=OFF, so project.json's requires: [\"" + *unsatisfied +
            "\"] cannot be satisfied");
    }
    result.envelope.basic_config =
        project.value("basic_config", nlohmann::json::object());
    result.envelope.asset_stores =
        project.value("asset_stores", nlohmann::json::object());
    result.envelope.hot_reload = parseProjectHotReload(project);
    result.envelope.prefabs = parsePrefabRegistryEntries(
        project.value("prefabs", nlohmann::json::array()));

    if (!project.contains("engine_min_version")) {
        return result;
    }
    if (!project.at("engine_min_version").is_string()) {
        throw std::runtime_error(
            "project.json engine_min_version must be a string");
    }

    result.envelope.engine_min_version =
        project.at("engine_min_version").get<std::string>();
    if (compareProjectVersions(*result.envelope.engine_min_version,
                        options.current_engine_version) <= 0) {
        return result;
    }

    const auto message =
        std::string{projectEngineTooOldCode} +
        ": project.json engine_min_version " +
        *result.envelope.engine_min_version +
        " is newer than this engine (" +
        std::string{options.current_engine_version} + ")";
    if (!options.ignore_engine_version) {
        throw std::runtime_error(message);
    }
    result.warnings.push_back(
        message +
        "; continuing because --ignore-engine-version was specified");
    return result;
}

ProjectEnvelopeParseResult
parseProjectEnvelopeText(std::string_view project_json,
                         const ProjectEnvelopeParseOptions &options) {
    return parseProjectEnvelopeJson(nlohmann::json::parse(project_json), options);
}

} // namespace Pelican
