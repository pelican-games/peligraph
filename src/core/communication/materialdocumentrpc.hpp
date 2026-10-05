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
//       status: "loaded" | "missing" | "invalid", reason?, line?, column?,
//       materials: [{name, surface | null, tags, id, registered}]}]}
//     FO-1: `status` is the engine's for a document it refused at start
//     (ProjectMaterialAssetContainer::refusedDocument: none of its materials
//     are registered until the engine starts again, or EB-2 until
//     register_material {path} or the file watch takes it in), with its
//     reason and,
//     for a document that is not JSON, the line and column; otherwise it is
//     the file as it reads now.
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
//       double_sided}, routings: [{alpha_mode, double_sided}],
//       render_state?: {blend, cull, depth_test, depth_write,
//       depth_compare}, error?}]}
//     MT-1b: the engine's registered `surfaces/**.surface` resources
//     (engine://, sorted), then every `*.surface` under the project
//     directory (project://, sorted; hidden and symlinked directories are not
//     entered). Each is read as get_surface_schema reads it; one that does not
//     read or parse carries `error` (and no counts) instead of being dropped.
//     `name`: the engine surface manifest's variant (OpenPBR's
//     manifest.json), only for a surface it lists. EB-6: `routings` every
//     routing a material on the surface is drawn with, as create_material
//     takes it -- the one the manifest pairs the surface with, or, for a
//     surface no manifest lists, each its render_state and hooks take
//     (one-sided before double-sided, opaque, mask, blend; [] when none,
//     e.g. no pelican_lighting_v1) -- and `routing` the first of them,
//     create_material's default (absent when there is none).
//     `render_state`: the header's render_state, only when it declares one.
//   get_material_bindings {model} ->
//     {model, material_bindings: path | null, bindings_digest | null,
//      primitives: [{mesh, primitive, slot, material, usd_path | null,
//                    unresolved?: {material, reason, drawn}}],
//      objects, object_overrides}
//     EB-6: `unresolved` when the primitive's bound `material` is a name no
//     registered document holds (FO-1's material.binding_unresolved@1): it
//     draws `drawn`, the model's own material, instead; `reason` says why
//     (the refused document holding the name and its status, when known).
//     MT-2: `objects` counts the scene objects (every scene of the document
//     the editor holds, prefab-generated ones included) whose simplemodelview
//     draws the model; `object_overrides` those of them with their own
//     materials (set_object_material).
//
// Writes ({committed, changed, ..., applied: "live" | "rebuilt" |
// "restart_required" | "unusable", restart_required: {reason} | null,
// problem?}):
//   EB-6: "unusable" -- the documents are written and the engine goes on,
//   but a pass that draws the material does not declare what its surface
//   uses (a scene acceleration structure query, ray_query): the document is
//   list_materials' "unusable" and get_problems lists
//   material.pass_contract_unmet@1 (source edit_rpc) until the pass declares
//   it; the next frame after that registers it (and rebuilds the models
//   binding it). `problem`: {code, message, document?, document_kind?, node?,
//   material, material_document, pass, passes?, requirement, reason}, the
//   listed problem's fields with its code and where. create_material and
//   register_material (a new document) answer it; edit_material and
//   rename_material when the registration anew is refused so (the models
//   keep drawing the previous registration until then); bind_material when a
//   material bound is such a document's (the model takes the bindings, its
//   primitives keep their own material until then).
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
//     that followed is in `previous` like a sent one. EB-6: `surface` alone
//     on a surface no manifest lists keeps the material's routing when the
//     surface takes it, and otherwise takes list_surfaces' `routing` for it. Both sent are taken as
//     they are; a routing whose pipeline state is not the surface's is
//     refused, material.document_invalid@1.
//   create_material {path, surface, name?, values?, tags?, routing?}
//     writes a new one-material document and lists it in asset_data
//     materials[] (one transaction), then registers it live
//     (ProjectMaterialAssetContainer::registerDocument). `name` defaults to
//     the file name without `.material.json` / `.json`; `routing` to
//     list_surfaces' `routing` for the surface (EB-6), or {alpha_mode:
//     opaque, double_sided: false} when it has none (a binding needs one).
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
//     EB-2: a listed document the engine refused at start (list_materials
//     status "invalid" / "missing" from the engine) is read again as the
//     file is now instead: refused by the parser's code when it still does
//     not read (material.document_unknown@1 / material.document_invalid@1
//     {path, reason}; it stays refused and listed in get_problems), and
//     otherwise registered (ProjectMaterialAssetContainer::recoverDocument):
//     its problem and the material.binding_unresolved@1 warnings of the
//     models it resolves leave the list, and those models are rebuilt in
//     place. Nothing is written: the answer has recovered: true,
//     rebuilt_models: [model], and asset_data_digest ==
//     previous_asset_data_digest; restart_required when the models could
//     not be rebuilt in place (the document stays registered).
//     EB-6: a listed document the engine holds is read again from its file
//     (a person's own editor wrote it; no watch did): checked as
//     create_material checks a document -- refused by the parser's code,
//     material.document_unknown@1 / material.document_invalid@1 {path,
//     reason}, nothing changing -- and registered anew as an edit outside
//     values is (MT-3): {committed, reloaded: true, path, names,
//     material_digest, asset_data_digest == previous_asset_data_digest,
//     applied: "rebuilt" | "unusable" | "restart_required", rebuilt_models}.
//     Nothing is written. material.path_conflict@1 is left for a listed
//     document this engine neither holds nor refused.
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
