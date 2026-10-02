#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Pelican {

// CV-1b: the one hook the engine calls on an authored rendering graph
// document before it builds anything from it. Each registered authoring
// lowering -- a module of the making side, such as the 2D layer stack --
// replaces its own nodes with nodes of the core render vocabulary, in a fixed
// order. The engine builds the value graph, the pose graph, the attribute
// sources and the composition from the lowered document; the author's
// document (what the editor returns, digests, edits and undoes) is never
// replaced by it, and the lowered one is never saved.
//
// Idempotent: a document no lowering touches comes back as it was (the same
// bytes when dumped), so a path that calls it twice gets the same document. A
// lowering's named failure (GraphRegistryError) reaches the caller unchanged,
// which treats it as the candidate's failure.

struct AuthoringGeneratedSource {
    // A file the lowered document reads, relative to the project root. It
    // lives under .pelican/generated/ (never in the project's history) and is
    // named by its content's digest.
    std::string path;
    std::string text;
};

struct AuthoringLowering {
    nlohmann::json document;
    bool changed = false;
    std::vector<AuthoringGeneratedSource> generated_sources;
    // Notes that do not stop the lowering, each with the `lowering` it came
    // from.
    nlohmann::json diagnostics = nlohmann::json::array();
    // {<lowering name>: report}: what each registered lowering made of the
    // document, for the editor to show (get_authoring_lowering). A build
    // without any lowering reports {}.
    nlohmann::json reports = nlohmann::json::object();
};

AuthoringLowering lowerAuthoringDocument(const nlohmann::json &graph_document);

// Writes the generated sources under `project_root`. A file that already holds
// the same text is left as it is -- its name is its content's digest, so a
// published generation reading it never sees it change -- and a new one is
// written beside its place and renamed into it.
void writeAuthoringGeneratedSources(const AuthoringLowering &lowering, const std::filesystem::path &project_root);

} // namespace Pelican
