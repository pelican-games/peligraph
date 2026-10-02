#pragma once

#include "playstatemodel.hpp"
#include "project.hpp"

#include <schema.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

enum class InspectorWidgetKind : std::uint8_t {
    SignedIntegerDrag,
    UnsignedIntegerDrag,
    FloatingPointDrag,
    BooleanCheckbox,
    EnumCombo,
    StringInput,
    VectorDrag,
    QuaternionDrag,
};

struct InspectorWidgetDescriptor {
    std::string field_key;
    std::string field_name;
    std::string component_slot;
    std::size_t component_index = 0;
    std::string json_pointer;
    Pelican::Schema::FieldDeclaration schema;
    InspectorWidgetKind kind = InspectorWidgetKind::StringInput;
    std::size_t columns = 1;
    std::optional<double> range_min;
    std::optional<double> range_max;
    std::vector<std::string> enum_values;
    std::string unit;
    nlohmann::json value;
    bool authored = false;
    bool value_matches_schema = false;
    bool default_available = false;
    std::optional<std::uint64_t> behavior_attachment_handle;
};

struct InspectorListDescriptor {
    std::string key;
    std::string path;
    nlohmann::json schema;
    nlohmann::json rows = nlohmann::json::array();
};

struct InspectorComponentSnapshot {
    std::string name;
    std::size_t component_index = 0;
    std::string title;
    nlohmann::json authored_json;
    std::optional<nlohmann::json> runtime_json;
    bool editable = false;
    bool pending = false;
    bool schema_available = false;
    std::string read_only_reason;
    std::vector<InspectorWidgetDescriptor> widgets;
    std::vector<InspectorListDescriptor> lists;
    bool generated = false;
    std::optional<std::string> stable_generated_id;
    nlohmann::json generated_source;
};

struct InspectorPrefabParameterSnapshot {
    std::string name;
    nlohmann::json value_resolved;
    std::string source;
    std::optional<nlohmann::json> value_authored;
};

struct InspectorObjectSnapshot {
    std::string scene_id;
    std::uint64_t scene_revision = 0;
    std::uint64_t authoring_object_id = 0;
    std::size_t declaration_index = 0;
    std::optional<std::string> name;
    std::vector<InspectorComponentSnapshot> components;
    std::optional<nlohmann::json> prefab_instance;
    std::vector<InspectorPrefabParameterSnapshot> prefab_parameters;
};

enum class InspectorNoticeKind : std::uint8_t {
    None,
    Information,
    Success,
    Error,
};

struct InspectorNotice {
    InspectorNoticeKind kind = InspectorNoticeKind::None;
    std::string message;
};

struct InspectorRpcRequest {
    std::uint64_t request_id = 0;
    std::string method;
    nlohmann::json params;
};

std::string inspectorJsonPointer(std::string_view schema_field_name);
std::vector<InspectorWidgetDescriptor>
makeInspectorWidgetPlan(const nlohmann::json &component,
                        std::uint64_t authoring_object_id);

// G-2: Outliner "Add object". What a new object starts as: an empty (a
// transform only) or one of the engine's primitive shapes (GP-5).
enum class AddObjectKind : std::uint8_t {
    Empty,
    Cube,
    Plane,
    Sphere,
    // T-1: a Plane laid as a floor -- just under the origin and wide -- so a
    // scene has somewhere to stand without typing a transform.
    Floor,
};

// The model references GP-5 resolves without an asset. Studio lists them
// itself, first, so a blank project has something to choose before the
// engine's own catalogue says anything.
[[nodiscard]] const std::vector<std::string> &enginePrimitiveModelReferences();

// "Empty", "Cube", "Plane", "Sphere", "Floor".
[[nodiscard]] std::string_view addObjectBaseName(AddObjectKind kind);

// Blender's rule: the base name if it is free, else the lowest free
// "<base>_001", "<base>_002", ... Blender writes "Cube.001"; the scene format
// takes object names of [a-zA-Z0-9_] only, so the separator is "_".
[[nodiscard]] std::string uniqueObjectName(std::string_view base,
                                           const std::vector<std::string> &taken);

// The `object` of one `spawn` op: a transform at the origin and, for a
// shape, a simplemodelview naming its engine primitive. A Floor is the
// plane primitive at (0, -0.5, 0), scaled 8 x 1 x 8.
[[nodiscard]] nlohmann::json addObjectDeclaration(AddObjectKind kind,
                                                  const std::string &name);

// T-1: the Inspector shows a transform's rotation as three angles in degrees,
// X then Y then Z (each about the fixed axes, so the rotation is
// Rz * Ry * Rx), and keeps storing the quaternion [x, y, z, w] it always did.
// The angles come back as X and Z in (-180, 180], Y in [-90, 90]; at Y = +-90
// X is 0 and Z carries the rest. A quaternion that is not unit is normalised
// first; a zero one reads as no rotation. The quaternion made from angles has
// w >= 0.
[[nodiscard]] std::array<double, 3>
eulerDegreesFromQuaternion(const std::array<double, 4> &quaternion);
[[nodiscard]] std::array<double, 4>
quaternionFromEulerDegrees(const std::array<double, 3> &degrees);
// True for the field whose value is shown as angles: transform.rotation.
[[nodiscard]] bool isEulerRotationField(const InspectorWidgetDescriptor &descriptor);

// How a built-in model reads in a list: "Cube (built-in)". Empty for a
// reference that is not under engine://primitives/.
[[nodiscard]] std::string builtinModelLabel(std::string_view reference);

struct ModelReferenceChoice {
    std::string value;  // what simplemodelview.model is set to
    std::string label;  // what the list shows
    bool builtin = false;
};

// The model field of a simplemodelview offers these: the three primitives
// first (Studio's own fallback, so they are there before and without the
// engine's catalogue), then the models list_assets reported, then the
// authored value when it is none of those. A primitive the engine also
// reports (GP-5b) is listed once.
[[nodiscard]] bool isModelReferenceField(const InspectorWidgetDescriptor &descriptor);
[[nodiscard]] std::vector<ModelReferenceChoice>
modelReferenceChoices(std::string_view current,
                      const std::vector<std::string> &engine_models = {});

// The model ids of a list_assets result, in its order.
[[nodiscard]] std::vector<std::string> modelAssetIds(const nlohmann::json &list_assets_result);

// Q-6: a component the engine lists but this build (or this scene) cannot
// take. get_component_add_candidates names why with a code
// ("physics.module_unavailable@1"); a candidate whose default merely needs
// input ("A model reference is required.") carries a sentence instead and
// stays addable once the declaration is filled in.
[[nodiscard]] bool isComponentUnavailableCode(std::string_view reason);
// The readable sentence for a code; empty for a code Studio does not know.
// This is Studio's one table of them.
[[nodiscard]] std::string componentUnavailableSentence(std::string_view code);
// What the description shows: "<sentence> (<code>)", or the code alone.
[[nodiscard]] std::string componentUnavailableDescription(std::string_view code);
// The short word after the name in the list: "not in this build",
// "needs a ray layout", or "unavailable" for a code Studio does not know.
[[nodiscard]] std::string componentUnavailableTag(std::string_view code);
// Q-10: a refusal that carries a code of that table, either as its own code
// or inside its message -- a sound the render pipeline refused while
// preparing a graph edit arrives as render_pipeline_preflight_failed with
// "audio.format_unsupported@1 {...}" as the message. Answers the table's
// "<sentence> (<code>)", or empty when neither names a code Studio knows.
[[nodiscard]] std::string knownRefusalDescription(std::string_view code,
                                                  std::string_view message);
// LP-4a: the four view_select refusals (LP-Va) are sentences made from the
// refusal's own fields -- the pass, the view family, the axis, and for a
// value past the axis its value and length:
//   graph.view_select_axis_unknown@1       {pass, family, axis[, value, length]}
//   graph.view_select_runtime_axis@1       {pass, family, axis}
//   graph.view_select_primary_family@1     {pass, family}
//   graph.view_select_needs_target_plan@1  {pass}
// `error` is a rejection's `error` object: the fields sit beside its code
// (the render edit service puts them there), or, for a refusal carried inside
// another's message, in the "<code>: {...}" of that message. Answers
// "<sentence> (<code>)." in Q-10's form, or empty for any other refusal.
[[nodiscard]] std::string viewSelectRefusalDescription(const nlohmann::json &error);
// LP-4f2 (VF-1): render.view_family_extent_mismatch@1 {family, pass, target,
// extent, expected_pass, expected_target, expected_extent, loop_origin?,
// expected_loop_origin?} -- the passes of one view family draw at two sizes.
// An extent reads as "256×256" ([w, h]) or "screen ×1.0" ({output_scale: s}),
// and a pass a loop copied is named with its loop and body node. Read as
// viewSelectRefusalDescription reads its own (which answers this one too).
// Empty for any other refusal.
[[nodiscard]] std::string viewFamilyExtentRefusalDescription(const nlohmann::json &error);
// An extent of that refusal as the sentence shows it.
[[nodiscard]] std::string viewFamilyExtentText(const nlohmann::json &extent);
// LP-4c, decision 9: the graph.loop_* and graph.definition_* refusals, one
// sentence each, made from the refusal's own fields (the loop, the branch,
// the definition, the port, the counts). `error` is read as for view_select:
// the fields beside the code, in `error_fields`, or in the message after the
// code ("<code> {...}" as the engine writes it, or "<code>: {...}"). Answers
// "<sentence> (<code>).", or empty for any other refusal.
[[nodiscard]] std::string graphLoopRefusalDescription(const nlohmann::json &error);
// UX-8, decision 13 (c) (engine CT-1): the engine could not read its
// configuration at start and stopped. Its fatal line ("Pelican fatal error :
// <message>") reads as "The engine stopped: <the code's sentence> in <file>.
// Fix the file (or Reload from disk) to start it again." -- the sentence of
// the first known code the message carries, the file the last .json path it
// names (the project's render documents when it names none). Empty when the
// line carries no code Studio has a sentence for.
[[nodiscard]] std::string engineStoppedSentence(std::string_view fatal_line);
// LP-0d: the graph variant (preview, XR) that refused an edit, named by
// `variant` beside the code; empty when the main graph did. LP-4d2: always --
// graph.loop_variant_missing@1 names the material's variant
// `material_variant` (engine 5e246063), so no refusal's own field is called
// `variant` any more.
[[nodiscard]] std::string graphRefusalVariant(const nlohmann::json &error);

struct ComponentAddRow {
    std::size_t candidate = 0;  // index into the catalog's "candidates"
    std::string key;            // what add_component is asked for
    std::string label;          // "physics — not in this build"
    bool addable = true;        // false: Add and the declaration are off
    std::string blocked;        // the description line when !addable
};

// The Add component list: every candidate once, the addable ones first,
// each group in the catalog's order.
[[nodiscard]] std::vector<ComponentAddRow> componentAddRows(const nlohmann::json &catalog);

// Q-7: what became of an Add object. A spawn asked for while an edit is in
// flight waits for that edit and is sent after it; only one waits.
enum class SpawnOutcome : std::uint8_t {
    Refused,
    Sent,
    Queued,
};

// OL-1: the edits that add, remove or move objects. The Outliner follows each
// to its end: the object it created is selected (by the created_objects the
// committed ticket names, not by searching for a name), and a refusal is said
// in one line under the tree.
enum class SceneEditKind : std::uint8_t {
    Spawn,
    Duplicate,
    Destroy,
    Reparent,
    Undo,
    Redo,
};

struct SceneEditOutcome {
    // Moves with every outcome, so a view can tell a new one from the last.
    std::uint64_t serial = 0;
    SceneEditKind kind = SceneEditKind::Spawn;
    // The authoring id of the object the edit was about; 0 for a spawn, an
    // undo and a redo.
    std::uint64_t subject_id = 0;
    bool committed = false;
    // A refusal's code ("cycle_detected", "physics.module_unavailable@1");
    // empty when committed, or when the transport failed before any answer.
    std::string code;
    // The refusal as the engine (or the transport) worded it.
    std::string message;
    // The committed ticket's created_objects, as the engine sent them.
    nlohmann::json created_objects = nlohmann::json::array();
};

// One line for a refused Outliner edit: `object` is the name of the object the
// edit was about and `target` the parent a reparent aimed at (either may be
// empty). A physics-OFF refusal reads as Q-6's sentence; a code Studio has no
// sentence for reads as the code with the engine's message.
[[nodiscard]] std::string sceneEditRefusalSentence(const SceneEditOutcome &outcome,
                                                   std::string_view object,
                                                   std::string_view target = {});

// Qt-independent state machine for the public editor RPC surface. The view
// dispatches takeRpcRequests() over any transport and feeds the replies back
// through receiveRpcResult()/receiveRpcFailure().
class InspectorModel {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    InspectorModel();
    ~InspectorModel();

    InspectorModel(const InspectorModel &) = delete;
    InspectorModel &operator=(const InspectorModel &) = delete;

    void startSession();
    void stopSession(std::string message = {});
    void selectObject(std::optional<OutlinerObjectKey> selection);
    void requestRefresh();

    std::vector<InspectorRpcRequest> takeRpcRequests();
    void receiveRpcResult(std::uint64_t request_id,
                          std::string_view result_json);
    void receiveRpcFailure(std::uint64_t request_id, std::string message);
    void pollPendingOperations();
    void pollExternalChanges();
    // Whether either poll has anything to ask. A panel with no edit in flight
    // and nothing selected has nothing to ask about, and then nothing needs to
    // wake up 25 times a second to find that out.
    [[nodiscard]] bool pendingPollWanted() const noexcept;
    [[nodiscard]] bool externalWatchWanted() const noexcept;

    bool beginWidgetEdit(std::string_view field_key);
    bool stageWidgetValue(std::string_view field_key,
                          nlohmann::json value);
    bool previewWidgetValue(std::string_view field_key,
                            nlohmann::json value);
    bool finishWidgetEdit(std::string_view field_key,
                          bool commit_staged = true);
    bool commitWidgetValue(std::string_view field_key,
                           nlohmann::json value);
    bool canInsertField(std::string_view field_key) const;
    bool insertField(std::string_view field_key);
    bool canRemoveComponent(std::size_t component_index) const noexcept;
    bool canEditList(std::string_view key) const;
    bool requestRayOccurrences();
    const std::optional<nlohmann::json> &rayOccurrences() const noexcept;
    bool addRayOccurrenceOverride(std::string_view list_key, std::string_view occurrence_id);
    bool insertListRow(std::string_view key, nlohmann::json row);
    bool removeListRow(std::string_view key, std::uint32_t row_id);
    bool moveListRow(std::string_view key, std::uint32_t row_id,
                     std::optional<std::uint32_t> before);
    bool editListRow(std::string_view key, std::uint32_t row_id, nlohmann::json row);
    bool removeComponent(std::size_t component_index);
    bool canRequestComponentAddCandidates() const noexcept;
    bool requestComponentAddCandidates();
    const std::optional<nlohmann::json> &componentAddCandidates() const noexcept;
    // G-2: the model ids list_assets last reported (asked when an object
    // with a simplemodelview is loaded); the revision moves with each answer.
    const std::optional<std::vector<std::string>> &modelAssets() const noexcept;
    std::uint64_t modelAssetsRevision() const noexcept;
    bool addComponent(std::string_view key);
    bool addComponent(std::string_view key, const nlohmann::json &declaration);
    // G-2: one `edit` carrying one `spawn` op, on the scene revision last
    // observed. Needs no selection. Q-7: while another edit is in flight the
    // spawn waits and goes out, on the revision that edit left, as soon as
    // the Inspector is idle (takeRpcRequests sends it). One waits at a time:
    // a second is refused with the reason in notice().
    SpawnOutcome spawnObject(const std::string &scene_id, nlohmann::json object);
    // The name of the object waiting to be added, if one is.
    std::optional<std::string> queuedSpawnName() const;
    // OL-1: one `edit` carrying one duplicate, destroy or reparent op from the
    // Outliner, on the scene revision last observed, through this session's
    // actor and undo stack. Needs no selection. Refused (the reason in
    // notice()) while another edit is in flight or a field is held: unlike a
    // new object, these name an object the edit in flight may be changing.
    SpawnOutcome submitSceneEdit(SceneEditKind kind, std::uint64_t subject_id,
                                 nlohmann::json operation, std::string description);
    // The last spawn, Outliner edit, undo or redo to reach its end.
    const std::optional<SceneEditOutcome> &lastSceneEdit() const noexcept;
    void abortActivePreview();
    void undo();
    void redo();
    bool saveScene();
    // Scene revisions are independent of the current selection. The outliner
    // also feeds them here so edits by another client are noticed with nothing
    // selected. Reset only after an explicitly accepted document switch.
    void observeSceneRevision(std::uint64_t revision);
    // PM-3b. A play session began on a saved scene and Stop kept nothing:
    // the scene it put back is the file's, so the next revision observed
    // past the current one counts as saved (Stop moves the revision on
    // rather than back, design_play_mode 6.3).
    void adoptRestoredRevisionAsSaved();
    void resetSceneSaveState();
    bool hasUnsavedChanges() const noexcept;
    bool sceneSaveInFlight() const noexcept;
    std::uint64_t sceneSaveCompletion() const noexcept;
    bool lastSceneSaveSucceeded() const noexcept;

    const std::optional<OutlinerObjectKey> &selection() const noexcept;
    const std::optional<InspectorObjectSnapshot> &snapshot() const noexcept;
    const InspectorNotice &notice() const noexcept;
    // PM-3b. The scene edits this session committed since the last call
    // (edits, Outliner edits, undo, redo, preview commits), with their
    // transaction ids, for the Play changes panel to name.
    std::vector<CommittedEdit> takeCommittedEdits();
    // The transaction the notice now reports as committed; empty when the
    // notice is about anything else.
    const std::string &noticeTransaction() const noexcept;
    // PM-3b (design_play_mode 12.3). Take from game: read the selected
    // object's Transform as the game has it now and write the same values as
    // one edit, marked Keep. Needs a selected object with an editable
    // Transform and nothing in flight.
    bool canTakeTransformFromGame() const noexcept;
    bool takeTransformFromGame();
    std::uint64_t contentRevision() const noexcept;
    bool refreshBlocked() const noexcept;
    bool busy() const noexcept;
    bool canEdit() const noexcept;
    bool canUndoRedo() const noexcept;
    bool canSave() const noexcept;
};

} // namespace PelicanStudio
