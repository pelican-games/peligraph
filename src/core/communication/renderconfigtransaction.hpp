#pragma once

#include "../../project/renderconfigdocument.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {

class RenderConfigExternalModification final : public std::runtime_error {
  public:
    explicit RenderConfigExternalModification(const std::string &message)
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

struct RenderConfigDocumentCommitReceiptEntry {
    std::filesystem::path path;
    std::optional<std::string> before_bytes;
    std::optional<std::string> after_bytes;
};

struct RenderConfigDocumentCommitReceipt {
    std::filesystem::path project_root;
    std::vector<RenderConfigDocumentCommitReceiptEntry> entries;

    bool changed() const noexcept { return !entries.empty(); }
};

// Called after each changed document is applied, and once more with
// `committed_index == the number of changed documents` immediately after the
// committed marker becomes visible -- the terminal step, where a fixture can
// stop the process to leave a committed-but-uncleaned transaction on disk. The
// decision is already durable there, so a throw from that last call is
// swallowed rather than allowed to re-enter the rollback path.
using RenderConfigDocumentCommitFault = std::function<void(
    std::size_t committed_index,
    const RenderConfigCandidateDocument &document)>;

// Stages every changed document, records a prepared recovery manifest, applies
// the ordered document operations, and publishes the committed marker last.
// Any in-process failure before that marker rolls the applied prefix back.
//
// The marker is a separate file created exclusively; manifest.json is written
// once and never superseded, so the transaction's own decision never has to
// replace a file another process holds open.
RenderConfigDocumentCommitReceipt commitRenderConfigCandidateDocuments(
    const std::filesystem::path &project_root,
    const RenderConfigCandidateDocumentSet &documents,
    const RenderConfigDocumentCommitFault &fault = {});

// Defensive support for a participant that violates the required
// commit-callback/noexcept-publication protocol after the source commit.
void rollbackCommittedRenderConfigDocuments(
    RenderConfigDocumentCommitReceipt receipt);

// Must run after PathResolver setup and before ProjectBasicConfig/Renderer
// load. A transaction is committed when its `committed` marker exists and
// digests the prepared manifest; without the marker it is prepared and is
// rolled back. A marker that does not match its manifest is rejected by name
// (render_authoring_manifest_mismatch@1) instead of being read as either. A
// manifest carrying the previous format's `status: "committed"` stops recovery
// with RenderConfigLegacyTransactionDirectory and is left on disk.
void recoverRenderConfigDocumentTransaction(
    const std::filesystem::path &project_root);

std::filesystem::path renderConfigTransactionDirectory(
    const std::filesystem::path &project_root);

// Which route the transaction's destination replacements took. On Windows the
// primary route is a POSIX-semantics rename, which supersedes a destination
// another process still holds open; MoveFileExW is the named fallback for a
// Windows or a filesystem without FileRenameInfoEx, and every fallback records
// why it happened. Tests read these to prove the production route ran; the
// counters are process-wide and monotonic, so read a delta.
struct RenderConfigAtomicReplaceRoutes {
    std::uint64_t posix_renames = 0;
    std::uint64_t legacy_fallbacks = 0;
    std::string last_fallback_reason;
};
RenderConfigAtomicReplaceRoutes renderConfigAtomicReplaceRoutesForTesting();

// The transaction's replace primitive on its own, so a test can put a
// contending opener against it without driving a whole editor commit.
void renderConfigAtomicReplaceForTesting(
    const std::filesystem::path &source,
    const std::filesystem::path &destination);

} // namespace Pelican
