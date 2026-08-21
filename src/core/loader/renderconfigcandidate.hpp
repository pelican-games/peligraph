#pragma once

#include <string>
#include <string_view>

namespace Pelican {

class PathResolver;
class RenderConfigCandidateDocumentSet;

// Request-local overlay used by both feature and preset loading. Candidate
// documents win; references absent from the set fall back to the immutable
// production PathResolver instance.
std::string loadRenderConfigCandidateText(
    const RenderConfigCandidateDocumentSet &documents,
    const PathResolver &fallback, std::string_view reference);

} // namespace Pelican
