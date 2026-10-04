#pragma once

#include "raylayouttemplate.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

// RTL-2s (a). The ray group layout and its bindings, as the Ray layout panel
// (and the task fields of the Graph tab) read and write them -- engine
// RTL-1a's get_ray_layout / edit_ray_layout, RTL-1b's mode and refusals.
//
// The words on screen: the engine's "ray group layout" is the Ray layout
// (never just "layout": the graph's node placement is set_layout), its
// `groups` Hit groups, `misses` Miss shaders, `ray_types` Ray types,
// `record` Record fields, the bindings Hit group assignments.
//
// Qt-free. The panel reads get_ray_layout and get_render_documents, asks
// this model for a plan (the documents a change writes: the layout, the
// bindings, the procedural records and every ray tracing task that uses the
// names it changes), writes the plan's shader files and sends the plan as
// one edit_batch.

using RayJson = nlohmann::json;

// --- get_ray_layout, read.

// The layout or the bindings document as get_ray_layout answers it.
struct RayLayoutDocument {
    // The root's rendering section names it.
    bool named = false;
    std::string reference;
    std::string digest;
    // The document; null when it is not JSON (or not there).
    RayJson document = nullptr;
    // A parser's refusal, or render_document_syntax@1 / render_document_missing.
    std::string error_code;
    std::string error_message;
    // A document that is not JSON: its text.
    std::string text;
    [[nodiscard]] bool usable() const {
        return named && document.is_object() && error_code.empty();
    }
};

struct RaySelectableMaterial {
    std::string label;
    std::string name;
    // Sent as it came: {document} or {asset, source_material_index}.
    RayJson selector;
};

struct RayProceduralAsset {
    std::string asset;
    // The asset's record now ({field: value}).
    RayJson record_fields = RayJson::object();
};

// A binding no scene row matches (not a refusal).
struct RayUnresolvedBinding {
    std::int64_t index = 0;
    RayJson selector;
    std::string group;
};

struct RayLayoutRead {
    std::string source_digest;
    // "none" / "layout" (the process's mode); nullopt when unset.
    std::optional<std::string> mode;
    RayLayoutDocument layout;
    RayLayoutDocument bindings;
    std::vector<RaySelectableMaterial> materials;
    std::vector<RayProceduralAsset> procedurals;
    std::vector<RayUnresolvedBinding> unresolved;
    // The documents name a layout and the process runs without one (or the
    // other way): written, applied when the engine starts again.
    [[nodiscard]] bool restartPending() const;
};

[[nodiscard]] RayLayoutRead parseRayLayoutRead(const RayJson &get_ray_layout);

// --- Names.

enum class RayNameKind { ray_type, group, miss, record_field };

// What is wrong with `name` as a new (or renamed) entry of `kind`, `taken`
// being the other names of that kind; nullopt when nothing is. The rule is
// the parser's (`^[A-Za-z0-9_]{1,64}$`, no duplicate); a record field may
// not take a name the engine's record uses.
[[nodiscard]] std::optional<std::string> rayNameProblem(RayNameKind kind, std::string_view name,
                                                        const std::vector<std::string> &taken);
// `base`, else base_2, base_3... -- the first `taken` does not hold.
[[nodiscard]] std::string rayFreeName(const std::string &base,
                                      const std::vector<std::string> &taken);

// --- The layout document.

struct RayGroupEntry {
    std::string name;
    // "triangles" or "procedural".
    std::string type = "triangles";
};

struct RayRecordField {
    std::string name;
    // i32, u32, f32, vec2, vec3, vec4.
    std::string type = "f32";
};

[[nodiscard]] const std::vector<std::string> &rayRecordTypes();
// The engine_record fields a layout may ask for.
[[nodiscard]] const std::vector<std::string> &rayEngineRecordFields();
// 0 of a record type: 0 or [0, ...].
[[nodiscard]] RayJson rayRecordZero(const std::string &type);

[[nodiscard]] std::vector<std::string> rayLayoutRayTypes(const RayJson &layout);
[[nodiscard]] std::vector<RayGroupEntry> rayLayoutGroups(const RayJson &layout);
[[nodiscard]] std::vector<std::string> rayLayoutGroupNames(const RayJson &layout);
[[nodiscard]] std::vector<std::string> rayLayoutMisses(const RayJson &layout);
[[nodiscard]] std::vector<RayRecordField> rayLayoutRecord(const RayJson &layout);
[[nodiscard]] std::vector<std::string> rayLayoutEngineRecord(const RayJson &layout);
[[nodiscard]] std::string rayLayoutDefaultGroup(const RayJson &layout);

// Enable hit groups' layout: ray type primary, the triangles group default
// (the default group), miss `miss`, no record fields, and the engine record
// that makes the generated header offer pelicanRayFetchNormal
// (material_index, geometry_index).
[[nodiscard]] RayJson defaultRayLayout();

// --- The ray tracing tasks of the project.

struct RayTracingTask {
    enum class Kind { graph_node, feature };
    Kind kind = Kind::graph_node;
    // The node's id, or the feature task's name.
    std::string name;
    // A graph node's scope: "root" or the definition's name.
    std::string scope = "root";
    // A feature task's document (its reference as get_render_documents
    // lists it) and digest (edit_compute_task's base_task_source_digest).
    std::string document;
    std::string digest;
    // The node's parameters whole, or the feature's task object whole.
    RayJson body = RayJson::object();

    [[nodiscard]] const RayJson &rayTracing() const;
    // The edit that writes `ray_tracing` into the task, as one entry of an
    // edit_batch: edit_graph_document's set_node_parameters for a node,
    // edit_compute_task for a feature's task.
    [[nodiscard]] RayJson edit(const RayJson &ray_tracing) const;
};

// The ray tracing tasks the root's graph (its nodes and every definition's)
// and the project's feature documents hold, from get_render_documents.
[[nodiscard]] std::vector<RayTracingTask> rayTracingTasks(const RayJson &render_documents);
// traced_ray_types / misses / hit_shaders rather than miss / closesthit.
[[nodiscard]] bool rayTracingUsesLayout(const RayJson &ray_tracing);

// --- Shaders.

// A shader reference's file for a stage, under the project root:
// project://shaders/x names shaders/x.rgen for a raygen (and so on); a
// reference with its extension names that file. nullopt for engine:// and
// anything outside the project.
[[nodiscard]] std::optional<std::filesystem::path>
rayShaderFile(const std::filesystem::path &project_root, const std::string &reference,
              std::string_view stage);
[[nodiscard]] std::string_view rayStageExtension(std::string_view stage);

// One shader file a plan writes before it sends its edit. `previous` is
// what was there (nullopt: the file is new) -- written back when the edit
// is refused.
struct RayShaderWrite {
    std::filesystem::path path;
    std::string content;
    std::optional<std::string> previous;
};

// RTL-2s-c: the templates are raylayouttemplate's -- the Graph tab's New...
// and this model's plans write the same ones. The names a layout document
// gives them (an empty set for anything that is not a layout).
[[nodiscard]] RayLayoutTemplateNames rayLayoutTemplateNames(const RayJson &layout);

// RTL-2s-c: the one rule that puts a task's ray tracing in the layout's form,
// for Enable hit groups and for a task the Graph's palette places. `legacy`'s
// raygen stays; its closesthit[k] (a reference or a list) becomes traced ray
// type k (the layout's k-th, in order) with that closest hit in every hit
// group's cell -- a procedural group's with `intersection` too, when given;
// every miss of the layout takes the task's miss j (the last one past its
// list). Every other key is kept.
[[nodiscard]] RayJson rayTracingInLayoutForm(const RayJson &legacy, const RayJson &layout,
                                             const std::string &intersection = {});

// RTL-2s-c: the hit shader files the layout's tasks name in their traced
// cells (closest hit, any hit, intersection), each once.
[[nodiscard]] std::vector<std::filesystem::path>
rayTracedHitShaderFiles(const std::vector<RayTracingTask> &tasks, const std::filesystem::path &project_root);
// What a hit shader of a traced ray type has to change for `layout`'s
// record: its Studio block written again (a RayShaderWrite), or one sentence
// naming it when it declares the record itself (or not at all) and Studio
// cannot write it.
struct RayRecordFit {
    std::vector<RayShaderWrite> writes;
    std::vector<std::string> problems;
};
[[nodiscard]] RayRecordFit fitRayShaderRecords(const std::vector<std::filesystem::path> &files,
                                               const RayJson &layout, const std::filesystem::path &project_root);

// RTL-2s-c: what checking `ray_type` in a task's Traced ray types leaves to
// do, as sentences naming the shaders. The new row takes the first traced
// ray type's cells (`ray_tracing` is the task before the check), whose hit
// shaders answer in that ray type's payload; and a hit shader of a traced
// ray type has to declare the layout's record (fitRayShaderRecords). Empty
// when nothing is left.
[[nodiscard]] std::vector<std::string> rayTracedRowNotes(const RayJson &ray_tracing, const std::string &ray_type,
                                                         const RayJson &layout,
                                                         const std::filesystem::path &project_root);

// Rewrites the literal traceRayEXT routing of a shader written without a
// layout -- sbtRecordOffset k, stride, missIndex j -- into the layout's
// names: PELICAN_RAY_TYPE_<ray_types[k]>, pelican_ray_type_count,
// PELICAN_MISS_<misses[j]>, and includes the generated header. nullopt
// when the shader calls traceRayEXT with anything but integer literals there
// (left as it is; the plan says so) or does not call it at all.
struct RayTraceRewrite {
    std::string content;
    int calls = 0;
};
[[nodiscard]] std::optional<RayTraceRewrite>
rewriteLegacyTraceCalls(const std::string &source, const std::vector<std::string> &ray_types,
                        const std::vector<std::string> &misses, std::string *problem);

// Occurrences of PELICAN_RAY_TYPE_<from> (or PELICAN_MISS_<from>) as whole
// identifiers.
[[nodiscard]] int countMacroUses(const std::string &source, const std::string &macro);
[[nodiscard]] std::string replaceMacroUses(const std::string &source, const std::string &from,
                                           const std::string &to);

// The shader files of the project a rename looks in: shaders/ and every
// file a task names.
[[nodiscard]] std::vector<std::filesystem::path>
rayProjectShaderFiles(const std::filesystem::path &project_root);

// --- Plans.

// Everything one change writes, sent as one edit_batch (or one
// edit_ray_layout when no task changes).
struct RayLayoutPlan {
    // The layout to write; null: leave it; and `remove_layout`: take it out
    // of the root (layout: null).
    RayJson layout = nullptr;
    bool remove_layout = false;
    // The bindings to write; null: leave them; `remove_bindings`: take them
    // out (bindings: null).
    RayJson bindings = nullptr;
    bool remove_bindings = false;
    // {asset: record} for each procedural asset whose record changes.
    RayJson procedural_records = nullptr;
    // Each task that changes, by index into the tasks the plan was made
    // from, and its new ray_tracing.
    std::vector<std::pair<std::size_t, RayJson>> tasks;
    std::vector<RayShaderWrite> shaders;
    // What the plan did that the author should hear about (a shader it
    // could not rewrite, the names it made).
    std::vector<std::string> notes;
    // The one line a confirmation asks with; empty when none is needed.
    std::string confirm;
    // The history's label.
    std::string label;
    // nullopt when the plan can be sent; otherwise why not.
    std::optional<std::string> problem;

    [[nodiscard]] bool empty() const {
        return layout.is_null() && !remove_layout && bindings.is_null() && !remove_bindings &&
               procedural_records.is_null() && tasks.empty();
    }
};

// The request a plan is sent as: {method, params}. edit_batch when tasks
// change (base_source_digest on the batch), edit_ray_layout otherwise.
struct RayLayoutRequest {
    std::string method;
    RayJson params;
};
[[nodiscard]] RayLayoutRequest rayLayoutRequest(const RayLayoutRead &read,
                                                const std::vector<RayTracingTask> &tasks,
                                                const RayLayoutPlan &plan);

// How a new cell of the hit shader grid (or a new miss) gets its shader.
enum class RayFillMode { copy, from_template };

// Decision 2: Enable hit groups. The default layout (more ray types and
// misses when a task's arrays hold more), each task in the layout's form
// (miss[j] -> misses{miss_j}, closesthit[k] -> traced ray type k with the
// default group's closest hit), and the shaders: Studio's own starting set
// replaced by the layout's (new files beside it), any other raygen's literal
// trace routing rewritten into a new file beside it. The files before stay
// as they were, so Undo (the render history's restore of the root) puts the
// task back on them.
[[nodiscard]] RayLayoutPlan planEnableHitGroups(const RayLayoutRead &read,
                                                const std::vector<RayTracingTask> &tasks,
                                                const std::filesystem::path &project_root);
// Disable: the layout taken out and each task back in the form without
// one; offered only when every task can go back (one hit group, no any hit
// or intersection, shaders that do not use the generated header -- or the
// files they were made from beside them). `problem` says what stays.
[[nodiscard]] RayLayoutPlan planDisableHitGroups(const RayLayoutRead &read,
                                                 const std::vector<RayTracingTask> &tasks,
                                                 const std::filesystem::path &project_root);

// A ray type: every task traces it too. Copied, its row is the first traced
// one's cells; from the template, a closest hit of that ray type for each
// hit group (a procedural group's intersection copied).
[[nodiscard]] RayLayoutPlan planAddRayType(const RayLayoutRead &read,
                                           const std::vector<RayTracingTask> &tasks,
                                           const std::string &name,
                                           const std::filesystem::path &project_root = {},
                                           RayFillMode mode = RayFillMode::copy);
[[nodiscard]] RayLayoutPlan planAddGroup(const RayLayoutRead &read,
                                         const std::vector<RayTracingTask> &tasks,
                                         const std::filesystem::path &project_root,
                                         const std::string &name, const std::string &type,
                                         RayFillMode mode);
[[nodiscard]] RayLayoutPlan planAddMiss(const RayLayoutRead &read,
                                        const std::vector<RayTracingTask> &tasks,
                                        const std::filesystem::path &project_root,
                                        const std::string &name, RayFillMode mode);
[[nodiscard]] RayLayoutPlan planRemoveRayType(const RayLayoutRead &read,
                                              const std::vector<RayTracingTask> &tasks,
                                              const std::string &name);
[[nodiscard]] RayLayoutPlan planRemoveGroup(const RayLayoutRead &read,
                                            const std::vector<RayTracingTask> &tasks,
                                            const std::string &name);
[[nodiscard]] RayLayoutPlan planRemoveMiss(const RayLayoutRead &read,
                                           const std::vector<RayTracingTask> &tasks,
                                           const std::string &name);
enum class RayEntryKind { ray_type, group, miss };
// The names in the layout, every task and the bindings; a ray type or a
// miss in the shaders too (`shader_files`: where to look).
[[nodiscard]] RayLayoutPlan planRename(const RayLayoutRead &read,
                                       const std::vector<RayTracingTask> &tasks,
                                       RayEntryKind kind, const std::string &from,
                                       const std::string &to,
                                       const std::vector<std::filesystem::path> &shader_files);
// One place up (-1) or down (+1).
[[nodiscard]] RayLayoutPlan planMove(const RayLayoutRead &read, RayEntryKind kind,
                                     const std::string &name, int by);
[[nodiscard]] RayLayoutPlan planSetDefaultGroup(const RayLayoutRead &read, const std::string &name);
[[nodiscard]] RayLayoutPlan planSetGroupType(const RayLayoutRead &read,
                                             const std::vector<RayTracingTask> &tasks,
                                             const std::string &name, const std::string &type);
// Record fields: every material row and every procedural asset's record
// with it (a new field's value 0). RTL-2s-c: and the record block of every
// hit shader the tasks trace with, written again (fitRayShaderRecords).
[[nodiscard]] RayLayoutPlan planAddRecordField(const RayLayoutRead &read, const std::string &name,
                                               const std::string &type,
                                               const std::vector<RayTracingTask> &tasks = {},
                                               const std::filesystem::path &project_root = {});
[[nodiscard]] RayLayoutPlan planRemoveRecordField(const RayLayoutRead &read,
                                                  const std::string &name,
                                                  const std::vector<RayTracingTask> &tasks = {},
                                                  const std::filesystem::path &project_root = {});
[[nodiscard]] RayLayoutPlan planSetEngineRecord(const RayLayoutRead &read,
                                                const std::vector<std::string> &fields,
                                                const std::vector<RayTracingTask> &tasks = {},
                                                const std::filesystem::path &project_root = {});

// --- Hit group assignments (bindings).

// The bindings document now, or an empty one.
[[nodiscard]] RayJson rayBindingsDocument(const RayLayoutRead &read);
// The material row of `selector`; nullopt when none.
[[nodiscard]] std::optional<std::size_t> rayBindingIndex(const RayJson &bindings,
                                                         const RayJson &selector);
// Assign `group` to the material `selector` (its record kept, or zeros);
// an empty group takes the row out.
[[nodiscard]] RayLayoutPlan planAssignMaterial(const RayLayoutRead &read, const RayJson &selector,
                                               const std::string &group);
[[nodiscard]] RayLayoutPlan planSetMaterialRecord(const RayLayoutRead &read,
                                                  const RayJson &selector,
                                                  const std::string &field, const RayJson &value);
[[nodiscard]] RayLayoutPlan planAssignProcedural(const RayLayoutRead &read,
                                                 const std::string &asset,
                                                 const std::string &group);
[[nodiscard]] RayLayoutPlan planSetProceduralRecord(const RayLayoutRead &read,
                                                    const std::string &asset,
                                                    const std::string &field,
                                                    const RayJson &value);
// Takes out the row at `index` (an unresolved one).
[[nodiscard]] RayLayoutPlan planRemoveBinding(const RayLayoutRead &read, std::size_t index);

// Decision 7: the Undo of an edit that rewrote procedural records (asset
// data is not a render document: restore_render_documents does not take it
// back) -- the layout, the bindings and the records as they were, sent
// again with edit_ray_layout.
[[nodiscard]] RayLayoutPlan planPutBack(const RayLayoutRead &before, const RayLayoutRead &now);

// A material's label for a selector (the engine's, else the selector).
[[nodiscard]] std::string raySelectorLabel(const RayLayoutRead &read, const RayJson &selector);
// Compares a document reference as the engine spells it in a refusal
// (`passes/x.json` for the root, project://features/... for a feature) with
// another spelling of it.
[[nodiscard]] bool sameRenderDocument(std::string_view left, std::string_view right);

} // namespace PelicanStudio
