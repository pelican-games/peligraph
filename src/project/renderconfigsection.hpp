#pragma once

#include "renderconfigdocument.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>

namespace Pelican {

// P-2a-4b: one place decides where an authored rendering key lives, and one
// place decides how a legacy config becomes a `pelican.graph` root.  Every
// reader, writer and generator calls these instead of growing its own
// `if (graph)`; that is why the shipping project and the fixture can be
// asserted to be the same document under two different formattings.
//
// A `pelican.graph` root owns five keys of its own (`schema`, `version`,
// `definitions`, `nodes`, `outputs`) and keeps the whole legacy rendering
// config under the single member `rendering`.

inline bool isRenderGraphRoot(const nlohmann::json &root) {
    return root.is_object() &&
           root.value("schema", std::string{}) == "pelican.graph";
}

inline bool isRenderGraphRoot(const nlohmann::ordered_json &root) {
    return root.is_object() &&
           root.value("schema", std::string{}) == "pelican.graph";
}

// A legacy root is its own section.  A graph root without an object
// `rendering` member has nowhere to put an authored key, and says so by name
// instead of writing a member the composer never reads.
template <class JsonLike>
JsonLike &renderingSectionOf(JsonLike &root) {
    if (root.value("schema", std::string{}) != "pelican.graph") return root;
    const auto found = root.find("rendering");
    if (found == root.end() || !found->is_object())
        throw RenderConfigSectionUnlocated{};
    return *found;
}

// Reads answer "nothing authored here" for a graph root without a section;
// only an edit is refused by name.
inline const nlohmann::json &renderingSectionOrEmpty(
    const nlohmann::json &root) {
    static const nlohmann::json empty = nlohmann::json::object();
    try {
        return renderingSectionOf(root);
    } catch (const RenderConfigSectionUnlocated &) {
        return empty;
    }
}

// The one wrap.  `devcli project init`, the graph-root fixtures and the
// shipping project all pass through here, so "the fixture and the shipping
// config were wrapped by the same rule" is a value comparison and not a
// convention two files are asked to keep by hand.
template <class JsonLike>
std::remove_cv_t<JsonLike> wrapLegacyRenderingConfig(const JsonLike &legacy) {
    using Document = std::remove_cv_t<JsonLike>;
    if (!legacy.is_object()) {
        throw std::invalid_argument(
            "a rendering config to wrap must be a JSON object");
    }
    if (legacy.value("schema", std::string{}) == "pelican.graph") {
        throw std::invalid_argument(
            "a pelican.graph rendering config is already wrapped");
    }
    Document wrapped = Document::object();
    wrapped["schema"] = "pelican.graph";
    wrapped["version"] = 1;
    wrapped["definitions"] = Document::object();
    wrapped["nodes"] = Document::array();
    wrapped["outputs"] = Document::object();
    wrapped["rendering"] = legacy;
    return wrapped;
}

} // namespace Pelican
