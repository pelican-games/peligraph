#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// MG-1: the atomic replacement of a set of project documents. It began as the
// render config editor's transaction (G-3a) and is now the one transaction
// every writer of several project documents at once goes through: the render
// config editor and `project migrate`. There is one transaction directory per
// project, so one writer excludes the other, and the recovery that runs when a
// project is opened resolves whichever of them crossed a crash.
//
// The on-disk format is the render config transaction's, unchanged: the
// directory `.pelican/render-authoring.transaction`, a manifest.json with
// schema `pelican.render_authoring_transaction` version 1 written once, and a
// separate `committed` marker that is the durable decision. MG-2a's refusal of
// the previous format (`status: "committed"` inside manifest.json) stands as
// it was.

// The sha256 (hex) of a document's bytes: the digest every expected state and
// every CAS in this transaction compares.
std::string projectDocumentDigest(std::string_view bytes);

enum class ProjectDocumentOperation {
    create,
    replace,
    erase,
};

enum class ProjectDocumentExistence {
    missing,
    present,
};

struct ProjectDocumentExpectedState {
    ProjectDocumentExistence existence = ProjectDocumentExistence::missing;
    std::string digest;

    bool operator==(const ProjectDocumentExpectedState &) const = default;
};

struct ProjectCandidateDocument {
    std::string reference;
    std::string normalized_reference;
    std::filesystem::path path;
    ProjectDocumentOperation operation = ProjectDocumentOperation::replace;
    ProjectDocumentExpectedState expected;
    // Empty for erase; otherwise the exact candidate bytes.
    std::string bytes;

    bool operator==(const ProjectCandidateDocument &) const = default;
};

class ProjectDocumentExternalModification final : public std::runtime_error {
  public:
    explicit ProjectDocumentExternalModification(const std::string &message)
        : std::runtime_error{message} {}
};

// MG-2a: a transaction directory in the format that recorded its decision as
// `status: "committed"` inside manifest.json. Nothing writes that format any
// more, and it is not read: recovery and a new commit stop on it by name and
// leave the directory where it is, for the developer to remove by hand.
//
// what() is "render_config.legacy_transaction_dir@1: {JSON fields}", the shape
// an edit ticket carries a named refusal in (splitCodedRefusalMessage); the
// fields are {path, message}.
inline constexpr std::string_view renderConfigLegacyTransactionDirCode =
    "render_config.legacy_transaction_dir@1";

class RenderConfigLegacyTransactionDirectory final : public std::runtime_error {
    std::filesystem::path path_;

  public:
    explicit RenderConfigLegacyTransactionDirectory(
        std::filesystem::path directory);
    const std::filesystem::path &path() const noexcept { return path_; }
    // {"path": <the directory, UTF-8>}
    nlohmann::json errorFields() const;
};

struct ProjectDocumentCommitReceiptEntry {
    std::filesystem::path path;
    std::optional<std::string> before_bytes;
    std::optional<std::string> after_bytes;
};

struct ProjectDocumentCommitReceipt {
    std::filesystem::path project_root;
    std::vector<ProjectDocumentCommitReceiptEntry> entries;

    bool changed() const noexcept { return !entries.empty(); }
};

// Called after each changed document is applied, and once more with
// `committed_index == the number of changed documents` immediately after the
// committed marker becomes visible -- the terminal step, where a fixture can
// stop the process to leave a committed-but-uncleaned transaction on disk. The
// decision is already durable there, so a throw from that last call is
// swallowed rather than allowed to re-enter the rollback path.
using ProjectDocumentCommitFault = std::function<void(
    std::size_t committed_index, const ProjectCandidateDocument &document)>;

// Stages every changed document, records a prepared recovery manifest, applies
// the ordered document operations, and publishes the committed marker last.
// Any in-process failure before that marker rolls the applied prefix back.
//
// The marker is a separate file created exclusively; manifest.json is written
// once and never superseded, so the transaction's own decision never has to
// replace a file another process holds open.
ProjectDocumentCommitReceipt commitProjectDocuments(
    const std::filesystem::path &project_root,
    std::span<const ProjectCandidateDocument> documents,
    const ProjectDocumentCommitFault &fault = {});

// Defensive support for a participant that violates the required
// commit-callback/noexcept-publication protocol after the source commit.
void rollbackCommittedProjectDocuments(ProjectDocumentCommitReceipt receipt);

// Runs when a project is opened, before anything reads a member of the
// document set. A transaction is committed when its `committed` marker exists
// and digests the prepared manifest; without the marker it is prepared and is
// rolled back. A marker that does not match its manifest is rejected by name
// (render_authoring_manifest_mismatch@1) instead of being read as either. A
// manifest carrying the previous format's `status: "committed"` stops recovery
// with RenderConfigLegacyTransactionDirectory and is left on disk.
void recoverProjectDocumentTransaction(
    const std::filesystem::path &project_root);

// `<project_root>/.pelican/render-authoring.transaction`. The name predates the
// generalization and is kept: it is what a crashed older writer left behind,
// and what recovery has to find.
std::filesystem::path projectDocumentTransactionDirectory(
    const std::filesystem::path &project_root);

// Which route the transaction's destination replacements took. On Windows the
// primary route is a POSIX-semantics rename, which supersedes a destination
// another process still holds open; MoveFileExW is the named fallback for a
// Windows or a filesystem without FileRenameInfoEx, and every fallback records
// why it happened. Tests read these to prove the production route ran; the
// counters are process-wide and monotonic, so read a delta.
struct ProjectDocumentAtomicReplaceRoutes {
    std::uint64_t posix_renames = 0;
    std::uint64_t legacy_fallbacks = 0;
    std::string last_fallback_reason;
};
ProjectDocumentAtomicReplaceRoutes projectDocumentAtomicReplaceRoutesForTesting();

// The transaction's replace primitive on its own, so a test can put a
// contending opener against it without driving a whole editor commit.
void projectDocumentAtomicReplaceForTesting(
    const std::filesystem::path &source,
    const std::filesystem::path &destination);

// Where a fallback of the replace route is reported. pelican_project has no
// logger of its own; the engine installs one when it sets its logger up, and
// a tool that installs nothing still has the counters above.
using ProjectDocumentTransactionWarningSink = void (*)(std::string_view message);
void setProjectDocumentTransactionWarningSink(
    ProjectDocumentTransactionWarningSink sink) noexcept;

} // namespace Pelican
