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
// (the resources are built in); only get_engine_shaders asks for it, never
// the catalog Studio reads at startup (EL-4b).
//
// EL-4c: reflecting compiles every GLSL source, seconds of work, so a worker
// thread does it and the frame loop and RPC never wait for it. The answer is
//   {status: "building"} until the worker is done, then for the rest of the
//   process {status: "ready", engine_shaders: [{reference, stage, forms,
//     loads, resource_port_include?, reflected_from | reflection_error,
//     pass_inputs?, variadic_inputs?,
//     resource_ports: [{port, binding, type, format?}],
//     fragment_outputs?, local_size?}]}
//   or {status: "failed", error} when the worker itself failed (a shader that
//   does not reflect is its own entry's reflection_error).
// A storage image port's `format` is its layout qualifier as the vocabulary
// names it (r8 is R8_UNORM); a port without one has none.
// Starts the worker when nothing has.
nlohmann::ordered_json engineShadersStatusJson();
// Starts the worker unless it is running or done, and returns at once. A
// running engine with RPC calls it after start so the list is warm before
// Studio asks.
void startEngineShadersReflection();
// Asks a running worker to stop after the shader it is reflecting and joins
// it; a done answer is kept, an unfinished one dropped (the next start
// begins again). The engine calls it as its RPC endpoint closes.
void stopEngineShadersReflection();
// How many times the reflection has been asked for or started in this
// process; a test reads it to see that get_graph_catalog never reaches it.
std::size_t engineShadersRequestCount();
// Tests: stop the worker and forget its answer, so the next request builds
// again; hold a worker before its first shader until released.
void resetEngineShadersReflectionForTesting();
void holdEngineShadersReflectionForTesting(bool held);

} // namespace Pelican
