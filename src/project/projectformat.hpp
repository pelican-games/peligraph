#pragma once

#include "prefab.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// The version of this engine, as `engine_min_version` compares it and as
// project.json's `saved_by_engine` records it.
inline constexpr std::string_view pelicanEngineVersion = "0.1.0";

// `engine_min_version` names a version newer than this engine.
inline constexpr std::string_view projectEngineTooOldCode =
    "project.engine_too_old@1";
// CC-1b: basic_config.camera.up, removed when the camera convention changed
// (docs/design_coordinate_convention.md): a camera looks along -Z of its
// rotation, its up is the rotation's +Y, and the world's up is +Y.
inline constexpr std::string_view projectCameraUpRemovedCode =
    "project.camera_up_removed@1";
inline constexpr std::string_view projectCameraUpRemovedMessage =
    "basic_config.camera.up was removed: the camera convention changed (a "
    "camera looks along -Z of its rotation, its up is the rotation's +Y and "
    "the world's up is +Y). Convert the project with "
    "`uv run tools/convert_camera_convention.py <project dir>`, which removes "
    "the key and turns its cameras to look the way they did";

// HW-1: project.json's `hot_reload` is not an array of distinct
// project-relative paths (not an array, an entry that is not a string, an
// empty entry, an absolute path, a `..` component, or a duplicate).
inline constexpr std::string_view projectHotReloadInvalidCode =
    "project.hot_reload_invalid@1";

struct ProjectEnvelope {
    std::optional<std::string> name;
    std::optional<std::string> engine_min_version;
    // MG-1: which engine last wrote project.json through `project init` or
    // `project migrate`. A record for people and diagnostics only: nothing
    // decides anything by it (a migration is decided by each document's own
    // `(schema, version)`), and a value that is not a string is ignored rather
    // than refused, as an engine before MG-1 ignores the whole key.
    std::optional<std::string> saved_by_engine;
    // `requires` in project.json: the build features this project cannot run
    // without. Kept in authoring order; `requires` itself is a C++ keyword.
    std::vector<std::string> requirements;
    nlohmann::json basic_config = nlohmann::json::object();
    nlohmann::json asset_stores = nlohmann::json::object();
    // HW-1: `hot_reload` in project.json, the folders and files the file
    // watch covers, as normalized logical paths (`assets/textures`, no
    // trailing `/`). nullopt when the key is absent: the watch keeps its
    // launch default (the whole project, or nothing under an RPC driver).
    // An empty list watches nothing.
    std::optional<std::vector<std::string>> hot_reload;
    std::vector<PrefabRegistryEntry> prefabs;
};

struct ProjectEnvelopeParseOptions {
    std::string_view current_engine_version = pelicanEngineVersion;
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

// The vocabulary of `requires`: one name per build module a project, a
// component codec or a package can need (projectrequirements.def). An unknown
// name is `project.requires_unknown@1` rather than a silently ignored key.
std::vector<std::string> knownProjectRequirements();

// The build option a requirement stands for, e.g. `PELICAN_WITH_ANIMATION`.
// Empty for a name outside the vocabulary.
std::string_view projectRequirementOption(std::string_view requirement);

// `<requirement>.module_unavailable@1` when this build left the module out;
// empty when the build has it or the name is outside the vocabulary. Every
// place that refuses something because a module is absent -- a project's
// `requires`, a component's codec, a package's capabilities -- answers with
// this one name.
std::string_view projectRequirementUnavailableCode(std::string_view requirement);

// Reads and validates `requires` without asking whether this build satisfies
// it. Throws `project.requires_unknown@1` for a name outside the vocabulary.
std::vector<std::string> parseProjectRequirements(const nlohmann::json &project);

// The first requirement this build cannot satisfy, or nullopt. A corpus that
// walks `projects/` uses this to skip such a project by name instead of
// failing on the operations the disabled module never registered.
std::optional<std::string>
unsatisfiedProjectRequirement(const nlohmann::json &project);

// HW-1: reads `hot_reload`. nullopt when absent; otherwise each entry
// normalized (generic separators, no `./`, no trailing `/`). Throws
// `project.hot_reload_invalid@1` for the refusals above. A path that does
// not exist is accepted: a folder may be created later.
std::optional<std::vector<std::string>>
parseProjectHotReload(const nlohmann::json &project);

} // namespace Pelican
