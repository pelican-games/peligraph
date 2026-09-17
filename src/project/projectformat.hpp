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
    // `requires` in project.json: the build features this project cannot run
    // without. Kept in authoring order; `requires` itself is a C++ keyword.
    std::vector<std::string> requirements;
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

// The vocabulary of `requires`. P-2b knows exactly one name -- `animation`,
// the build feature registry's name for PELICAN_WITH_ANIMATION -- because a
// wider module namespace is X-1's decision, not this project's. An unknown
// name is `project.requires_unknown@1` rather than a silently ignored key.
std::vector<std::string> knownProjectRequirements();

// The build option a requirement stands for, e.g. `PELICAN_WITH_ANIMATION`.
std::string_view projectRequirementOption(std::string_view requirement);

// Reads and validates `requires` without asking whether this build satisfies
// it. Throws `project.requires_unknown@1` for a name outside the vocabulary.
std::vector<std::string> parseProjectRequirements(const nlohmann::json &project);

// The first requirement this build cannot satisfy, or nullopt. A corpus that
// walks `projects/` uses this to skip such a project by name instead of
// failing on the operations the disabled module never registered.
std::optional<std::string>
unsatisfiedProjectRequirement(const nlohmann::json &project);

} // namespace Pelican
