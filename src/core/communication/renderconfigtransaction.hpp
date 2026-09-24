#pragma once

#include "../../project/projectdocumenttransaction.hpp"
#include "../../project/renderconfigdocument.hpp"

#include <filesystem>
#include <utility>

namespace Pelican {

// The render config editor's names for the project document transaction
// (project/projectdocumenttransaction.hpp). MG-1 moved the transaction into
// pelican_project so `project migrate` shares it; the editor keeps calling it
// by the names it always had, and the on-disk format did not change.

using RenderConfigExternalModification = ProjectDocumentExternalModification;
using RenderConfigDocumentCommitReceiptEntry = ProjectDocumentCommitReceiptEntry;
using RenderConfigDocumentCommitReceipt = ProjectDocumentCommitReceipt;
using RenderConfigDocumentCommitFault = ProjectDocumentCommitFault;
using RenderConfigAtomicReplaceRoutes = ProjectDocumentAtomicReplaceRoutes;

inline RenderConfigDocumentCommitReceipt commitRenderConfigCandidateDocuments(
    const std::filesystem::path &project_root,
    const RenderConfigCandidateDocumentSet &documents,
    const RenderConfigDocumentCommitFault &fault = {}) {
    return commitProjectDocuments(project_root, documents.documents(), fault);
}

inline void rollbackCommittedRenderConfigDocuments(
    RenderConfigDocumentCommitReceipt receipt) {
    rollbackCommittedProjectDocuments(std::move(receipt));
}

// Must run after PathResolver setup and before ProjectBasicConfig/Renderer
// load; see recoverProjectDocumentTransaction.
inline void recoverRenderConfigDocumentTransaction(
    const std::filesystem::path &project_root) {
    recoverProjectDocumentTransaction(project_root);
}

inline std::filesystem::path renderConfigTransactionDirectory(
    const std::filesystem::path &project_root) {
    return projectDocumentTransactionDirectory(project_root);
}

inline RenderConfigAtomicReplaceRoutes
renderConfigAtomicReplaceRoutesForTesting() {
    return projectDocumentAtomicReplaceRoutesForTesting();
}

inline void renderConfigAtomicReplaceForTesting(
    const std::filesystem::path &source,
    const std::filesystem::path &destination) {
    projectDocumentAtomicReplaceForTesting(source, destination);
}

} // namespace Pelican
