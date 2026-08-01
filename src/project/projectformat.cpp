#include "projectformat.hpp"

#include <algorithm>
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

int compareVersions(std::string_view lhs, std::string_view rhs) {
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
    result.envelope.basic_config =
        project.value("basic_config", nlohmann::json::object());
    result.envelope.asset_stores =
        project.value("asset_stores", nlohmann::json::object());

    if (!project.contains("engine_min_version")) {
        return result;
    }
    if (!project.at("engine_min_version").is_string()) {
        throw std::runtime_error(
            "project.json engine_min_version must be a string");
    }

    result.envelope.engine_min_version =
        project.at("engine_min_version").get<std::string>();
    if (compareVersions(*result.envelope.engine_min_version,
                        options.current_engine_version) <= 0) {
        return result;
    }

    const auto message =
        "project.json engine_min_version " +
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
