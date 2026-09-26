#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// LP-3b: a one-piece material pass rewritten as the graph nodes it means
// (docs/design_graph_loops.md 4.3, 9): a render.material_pass with the same
// name, fields and place, its attachments through render.target_ref nodes,
// and its draws from a graph.loop over draw_items whose one (else) branch is
// a definition holding draw.material -- draw.variant when the pass names a
// material_variant. The pass's material_filter becomes the loop's
// `where.tags`. decompose_material_pass applies it to the rendering section
// or to a forked preset, and to the root graph, in one transaction.
namespace Pelican {

inline constexpr std::string_view materialPassNotDecomposableCode = "material_pass_not_decomposable";

// material_pass_not_decomposable {pass, reason, ...}. `reason`:
//   not_found              -- the list holds no pass of that name
//   not_material           -- it is not a material pass (+ type)
//   contract_unsupported   -- its contract has no draw_loop (+ contract)
//   field_unsupported      -- a field render.material_pass cannot carry (+ field)
//   attachment_unsupported -- an attachment the graph cannot name: a
//                             {target, subresource} entry, no depth, or a
//                             color count its sockets do not have (+ socket)
// The editor adds feature_pass (+ feature) and graph_pass for a name the
// rendering section and the preset do not declare.
class MaterialPassDecomposeError : public std::runtime_error {
    std::string code_;
    nlohmann::ordered_json fields_;

  public:
    MaterialPassDecomposeError(std::string code, nlohmann::ordered_json fields, const std::string &message)
        : std::runtime_error(message), code_(std::move(code)), fields_(std::move(fields)) {}
    const std::string &code() const noexcept { return code_; }
    const nlohmann::ordered_json &fields() const noexcept { return fields_; }
};

// The composed declaration of a render target, or nullptr when it is not
// known. A new render.target_ref declares pelican.render.depth@1 (device,
// projection) for the depth attachment -- with the topology of a cube or
// layered target -- and the untyped pelican.render.legacy_opaque_image@1 for
// a color one: the one-piece pass states nothing about the image.
using ComposedRenderTargetLookup = std::function<const nlohmann::json *(const std::string &)>;

struct MaterialPassDecomposition {
    // The pass family the pass was declared in (the node's pass_set).
    std::string family;
    // The node's insert ({before|after: <neighbor>, declared: true}); null
    // when the pass was the only one of its family (it is appended).
    nlohmann::ordered_json insert;
    std::string loop;
    std::string definition;
    // The render.target_ref nodes the pass's sockets read, in socket order
    // (colors, then depth); an existing node naming the same target is reused.
    std::vector<std::string> target_refs;
    std::vector<std::string> added_target_refs;
};

// Takes the material pass `pass` out of `pass_owner["rendering_passes"]` (the
// rendering section, or a preset's `config`) and adds its graph form to
// `graph` (the pelican.graph root: `nodes`, `definitions`). The ids it adds
// are the pass's own name, `<pass>_draws` (the loop), `<pass>_draw` (the
// definition) and `<target>_ref` for a target no render.target_ref names yet;
// one already taken is refused as graph.node_duplicate@1 {node, scope:
// "root"} / graph.definition_duplicate@1 {definition} (GraphRegistryError).
// Other refusals are MaterialPassDecomposeError. Nothing is changed when it
// throws.
MaterialPassDecomposition decomposeMaterialPass(nlohmann::ordered_json &pass_owner, nlohmann::ordered_json &graph,
                                                const std::string &pass,
                                                const ComposedRenderTargetLookup &composed_target = {});

} // namespace Pelican
