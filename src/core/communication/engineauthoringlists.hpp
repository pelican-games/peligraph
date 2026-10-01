#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>

namespace Pelican {

// EL-4: lists the engine answers Studio with, each read off the table the
// engine checks against, so Studio offers what the engine takes instead of
// copying it. get_graph_catalog adds the two format lists beside the module
// catalog; the engine shaders are get_engine_shaders' own (EL-4b).

// The formats each socket of a material pass without material_outputs takes,
// per contract (validateMaterialPassAttachments' tables), and the formats a
// material_outputs pass takes per output type and for its depth.
//   [{contract, material_outputs, sockets: [{socket, aspect, formats}],
//     combinations: [{<socket>: <format>, ...}]}]
nlohmann::ordered_json materialPassContractsJson();
//   {color: {<output type>: [<format>...]}, depth: [<format>...]}
nlohmann::ordered_json materialOutputFormatsJson();

// The shaders the engine ships that a pass's stem reference loads
// (engine://<stem> for a stage), with what reflection reads of each: the
// pass-input set a fullscreen or raster pass binds, the resource ports, the
// fragment outputs and a compute shader's local size. Read once per process
// (the resources are built in), on the first call: reflecting compiles every
// GLSL source, seconds of work, so only get_engine_shaders asks for it and
// the catalog Studio reads at startup does not (EL-4b).
//   [{reference, stage, forms, loads, resource_port_include?,
//     reflected_from | reflection_error, pass_inputs?, variadic_inputs?,
//     resource_ports, fragment_outputs?, local_size?}]
const nlohmann::ordered_json &engineShadersJson();
// How many times engineShadersJson() has been asked for in this process; a
// test reads it to see that get_graph_catalog never reaches the reflection.
std::size_t engineShadersRequestCount();

} // namespace Pelican
