#pragma once

#include "../../project/renderconfigdocument.hpp"

#include <filesystem>
#include <functional>
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

using RenderConfigDocumentCommitFault = std::function<void(
    std::size_t committed_index,
    const RenderConfigCandidateDocument &document)>;

// Stages every changed document, records a prepared recovery manifest, applies
// the ordered document operations, and writes the committed state to the
// manifest last. Any in-process failure rolls the applied prefix back.
RenderConfigDocumentCommitReceipt commitRenderConfigCandidateDocuments(
    const std::filesystem::path &project_root,
    const RenderConfigCandidateDocumentSet &documents,
    const RenderConfigDocumentCommitFault &fault = {});

// Defensive support for a participant that violates the required
// commit-callback/noexcept-publication protocol after the source commit.
void rollbackCommittedRenderConfigDocuments(
    RenderConfigDocumentCommitReceipt receipt);

// Must run after PathResolver setup and before ProjectBasicConfig/Renderer
// load. A prepared transaction is rolled back; a committed transaction is
// verified and retained. Both states then remove transaction artifacts.
void recoverRenderConfigDocumentTransaction(
    const std::filesystem::path &project_root);

std::filesystem::path renderConfigTransactionDirectory(
    const std::filesystem::path &project_root);

} // namespace Pelican
