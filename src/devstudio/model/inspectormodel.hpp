#pragma once

#include "project.hpp"

#include <schema.hpp>

#include <nlohmann/json.hpp>

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
    bool addComponent(std::string_view key);
    bool addComponent(std::string_view key, const nlohmann::json &declaration);
    void abortActivePreview();
    void undo();
    void redo();
    bool saveScene();
    // Scene revisions are independent of the current selection. The outliner
    // also feeds them here so edits by another client are noticed with nothing
    // selected. Reset only after an explicitly accepted document switch.
    void observeSceneRevision(std::uint64_t revision);
    void resetSceneSaveState();
    bool hasUnsavedChanges() const noexcept;
    bool sceneSaveInFlight() const noexcept;
    std::uint64_t sceneSaveCompletion() const noexcept;
    bool lastSceneSaveSucceeded() const noexcept;

    const std::optional<OutlinerObjectKey> &selection() const noexcept;
    const std::optional<InspectorObjectSnapshot> &snapshot() const noexcept;
    const InspectorNotice &notice() const noexcept;
    std::uint64_t contentRevision() const noexcept;
    bool refreshBlocked() const noexcept;
    bool busy() const noexcept;
    bool canEdit() const noexcept;
    bool canUndoRedo() const noexcept;
    bool canSave() const noexcept;
};

} // namespace PelicanStudio
