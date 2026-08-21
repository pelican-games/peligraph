#include "renderconfigcandidate.hpp"

#include "pathresolver.hpp"
#include "../../project/renderconfigdocument.hpp"

#include <stdexcept>

namespace Pelican {

std::string loadRenderConfigCandidateText(
    const RenderConfigCandidateDocumentSet &documents,
    const PathResolver &fallback, std::string_view reference) {
    const auto normalized = fallback.normalizedReference(reference);
    if (const auto *candidate = documents.find(normalized)) {
        if (candidate->operation == RenderConfigDocumentOperation::erase) {
            throw std::runtime_error(
                "render config candidate references a deleted document: " +
                std::string{reference});
        }
        return candidate->bytes;
    }
    return fallback.loadText(reference);
}

} // namespace Pelican
