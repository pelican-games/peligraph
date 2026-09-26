#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// GT-1: the decomposed standard features the engine ships as graph templates
// (engine://graphs/<name>.json; docs/design_graph_loops.md 5.3), and how a
// project takes one in. A template is a pelican.graph document whose
// `definitions` and `nodes` the project copies into its own graph document;
// its `decomposes` says what happens to the one-piece feature it stands for:
//
//   - {"feature": <ref>, "remove_feature": true}: the feature instance with
//     that ref leaves the features list (bloom: the graph replaces it all);
//   - {"feature": <ref>, "pass_replacements": {...}}: the instance keeps its
//     targets, views and surface resources, and those of its passes / tasks
//     are replaced by the graph's nodes (planar: the mip chain);
//   - "shader_defines": defines the feature gave every shader from its
//     parameters (bloom's threshold and knee), added to the rendering
//     section's shader_defines.
//
// Nothing in the engine's composition reads `decomposes`. apply_graph_template
// (renderconfigeditor.cpp) runs these steps against the rendering section or
// a forked preset and the root graph in one transaction.
namespace Pelican {

// apply_graph_template named no template the engine ships
// (engine://graphs/<name>.json): fields {template}.
inline constexpr std::string_view graphTemplateUnknownCode = "graph_template_unknown";
// The template's `decomposes` does not say what it replaces: fields
// {reason}.
inline constexpr std::string_view graphTemplateInvalidCode = "graph_template_invalid";
// The feature the template decomposes is not listed where the composition
// reads it (the rendering section, or the preset): fields {feature}.
inline constexpr std::string_view graphTemplateFeatureAbsentCode = "graph_template_feature_absent";
// The feature's instance already replaces a pass of the template's with
// another node: fields {feature, pass, current, requested}.
inline constexpr std::string_view graphTemplatePassReplacementConflictCode =
    "graph_template_pass_replacement_conflict";

class GraphTemplateError : public std::runtime_error {
    std::string code_;
    nlohmann::ordered_json fields_;

  public:
    GraphTemplateError(std::string code, nlohmann::ordered_json fields, const std::string &message)
        : std::runtime_error(message), code_(std::move(code)), fields_(std::move(fields)) {}
    const std::string &code() const noexcept { return code_; }
    const nlohmann::ordered_json &fields() const noexcept { return fields_; }
};

struct GraphTemplateDecomposes {
    std::string feature;
    bool remove_feature = false;
    // {pass or task name, graph node id}, in the template's order.
    std::vector<std::pair<std::string, std::string>> pass_replacements;
    std::vector<std::string> shader_defines;
};

// The template's `decomposes`; GraphTemplateError graph_template_invalid when
// it has none, names no feature, or a member has the wrong type.
GraphTemplateDecomposes readGraphTemplateDecomposes(const nlohmann::ordered_json &graph_template);

// The feature reference a features-list entry names ("" when it names none).
std::string graphTemplateFeatureRef(const nlohmann::ordered_json &entry);

// How many entries of `features` (a features list, or anything else: 0)
// name `feature`.
std::size_t countGraphTemplateFeature(const nlohmann::ordered_json &features, std::string_view feature);

struct GraphTemplateBody {
    std::vector<std::string> definitions;
    std::vector<std::string> nodes;
};

// Step 1: the template's definitions and root nodes appended to `graph` (a
// pelican.graph root), in the template's order. A root node id or a
// definition name already taken is refused as graph.node_duplicate@1 {node,
// scope: "root"} / graph.definition_duplicate@1 {definition}
// (GraphRegistryError). `outputs` is added (empty) when the root has none.
// Nothing is changed when it throws.
GraphTemplateBody copyGraphTemplateBody(nlohmann::ordered_json &graph, const nlohmann::ordered_json &graph_template);

// Steps 2 and 3 on one features list: every instance of the feature is
// removed (remove_feature), or its pass_replacements gain the template's (a
// string entry becomes {ref, pass_replacements}). A replacement the instance
// already has is kept; one that replaces the same pass with another node is
// GraphTemplateError graph_template_pass_replacement_conflict. Returns how
// many instances there were. Nothing is changed when it throws.
std::size_t applyGraphTemplateToFeatures(nlohmann::ordered_json &features, const GraphTemplateDecomposes &decomposes);

// Step 4: the template's shader_defines appended to `section`'s
// shader_defines (created when missing), each once. Returns the ones added.
std::vector<std::string> addGraphTemplateShaderDefines(nlohmann::ordered_json &section,
                                                       const GraphTemplateDecomposes &decomposes);

// All four steps on a graph root whose own rendering section lists the
// feature: GraphTemplateError graph_template_feature_absent when it does not.
nlohmann::ordered_json applyGraphTemplate(nlohmann::ordered_json document, const nlohmann::ordered_json &graph_template);
// The same on nlohmann::json documents (the tests' form).
nlohmann::json applyGraphTemplate(const nlohmann::json &document, const nlohmann::json &graph_template);

} // namespace Pelican
