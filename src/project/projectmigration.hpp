#pragma once

#include "projectdocumenttransaction.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

// MG-1: moving a project written by an earlier version of the engine forward.
//
// The unit is a document's own `(schema, version)` envelope, not the engine
// version that saved it: a document hand-edited or merged from another branch
// carries its version with it, so a step is never applied twice and never
// skipped. `saved_by_engine` in project.json is a record for people and is not
// read by anything here except to report it.
//
// Every failure is a ProjectMigrationError whose what() is
// "<code>: <fields JSON>" (splitCodedRefusalMessage reads it back).

class ProjectMigrationError : public std::runtime_error {
    std::string code_;
    nlohmann::ordered_json fields_;

  public:
    ProjectMigrationError(std::string code, nlohmann::ordered_json fields);
    const std::string &code() const noexcept { return code_; }
    const nlohmann::ordered_json &fields() const noexcept { return fields_; }
};

inline constexpr std::string_view projectMigrationStepInvalidCode =
    "project.migration_step_invalid@1";
inline constexpr std::string_view projectMigrationStepDuplicateCode =
    "project.migration_step_duplicate@1";
inline constexpr std::string_view projectMigrationStepGapCode =
    "project.migration_step_gap@1";
inline constexpr std::string_view projectMigrationStepCycleCode =
    "project.migration_step_cycle@1";
inline constexpr std::string_view projectMigrationRequiredCode =
    "project.migration_required@1";
inline constexpr std::string_view projectMigrationStepFailedCode =
    "project.migration_step_failed@1";
inline constexpr std::string_view projectMigrationPlanStaleCode =
    "project.migration_plan_stale@1";
inline constexpr std::string_view projectMigrationApplyFailedCode =
    "project.migration_apply_failed@1";
inline constexpr std::string_view projectMigrationProjectMissingCode =
    "project.migration_project_missing@1";
// MG-1b: apply found the project's document transaction directory taken --
// by an engine or Studio that has the project open and is committing an edit,
// or left behind by a writer that stopped; from outside the two look the same.
// Nothing is read, recovered or waited for. {path: the directory, message}.
inline constexpr std::string_view projectMigrationTransactionBusyCode =
    "project.migration_transaction_busy@1";

// What a step is told about the document it rewrites.
struct ProjectContext {
    std::filesystem::path project_root;
    // Project-relative, '/'-separated.
    std::string document_path;
};

// A place the step could not rewrite by itself. `code` names the kind of
// problem ("<schema>.<name>@<n>"), `pointer` is the RFC 6901 pointer into the
// document as the step received it, `message` says what to do by hand.
struct MigrationManualItem {
    std::string code;
    std::string pointer;
    std::string message;
};

struct StepResult {
    // The rewritten document. The registry writes `version: to_version` into
    // it after the step returns, so a step only changes what it migrates.
    nlohmann::ordered_json document;
    std::vector<MigrationManualItem> manual_items;
};

using DocumentMigrationPlanFunction = std::function<StepResult(
    const nlohmann::ordered_json &document, const ProjectContext &context)>;

// One registered step: `schema` documents at `from_version` become
// `to_version` documents. The shape follows GraphModuleManifest: a value that
// is handed to a registry, which accepts it whole or refuses it by name.
struct DocumentMigrationStep {
    std::string id;
    std::string schema;
    int from_version = 0;
    int to_version = 0;
    std::string description;
    DocumentMigrationPlanFunction plan;
};

class DocumentMigrationRegistry {
    std::map<std::pair<std::string, int>, DocumentMigrationStep> steps_;

  public:
    // project.migration_step_invalid@1   empty id or schema, a version below
    //                                    1, or no plan function
    // project.migration_step_duplicate@1 the id, or (schema, from_version),
    //                                    is already registered
    // project.migration_step_cycle@1     to_version <= from_version: the step
    //                                    points back at a version the chain
    //                                    already passes through
    // project.migration_step_gap@1       to_version > from_version + 1
    // A rejected step changes nothing.
    void registerStep(DocumentMigrationStep step);

    const DocumentMigrationStep *find(std::string_view schema,
                                      int from_version) const;
    bool empty() const noexcept { return steps_.empty(); }
    std::size_t size() const noexcept { return steps_.size(); }
    // Steps in (schema, from_version) order, without the plan functions.
    nlohmann::ordered_json catalog() const;
};

// The steps the engine itself ships. At v0 there are none: v0 is the first
// version a later engine migrates from.
DocumentMigrationRegistry makeBuiltinDocumentMigrationRegistry();
const DocumentMigrationRegistry &builtinDocumentMigrationRegistry();

// A document whose envelope matches a registered step.
struct ProjectMigrationDetection {
    std::string path; // project-relative, '/'-separated
    std::string schema;
    int from_version = 0;
    // The version the chain of registered steps ends at.
    int to_version = 0;
};

// Reads the `schema` / `version` envelope of every `*.json` under the project
// root, raw, before any parser: a directory that starts with '.' (.pelican,
// .git) is skipped, a file that is not a JSON object with a string `schema`
// and an integer `version` is not a document. Sorted by path.
std::vector<ProjectMigrationDetection> detectProjectMigration(
    const std::filesystem::path &project_root,
    const DocumentMigrationRegistry &registry);

// The gate every entry that opens a project passes before a parser reads a
// document: it first resolves a document transaction a crash left behind
// (recoverProjectDocumentTransaction), then throws
// project.migration_required@1 {documents:[{path, schema, from, to}...]} when
// detectProjectMigration finds anything.
void requireProjectMigrationNotRequired(
    const std::filesystem::path &project_root,
    const DocumentMigrationRegistry &registry =
        builtinDocumentMigrationRegistry());

struct ProjectMigrationDocumentPlan {
    std::string path;
    std::string schema;
    int from_version = 0;
    int to_version = 0;
    std::vector<std::string> steps;
    std::vector<MigrationManualItem> manual_items;
    std::string digest;
    std::string before_bytes;
    std::string after_bytes;
};

struct ProjectMigrationPlan {
    std::filesystem::path project_root;
    std::vector<ProjectMigrationDocumentPlan> documents;
    std::optional<std::string> saved_by_engine;
    std::string engine_version;
    std::string project_json_digest;
    std::string project_json_before;
    // project.json with `saved_by_engine` updated, after any step of its own.
    std::string project_json_after;

    bool required() const noexcept { return !documents.empty(); }
    // The CAS set apply is bound to: every document apply writes, including
    // project.json, by path -> digest.
    std::map<std::string, std::string> expectedDigests() const;
    // {project_root, required, engine_version, saved_by_engine,
    //  documents:[{path, schema, from, to, steps, digest, manual_items}],
    //  expected:{path: digest}}
    nlohmann::ordered_json toJson() const;
};

// A dry run: reads, never writes. project.migration_step_failed@1 when a step
// throws or returns something that is not an object of the same schema.
ProjectMigrationPlan planProjectMigration(
    const std::filesystem::path &project_root,
    const DocumentMigrationRegistry &registry);

struct ProjectMigrationApplyOptions {
    // The CAS: when set, it must equal the plan's expectedDigests() exactly,
    // or apply writes nothing and throws project.migration_plan_stale@1.
    std::optional<std::map<std::string, std::string>> expected_digests;
    // The transaction's fault seam, passed through unchanged.
    ProjectDocumentCommitFault fault;
    // Names the backup directory; defaults to the current UTC time.
    std::function<std::string()> timestamp;
};

struct ProjectMigrationApplyResult {
    ProjectMigrationPlan plan;
    bool applied = false;
    // Project-relative ".pelican/migration/<timestamp>", empty when nothing
    // was applied.
    std::string backup;
    nlohmann::ordered_json toJson() const;
};

// lock -> plan -> CAS -> (a) copy every document it writes to
// .pelican/migration/<timestamp>/ (b)+(c) replace the migrated documents and
// project.json's `saved_by_engine` in one project document transaction. A
// failure inside the transaction rolls every document back and removes the
// backup, and is reported as project.migration_apply_failed@1 {message}.
// Nothing to migrate writes nothing.
//
// MG-1b: the lock is the project's transaction directory, taken before the
// plan is read and held through the commit. apply never recovers a
// transaction: a directory that is already there may be another process's
// commit in progress, and rolling it back would undo that edit. It stops with
// project.migration_transaction_busy@1 instead and leaves the directory as it
// is; the gate that opens the project (requireProjectMigrationNotRequired) is
// where a leftover is resolved.
ProjectMigrationApplyResult applyProjectMigration(
    const std::filesystem::path &project_root,
    const DocumentMigrationRegistry &registry,
    const ProjectMigrationApplyOptions &options = {});

} // namespace Pelican
