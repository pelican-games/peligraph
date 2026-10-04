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
//     each material by its name, EB-1). `id` is the
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
// Writes ({committed, changed, ..., applied: "live" | "rebuilt" |
// "restart_required", restart_required: {reason} | null}):
//   edit_material {base_material_digest, path, name?, values?, textures?,
//                  tags?, routing?, variants?, surface?}
//     values / textures: {param: value | null} merged into the material's
//     (null removes the override); tags / routing / variants: the whole value
//     (null removes the key); surface: the reference. -> adds path, name,
//     material_digest, previous_material_digest and previous (each key given,
//     with what it held before: a value, or null for absent -- the same call
//     with `previous` and the new digest takes the edit back). A change of
//     values alone is applied live through the material values reload
//     (MaterialContainer::prepareSurfaceMaterialReload). MT-3: a change of
//     any other key is applied "rebuilt": the document's materials are
//     registered anew (ProjectMaterialAssetContainer::reregisterDocument)
//     and the models drawing the previous registration rebuilt in place
//     (rebuilt_models: [model]); restart_required only when this engine did
//     not register the material, or the new registration or the rebuild
//     fails (reason says which; the document stays written).
//     MT-3: a routing change is carried into every material_bindings entry
//     naming the material, written in the same transaction
//     (rewritten_bindings: [{model, material_bindings, bindings_digest,
//     previous_bindings_digest}], [] when none); a routing taken off a
//     bound material is refused, material.routing_conflict@1.
//     VR-1: an engine surface manifest (OpenPBR's) pairs each surface it
//     lists with a routing; the half of that pair not sent follows the half
//     sent, in the same edit: `surface` alone takes its manifest routing
//     (when the material holds a routing), `routing` alone takes the surface
//     of that routing from the manifest of the material's surface. The key
//     that followed is in `previous` like a sent one. Both sent are taken as
//     they are; a routing whose pipeline state is not the surface's is
//     refused, material.document_invalid@1.
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
//     MT-2b: when every primitive ends on the material its model file (or
//     engine primitive) gives it -- an engine primitive's
//     pelican.primitive.default included -- nothing is written: the
//     bindings document is erased and asset_data's material_bindings taken
//     off (with the engine primitive's models[] entry the bind made), and the
//     answer's material_bindings and bindings_digest are null.
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
// material.transaction_busy@1 {path}, material.routing_conflict@1 {path,
// material, routing: null, bound_by: [{model, routing}]}.
//
// MI-1: model files the editor has put in the project, registered and taken
// back over RPC (the editor copies the file; the engine only lists it).
//   register_model {path, name?} ->
//     {committed, name, path, asset_data_digest, previous_asset_data_digest,
//      applied: "live" | "restart_required", restart_required: {reason} | null}
//     `path`: project-relative (project:// is taken off), a .glb / .gltf /
//     .vrm file in the project; `name` defaults to the file name's stem.
//     The file is read as a models[] entry is read at start before anything
//     is written; then {name, path} is appended to asset_data models[] (one
//     transaction, on the digest the handler read) and the model registered
//     live (ModelAssetContainer::registerModel).
//   unregister_model {name} ->
//     {committed, name, removed: {name, path, material_bindings?},
//      asset_data_digest, previous_asset_data_digest, applied, restart_required}
//     Takes the entry off asset_data models[] (no file is erased) and
//     releases the template; restart_required when a live instance or an
//     animation rig keeps it.
//   Refusals (nothing written): model.path_invalid@1 {path, reason:
//   "empty" | "backslash" | "absolute" | "fragment" | "dot_segment"},
//   model.format_unsupported@1 {path, extension, supported},
//   model.file_missing@1 {path}, model.path_conflict@1 {path, name},
//   model.name_invalid@1 {name, reason: "empty" | "fragment" |
//   "engine_reference" | "engine_primitive"}, model.name_conflict@1 {name},
//   model.load_failed@1 {path, message}, model.unknown@1 {name},
//   model.in_use@1 {name, count, objects}, model.transaction_busy@1 {path},
//   external_modification {path, expected, actual}.
//
// EB-1: a material taken off the project, put back, and renamed.
//   delete_material {name} ->
//     {committed, name, removed: {path}, asset_data_digest,
//      previous_asset_data_digest, applied: "live" | "restart_required",
//      restart_required}
//     Takes the document holding `name` off asset_data materials[] (the
//     document file stays) and releases its registration. Refused while
//     anything names it: material.in_use@1 {name, bindings: [model],
//     objects: [first 8 scene objects with their own material], count
//     (objects), render_documents: [project-relative render documents whose
//     draw loop names "<path>#<name>" or whose ray bindings name the
//     document]}. A document holding several materials is refused,
//     material.document_shared@1 {name, path, names}.
//   register_material {path} ->
//     {committed, path, names, material_digest, asset_data_digest,
//      previous_asset_data_digest, applied, restart_required}
//     Lists a material document already in the project (delete_material's
//     removed.path) in asset_data materials[] and registers it live.
//     material.path_conflict@1 when listed already, material.name_conflict@1
//     when one of its names is held.
//   rename_material {name, new_name} ->
//     {committed, changed, path, name: new_name, previous_name,
//      material_digest, previous_material_digest, rewritten_bindings: [{model,
//      material_bindings, bindings_digest, previous_bindings_digest}],
//      applied: "rebuilt" | "live" | "restart_required", restart_required,
//      rebuilt_models?}
//     Writes the material's name and every material_bindings entry naming
//     it in one transaction, then registers it anew and rebuilds the models
//     drawing it (as edit_material's "rebuilt"). A scene object's own
//     material and a render document are not rewritten: a rename they name
//     is refused, material.in_use@1 (bindings: []). The same call with the
//     names swapped takes it back.
//   For both delete and rename an engine material (pelican.*) is
//   material.name_invalid@1 {reason: "engine_material"}, a model's glTF
//   material {reason: "model_material", model}, and a name no listed
//   document holds material.name_unknown@1.
void configureMaterialDocumentRpcHandlers(RpcServer &server,
                                          PathResolver &path_resolver,
                                          watch::ReloadService *reload_service);

} // namespace Pelican
