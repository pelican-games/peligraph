#pragma once

#include "passfieldownership.hpp"
#include "rendergraphir.hpp"

#include <nlohmann/json.hpp>

namespace Pelican {

// RC-3c1a: the one JSON parser of a completed render graph
// (docs/design_render_graph_ir.md §2.2). It reads the normal form
// (normalizeRenderingJson, RC-3b): a config with `rendering_passes[]` -- the
// composed config the compiler plans, or a `pelican.graph` root, whose
// `rendering` section is read. It reads the keys the planning parse it
// replaced (RC-3c1b removed it from core) and the render target and buffer
// parsers (RC-3c2a removed them from core: it is the one read of the image
// and buffer declarations) read, accepts what they accepted and refuses what
// they refused. It also refuses, ahead of the execution parsers
// that refused them before (§2.3 and what the IR's info carries): a material
// resource reading a cube view or an explicit subresource same_pixel, an
// unknown compute schedule, a malformed compute ray_query /
// ray_tracing.acceleration_structure / ray_task_acceleration_structures.
//
// Every refusal is a GraphRegistryError (graphcomposition.hpp), a code and
// the subject's names ("<code> <detail>"):
//   render.json_not_normalized@1          {path}   a form other than the
//                                                  normal one (a string
//                                                  input, top-level passes)
//   render_graph.not_object@1             {graph?, field, index?}
//   render_graph.section_invalid@1        {graph?, field}
//   render_graph.graph_name_invalid@1     {index, graph?, reason}  not a
//                                                  string, empty, or a name
//                                                  another graph has
//   render_graph.node_name_invalid@1      {graph, field, index}
//   render_graph.node_field_invalid@1     {graph, node, field, reason, detail?}
//   render_graph.pass_field_ownership@1   {graph, node, detail}
//   render_graph.pass_shape@1             {graph, node, violation, target?, detail?}
//   render_graph.attachment_option_without_output@1 {graph, node, field}
//   render_graph.attachment_invalid@1     {graph, node, field, reason, target?, value?}
//   render_graph.resource_reference_invalid@1 {graph, node, field, resource}
//   render_graph.footprint_invalid@1      {graph, node, field, resource, reason}
//   render_graph.image_invalid@1          {resource?, field, reason}
//   render_graph.buffer_invalid@1         {resource?, field?, reason}  RC-3c2a:
//                                                  the whole declaration
//                                                  (size, size_from_extent of
//                                                  a config image, host
//                                                  source, command layout,
//                                                  initial_f32, view family,
//                                                  lifetime) and a declared
//                                                  size's contract
//                                                  (requireBufferSizeContract)
//   render_graph.graph_declaration_unsupported@1 {graph, field}  a graph's
//                                                  own render_targets or
//                                                  buffers
//   render_graph.snapshot_size_unavailable@1 {graph, node, resource}
//   render_graph.frame_node_order_invalid@1 {graph?, node?, reason}
//   render_graph.compute_task_invalid@1   {node, field, reason}
// (ray_task_acceleration_structures: only its form here; the tasks it names
// are resolved in the final revision, finalizeRenderGraph.)
// An image with a format_class and no format is read with its format
// unresolved (ImageResource::format_unresolved).
// `reason` is a fixed word per check; `detail` the refusing helper's sentence
// where a shared project helper refuses.
RenderGraphIR parseRenderGraph(const nlohmann::json &document,
                               PassFieldOwnershipCapabilities capabilities);

// RC-3c1b: how many times parseRenderGraph has run on this thread (one
// parse per completed revision; tests count the production path with it).
std::size_t renderGraphParseCount() noexcept;

} // namespace Pelican
