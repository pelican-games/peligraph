#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct ProjectEnvelope {
    std::optional<std::string> name;
    std::optional<std::string> engine_min_version;
    nlohmann::json basic_config = nlohmann::json::object();
    nlohmann::json asset_stores = nlohmann::json::object();
};

struct ProjectEnvelopeParseOptions {
    std::string_view current_engine_version = "0.1.0";
    bool ignore_engine_version = false;
};

struct ProjectEnvelopeParseResult {
    ProjectEnvelope envelope;
    std::vector<std::string> warnings;
};

ProjectEnvelopeParseResult
parseProjectEnvelopeJson(const nlohmann::json &project,
                         const ProjectEnvelopeParseOptions &options = {});

ProjectEnvelopeParseResult
parseProjectEnvelopeText(std::string_view project_json,
                         const ProjectEnvelopeParseOptions &options = {});

} // namespace Pelican
