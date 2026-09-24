#pragma once

#include "renderconfigdocument.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace Pelican {

// P-2a-4b: one place decides where an authored rendering key lives. Every
// reader, writer and generator calls these instead of growing its own
// `if (graph)`.
//
// The authored root is a `pelican.graph` document: it owns five keys of its
// own (`schema`, `version`, `definitions`, `nodes`, `outputs`) and keeps the
// rendering config under the single member `rendering`. MG-2b: a root in the
// older form (the rendering config's own keys at the root) is not read; it
// stops by name with RenderConfigLegacyRoot (`render_config.legacy_root@1`).

inline bool isRenderGraphRoot(const nlohmann::json &root) {
    return root.is_object() &&
           root.value("schema", std::string{}) == "pelican.graph";
}

inline bool isRenderGraphRoot(const nlohmann::ordered_json &root) {
    return root.is_object() &&
           root.value("schema", std::string{}) == "pelican.graph";
}

// A root that is not a graph root is refused by name. A graph root without an
// object `rendering` member has nowhere to put an authored key, and says so by
// name instead of writing a member the composer never reads.
template <class JsonLike>
JsonLike &renderingSectionOf(JsonLike &root) {
    if (!root.is_object() ||
        root.value("schema", std::string{}) != "pelican.graph")
        throw RenderConfigLegacyRoot{};
    const auto found = root.find("rendering");
    if (found == root.end() || !found->is_object())
        throw RenderConfigSectionUnlocated{};
    return *found;
}

// Reads answer "nothing authored here" for a graph root without a section;
// only an edit is refused by name. A root that is not a graph root is refused
// here too.
inline const nlohmann::json &renderingSectionOrEmpty(
    const nlohmann::json &root) {
    static const nlohmann::json empty = nlohmann::json::object();
    try {
        return renderingSectionOf(root);
    } catch (const RenderConfigSectionUnlocated &) {
        return empty;
    }
}

} // namespace Pelican
