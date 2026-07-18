#pragma once

#include "../communication/editorcommandservice.hpp"
#include "../launchconfig.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct InspectorPanelTrace {
    std::uint64_t panel_callback_calls = 0;
    std::uint64_t query_calls = 0;
    std::uint64_t edit_enqueue_calls = 0;
};

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
    std::string field_name;
    std::string json_pointer;
    InspectorWidgetKind kind = InspectorWidgetKind::StringInput;
    std::size_t columns = 1;
    std::optional<double> range_min;
    std::optional<double> range_max;
    std::vector<std::string> enum_values;
};

std::string inspectorJsonPointer(std::string_view schema_field_name);
std::vector<InspectorWidgetDescriptor>
makeInspectorWidgetPlan(const EditorComponentQueryResult &component);
bool applyInspectorStaleResult(EditorObjectQueryResult &selected,
                               EditorSceneTreeResult &tree,
                               const nlohmann::ordered_json &result,
                               std::string_view component_slot);

class InspectorServiceAdapter {
    EditorCommandService &service_;
    InspectorPanelTrace &trace_;

  public:
    InspectorServiceAdapter(EditorCommandService &service,
                            InspectorPanelTrace &trace)
        : service_{service}, trace_{trace} {}

    EditorSceneTreeResult sceneTree(const EditorSceneTreeRequest &request = {});
    EditorObjectQueryResult getComponents(const EditorGetComponentsRequest &request);
    nlohmann::ordered_json openEditorSession(const nlohmann::json &params);
    nlohmann::ordered_json edit(const nlohmann::json &params);
    nlohmann::ordered_json undo(const nlohmann::json &params);
    nlohmann::ordered_json redo(const nlohmann::json &params);
    nlohmann::ordered_json openPreview(const nlohmann::json &params);
    nlohmann::ordered_json updatePreview(const nlohmann::json &params);
    nlohmann::ordered_json commitPreview(const nlohmann::json &params);
    nlohmann::ordered_json abortPreview(const nlohmann::json &params);
    nlohmann::ordered_json getEditResult(const nlohmann::json &params);
    nlohmann::ordered_json getPreviewResult(const nlohmann::json &params);
};

bool invokeInspectorPanelCallback(const EngineLaunchConfig &config,
                                  InspectorPanelTrace &trace,
                                  const std::function<void()> &callback);

class InspectorPanel {
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    InspectorPanel(EditorCommandService &service, InspectorPanelTrace &trace);
    ~InspectorPanel();

    void draw(bool *tree_open, bool *inspector_open);
};

} // namespace Pelican
