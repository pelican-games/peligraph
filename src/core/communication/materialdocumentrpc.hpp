#pragma once

#include <string_view>

namespace Pelican {

class PathResolver;
class RpcServer;

namespace watch {
class ReloadService;
}

// MT-1: material documents (`pelican.material`), their surfaces' schema and
// the models' material bindings, read and written over RPC so an editor never
// writes a project file itself. Every write goes through the project document
// transaction with the digest it was read with, and is applied to the running
// engine before the call returns, or answered restart_required.
//
// Reads:
//   list_materials {} ->
//     {asset_data_digest, documents: [{path, material_digest | null,
//       status: "loaded" | "missing" | "invalid", reason?,
//       materials: [{name, surface | null, tags, id, registered}]}]}
//     Every asset_data materials[] entry, in its order (list_assets lists
//     only entries with a name, which a material entry has not). `id` is the
//     draw-loop key get_render_authoring_context's materials[] carry;
//     `registered`: the running engine holds the material.
//   get_material {path, name?} ->
//     {path, material_digest, name, names, registered, material: {...}}
//     `material` is the material object as the document holds it (name,
//     tags, surface, values, textures, routing, variants, ...); `name` may be
//     left out when the document holds one material.
//   get_surface_schema {surface} ->
//     {surface, params: [{name, type, default, min?, max?, hint?, encoding}],
//      textures: [{name, default, color_space, role, dimension}]}
//     As the surface's `//!` header declares them, nothing added.
//   list_surfaces {} ->
//     {surfaces: [{reference, source: "engine" | "project", name?,
//       params: <count>, textures: <count>, routing?: {alpha_mode,
//       double_sided}, render_state?: {blend, cull, depth_test, depth_write,
//       depth_compare}, error?}]}
//     MT-1b: the engine's registered `surfaces/**.surface` resources
//     (engine://, sorted), then every `*.surface` under the project
//     directory (project://, sorted; hidden and symlinked directories are not
//     entered). Each is read as get_surface_schema reads it; one that does not
//     read or parse carries `error` (and no counts) instead of being dropped.
//     `name` and `routing`: the engine surface manifest's variant (OpenPBR's
//     manifest.json), only for a surface it lists; `routing` is a material's
//     routing as create_material takes it. `render_state`: the header's
//     render_state, only when it declares one.
//   get_material_bindings {model} ->
//     {model, material_bindings: path | null, bindings_digest | null,
//      primitives: [{mesh, primitive, slot, material, usd_path | null}],
//      objects, object_overrides}
//     MT-2: `objects` counts the scene objects (every scene of the document
//     the editor holds, prefab-generated ones included) whose simplemodelview
//     draws the model; `object_overrides` those of them with their own
//     materials (set_object_material).
//
// Writes ({committed, changed, ..., applied: "live" | "restart_required",
// restart_required: {reason} | null}):
//   edit_material {base_material_digest, path, name?, values?, textures?,
//                  tags?, routing?, variants?, surface?}
//     values / textures: {param: value | null} merged into the material's
//     (null removes the override); tags / routing / variants: the whole value
//     (null removes the key); surface: the reference. -> adds path, name,
//     material_digest, previous_material_digest and previous (each key given,
//     with what it held before: a value, or null for absent -- the same call
//     with `previous` and the new digest takes the edit back). A change of
//     values alone is applied live through the material values reload
//     (MaterialContainer::prepareSurfaceMaterialReload); any other key is
//     written and answered restart_required.
//   create_material {path, surface, name?, values?, tags?, routing?}
//     writes a new one-material document and lists it in asset_data
//     materials[] (one transaction), then registers it live
//     (ProjectMaterialAssetContainer::registerDocument). `name` defaults to
//     the file name without `.material.json` / `.json`; `routing` to
//     {alpha_mode: opaque, double_sided: false} (a binding needs one).
//   bind_material {model, material, primitive?: {mesh, primitive} | slot?}
//   bind_material {model, bindings: [{mesh, primitive, material}]}
//     writes the model's pelican.material_bindings document (made, and named
//     from asset_data, when the model has none; an engine://primitives/
//     reference not declared there is declared under its own reference) and
//     rebuilds the model live through a model reload. With neither primitive
//     nor slot every primitive is bound. -> previous: {bindings: [{mesh,
//     primitive, material}]} (send it as `bindings` to take it back).
//
// Refusals (application errors, data {error_code, error_fields}; nothing is
// written): external_modification {path, expected, actual},
// material.value_invalid@1 {path, param, reason},
// material.param_unknown@1 {path, param, kind: "value" | "texture"},
// material.document_unknown@1 {path, reason}, material.document_invalid@1
// {path, reason}, material.name_unknown@1 {name, reason},
// material.name_invalid@1 {name, reason}, material.name_conflict@1 {name,
// path? | model?}, material.path_invalid@1 {path, reason},
// material.path_conflict@1 {path}, material.surface_unknown@1 {surface,
// reason}, material.model_unknown@1 {model, reason},
// material.binding_invalid@1 {model, reason},
// material.transaction_busy@1 {path}.
void configureMaterialDocumentRpcHandlers(RpcServer &server,
                                          PathResolver &path_resolver,
                                          watch::ReloadService *reload_service);

} // namespace Pelican
