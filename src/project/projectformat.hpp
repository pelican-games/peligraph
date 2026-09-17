#pragma once

#include "prefab.hpp"

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
    std::vector<PrefabRegistryEntry> prefabs;
};

struct ProjectEnvelopeParseOptions {
    std::string_view current_engine_version = "0.1.0";
    bool ignore_engine_version = false;
};

struct ProjectEnvelopeParseResult {
    ProjectEnvelope envelope;
    std::vector<std::string> warnings;
};

// Compares dotted numeric versions ("X.Y.Z").  One rule for every
// `engine_min_version` in the tree: project.json reads it here, and so does a
// package manifest.  Throws std::runtime_error for a malformed version.
int compareProjectVersions(std::string_view lhs, std::string_view rhs);

ProjectEnvelopeParseResult
parseProjectEnvelopeJson(const nlohmann::json &project,
                         const ProjectEnvelopeParseOptions &options = {});

ProjectEnvelopeParseResult
parseProjectEnvelopeText(std::string_view project_json,
                         const ProjectEnvelopeParseOptions &options = {});

} // namespace Pelican
