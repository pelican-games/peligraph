#pragma once

#include "project.hpp"

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
    InspectorWidgetKind kind = InspectorWidgetKind::StringInput;
    std::size_t columns = 1;
    std::optional<double> range_min;
    std::optional<double> range_max;
    std::vector<std::string> enum_values;
    std::string unit;
    nlohmann::json value;
    bool authored = false;
    bool value_matches_schema = false;
    std::optional<std::uint64_t> behavior_attachment_handle;
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
};

struct InspectorObjectSnapshot {
    std::string scene_id;
    std::uint64_t scene_revision = 0;
    std::uint64_t authoring_object_id = 0;
    std::size_t declaration_index = 0;
    std::optional<std::string> name;
    std::vector<InspectorComponentSnapshot> components;
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

    bool beginWidgetEdit(std::string_view field_key);
    bool stageWidgetValue(std::string_view field_key,
                          nlohmann::json value);
    bool previewWidgetValue(std::string_view field_key,
                            nlohmann::json value);
    bool finishWidgetEdit(std::string_view field_key,
                          bool commit_staged = true);
    bool commitWidgetValue(std::string_view field_key,
                           nlohmann::json value);
    void abortActivePreview();
    void undo();
    void redo();

    const std::optional<OutlinerObjectKey> &selection() const noexcept;
    const std::optional<InspectorObjectSnapshot> &snapshot() const noexcept;
    const InspectorNotice &notice() const noexcept;
    std::uint64_t contentRevision() const noexcept;
    bool refreshBlocked() const noexcept;
    bool busy() const noexcept;
    bool canEdit() const noexcept;
    bool canUndoRedo() const noexcept;
};

} // namespace PelicanStudio
