#pragma once

#include "../../project/projectpathresolver.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <string_view>

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

// EL-5: get_shader_interface {reference, refresh?}: what reflection reads of
// each stage of a stem reference, as get_engine_shaders' entries read it.
//   {status: "building"}
//   | {status: "ready", reference, stages: [<get_engine_shaders entry>...]}
//   | {status: "failed", reference, error}
// A project stem's stages are the files ShaderLibrary::loadFromStemReference
// would load (each stage's source and .spv, by the same candidates and
// check, read through `resolve`); none: failed with error
// "shader_not_found" at once. A worker thread of its own builds the answer
// (one reference at a time, queued) and keeps it per reference with the
// hash of those files' bytes: changed bytes, or `refresh`, build it again.
// A stage that does not compile or reflect is its entry's
// reflection_error. An engine:// stem is get_engine_shaders' entries of the
// stem (building while that list builds; refresh does not apply).
// A reference the resolver refuses throws std::invalid_argument.
// RTL-1b: `virtual_includes` are generated includes a project source may
// read, as the pass that runs it would give them (the project's ray group
// layout header, __pelican_ray_group_layout.glsl); they are part of the
// answer's key, so a changed layout builds it again.
using ShaderReferenceResolver = std::function<ResolvedRef(std::string_view)>;
nlohmann::ordered_json shaderInterfaceStatusJson(
    std::string_view reference, bool refresh, const ShaderReferenceResolver &resolve,
    std::vector<std::pair<std::string, std::string>> virtual_includes = {});
// Stops the worker after the stage it is reflecting and joins it; done
// answers are kept, queued builds dropped. The engine calls it with
// stopEngineShadersReflection.
void stopShaderInterfaceReflection();
// Tests: stop and forget every answer; hold the worker before its next
// build until released.
void resetShaderInterfaceReflectionForTesting();
void holdShaderInterfaceReflectionForTesting(bool held);

// EL-6: get_registered_providers {}: the provider names each render provider
// registry would resolve now, keyed by the document field that names one:
//   {draw_sort: [<entry>...],            draw_sort.{opaque,transparent}.provider
//    pass_implementation: [<entry>...],  a pass's implementation.provider
//    region_replacements: [<entry>...],  region_replacements[].provider
//    graph_transforms: [<entry>...],     graph_transforms[].provider
//    render_strategy: [<entry>...]}      render_strategy.provider
//   <entry> = {name, source: "engine" | "game", provider_version}
// sorted by name. A game provider that shadows the engine's under one name is
// the one listed. Read from the registries at each call: a game logic reload
// that registers or releases providers shows in the next answer.
nlohmann::ordered_json registeredProvidersJson();

} // namespace Pelican
