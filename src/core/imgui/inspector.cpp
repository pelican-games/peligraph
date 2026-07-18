#include "inspector.hpp"

#include "imguiruntime.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

std::string escapeJsonPointerSegment(std::string_view segment) {
    std::string escaped;
    escaped.reserve(segment.size());
    for (const auto character : segment) {
        if (character == '~') escaped += "~0";
        else if (character == '/') escaped += "~1";
        else escaped.push_back(character);
    }
    return escaped;
}

std::pair<std::optional<double>, std::optional<double>>
numericRange(const StructFieldRange &range) {
    return std::visit(
        [](const auto &bounds)
            -> std::pair<std::optional<double>, std::optional<double>> {
            using Bounds = std::decay_t<decltype(bounds)>;
            if constexpr (std::same_as<Bounds, std::monostate>) {
                return {};
            } else {
                return {static_cast<double>(bounds.first),
                        static_cast<double>(bounds.second)};
            }
        },
        range);
}

bool isDragWidget(InspectorWidgetKind kind) {
    return kind == InspectorWidgetKind::SignedIntegerDrag ||
           kind == InspectorWidgetKind::UnsignedIntegerDrag ||
           kind == InspectorWidgetKind::FloatingPointDrag ||
           kind == InspectorWidgetKind::VectorDrag ||
           kind == InspectorWidgetKind::QuaternionDrag;
}

std::string objectLabel(const EditorObjectQueryResult &object) {
    if (object.name) return *object.name;
    return "<object " + std::to_string(object.authoring_object_id.value) + ">";
}

std::string resultErrorCode(const OrderedJson &result) {
    const auto error = result.find("error");
    if (error == result.end() || !error->is_object()) return {};
    return error->value("code", std::string{});
}

std::string resultErrorMessage(const OrderedJson &result) {
    const auto error = result.find("error");
    if (error == result.end() || !error->is_object()) return "editor command failed";
    return error->value("message", std::string{"editor command failed"});
}

struct WidgetInteraction {
    bool valid = false;
    bool changed = false;
    bool committed = false;
    bool drag = false;
    Json value;
};

WidgetInteraction drawSchemaWidget(const InspectorWidgetDescriptor &widget,
                                   const Json &current) {
    WidgetInteraction result{.drag = isDragWidget(widget.kind), .value = current};
    const auto *label = widget.field_name.c_str();
    switch (widget.kind) {
    case InspectorWidgetKind::SignedIntegerDrag: {
        if (!current.is_number_integer()) break;
        auto value = current.get<std::int64_t>();
        std::int64_t minimum = widget.range_min
                                   ? static_cast<std::int64_t>(*widget.range_min)
                                   : std::numeric_limits<std::int64_t>::lowest();
        std::int64_t maximum = widget.range_max
                                   ? static_cast<std::int64_t>(*widget.range_max)
                                   : std::numeric_limits<std::int64_t>::max();
        result.valid = true;
        result.changed = ImGui::DragScalar(
            label, ImGuiDataType_S64, &value, 1.0F,
            widget.range_min ? &minimum : nullptr,
            widget.range_max ? &maximum : nullptr, "%lld",
            widget.range_min ? ImGuiSliderFlags_AlwaysClamp : 0);
        result.committed = ImGui::IsItemDeactivatedAfterEdit();
        result.value = value;
        break;
    }
    case InspectorWidgetKind::UnsignedIntegerDrag: {
        if (!current.is_number_unsigned() && !current.is_number_integer()) break;
        auto value = current.get<std::uint64_t>();
        std::uint64_t minimum = widget.range_min
                                    ? static_cast<std::uint64_t>(*widget.range_min)
                                    : 0;
        std::uint64_t maximum = widget.range_max
                                    ? static_cast<std::uint64_t>(*widget.range_max)
                                    : std::numeric_limits<std::uint64_t>::max();
        result.valid = true;
        result.changed = ImGui::DragScalar(
            label, ImGuiDataType_U64, &value, 1.0F,
            widget.range_min ? &minimum : nullptr,
            widget.range_max ? &maximum : nullptr, "%llu",
            widget.range_min ? ImGuiSliderFlags_AlwaysClamp : 0);
        result.committed = ImGui::IsItemDeactivatedAfterEdit();
        result.value = value;
        break;
    }
    case InspectorWidgetKind::FloatingPointDrag: {
        if (!current.is_number()) break;
        auto value = current.get<double>();
        const double minimum = widget.range_min.value_or(0.0);
        const double maximum = widget.range_max.value_or(0.0);
        auto speed = 0.1;
        if (widget.range_min && widget.range_max) {
            speed = std::max((*widget.range_max - *widget.range_min) / 200.0,
                             0.0001);
        }
        result.valid = true;
        result.changed = ImGui::DragScalar(
            label, ImGuiDataType_Double, &value, static_cast<float>(speed),
            widget.range_min ? &minimum : nullptr,
            widget.range_max ? &maximum : nullptr, "%.5f",
            widget.range_min ? ImGuiSliderFlags_AlwaysClamp : 0);
        result.committed = ImGui::IsItemDeactivatedAfterEdit();
        result.value = value;
        break;
    }
    case InspectorWidgetKind::VectorDrag:
    case InspectorWidgetKind::QuaternionDrag: {
        if (!current.is_array() || current.size() != widget.columns) break;
        std::array<float, 4> values{};
        bool numeric = true;
        for (std::size_t index = 0; index < widget.columns; ++index) {
            numeric = numeric && current[index].is_number();
            if (numeric) values[index] = current[index].get<float>();
        }
        if (!numeric) break;
        const float minimum = static_cast<float>(widget.range_min.value_or(0.0));
        const float maximum = static_cast<float>(widget.range_max.value_or(0.0));
        auto speed = 0.01F;
        if (widget.range_min && widget.range_max) {
            speed = static_cast<float>(
                std::max((*widget.range_max - *widget.range_min) / 200.0,
                         0.0001));
        }
        result.valid = true;
        result.changed = ImGui::DragScalarN(
            label, ImGuiDataType_Float, values.data(),
            static_cast<int>(widget.columns), speed,
            widget.range_min ? &minimum : nullptr,
            widget.range_max ? &maximum : nullptr, "%.4f",
            widget.range_min ? ImGuiSliderFlags_AlwaysClamp : 0);
        result.committed = ImGui::IsItemDeactivatedAfterEdit();
        result.value = Json::array();
        for (std::size_t index = 0; index < widget.columns; ++index) {
            result.value.push_back(values[index]);
        }
        break;
    }
    case InspectorWidgetKind::BooleanCheckbox: {
        if (!current.is_boolean()) break;
        auto value = current.get<bool>();
        result.valid = true;
        result.changed = ImGui::Checkbox(label, &value);
        result.committed = result.changed;
        result.value = value;
        break;
    }
    case InspectorWidgetKind::EnumCombo: {
        if (!current.is_string()) break;
        const auto selected = current.get<std::string>();
        result.valid = true;
        if (ImGui::BeginCombo(label, selected.c_str())) {
            for (const auto &candidate : widget.enum_values) {
                const bool is_selected = candidate == selected;
                if (ImGui::Selectable(candidate.c_str(), is_selected)) {
                    result.value = candidate;
                    result.changed = true;
                    result.committed = true;
                }
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        break;
    }
    case InspectorWidgetKind::StringInput: {
        if (!current.is_string()) break;
        std::array<char, 1024> buffer{};
        const auto &text = current.get_ref<const std::string &>();
        const auto count = std::min(text.size(), buffer.size() - 1U);
        std::memcpy(buffer.data(), text.data(), count);
        result.valid = true;
        result.changed = ImGui::InputText(label, buffer.data(), buffer.size());
        result.committed = ImGui::IsItemDeactivatedAfterEdit();
        result.value = std::string{buffer.data()};
        break;
    }
    }
    return result;
}

} // namespace

std::string inspectorJsonPointer(std::string_view schema_field_name) {
    std::string result;
    std::string segment;
    const auto flush = [&] {
        if (segment.empty()) return;
        result += "/" + escapeJsonPointerSegment(segment);
        segment.clear();
    };
    for (std::size_t index = 0; index < schema_field_name.size();) {
        const auto character = schema_field_name[index];
        if (character == '.') {
            flush();
            ++index;
            continue;
        }
        if (character == '[') {
            const auto close = schema_field_name.find(']', index + 1U);
            if (close != std::string_view::npos) {
                flush();
                result += "/" + escapeJsonPointerSegment(
                                      schema_field_name.substr(index + 1U,
                                                               close - index - 1U));
                index = close + 1U;
                continue;
            }
        }
        segment.push_back(character);
        ++index;
    }
    flush();
    return result;
}

std::vector<InspectorWidgetDescriptor>
makeInspectorWidgetPlan(const EditorComponentQueryResult &component) {
    std::vector<InspectorWidgetDescriptor> result;
    if (component.schema_state != EditorComponentSchemaState::Available) return result;
    result.reserve(component.schema_fields.size());
    for (const auto &field : component.schema_fields) {
        InspectorWidgetDescriptor widget{
            .field_name = std::string{field.name},
            .json_pointer = (component.name == "behavior" ? std::string{"/params"}
                                                            : std::string{}) +
                            inspectorJsonPointer(field.name),
        };
        const auto [minimum, maximum] = numericRange(field.range);
        widget.range_min = minimum;
        widget.range_max = maximum;
        switch (field.type) {
        case StructFieldType::I8:
        case StructFieldType::I16:
        case StructFieldType::I32:
        case StructFieldType::I64:
            widget.kind = InspectorWidgetKind::SignedIntegerDrag;
            break;
        case StructFieldType::U8:
        case StructFieldType::U16:
        case StructFieldType::U32:
        case StructFieldType::U64:
            widget.kind = InspectorWidgetKind::UnsignedIntegerDrag;
            break;
        case StructFieldType::F32:
        case StructFieldType::F64:
            widget.kind = InspectorWidgetKind::FloatingPointDrag;
            break;
        case StructFieldType::Vec2:
            widget.kind = InspectorWidgetKind::VectorDrag;
            widget.columns = 2;
            break;
        case StructFieldType::Vec3:
            widget.kind = InspectorWidgetKind::VectorDrag;
            widget.columns = 3;
            break;
        case StructFieldType::Vec4:
            widget.kind = InspectorWidgetKind::VectorDrag;
            widget.columns = 4;
            break;
        case StructFieldType::Quat:
            widget.kind = InspectorWidgetKind::QuaternionDrag;
            widget.columns = 4;
            break;
        case StructFieldType::String:
            widget.kind = InspectorWidgetKind::StringInput;
            break;
        case StructFieldType::Bool:
            widget.kind = InspectorWidgetKind::BooleanCheckbox;
            break;
        case StructFieldType::Enum:
            widget.kind = InspectorWidgetKind::EnumCombo;
            for (std::size_t index = 0; index < field.enum_value_count; ++index) {
                widget.enum_values.emplace_back(field.enum_values[index]);
            }
            break;
        }
        result.push_back(std::move(widget));
    }
    return result;
}

bool applyInspectorStaleResult(EditorObjectQueryResult &selected,
                               EditorSceneTreeResult &tree,
                               const OrderedJson &result,
                               std::string_view component_slot) {
    const auto error = result.find("error");
    if (error == result.end() || !error->is_object()) return false;
    const auto payload = error->find("payload");
    if (payload == error->end() || !payload->is_object()) return false;
    bool applied = false;
    if (const auto revision = payload->find("current_revision");
        revision != payload->end() && revision->is_number_integer()) {
        const auto value = revision->get<std::uint64_t>();
        tree.scene_revision = SceneRevision{value};
        selected.scene_revision = SceneRevision{value};
        applied = true;
    }
    const auto target = payload->find("target");
    if (target == payload->end() || !target->is_object()) return applied;
    const auto authored = target->find("authored_component");
    if (authored == target->end()) return applied;
    const auto attachment_index = target->find("attachment_index");
    const auto component = std::find_if(
        selected.components.begin(), selected.components.end(),
        [&](const auto &candidate) {
            return candidate.name == component_slot &&
                   (attachment_index == target->end() ||
                    !attachment_index->is_number_unsigned() ||
                    candidate.component_index ==
                        attachment_index->get<std::size_t>());
        });
    if (component != selected.components.end()) {
        component->authored_json = *authored;
        applied = true;
    }
    return applied;
}

bool InspectorWatchState::advanceFrame() noexcept {
    ++frames_since_poll;
    if (frames_since_poll < inspectorWatchPollFrameInterval) return false;
    frames_since_poll = 0;
    return true;
}

bool InspectorWatchState::differs(
    const EditorSceneRevisionResult &revision) const noexcept {
    return observed && *observed != revision.token;
}

void InspectorWatchState::observe(
    const EditorSceneRevisionResult &revision) noexcept {
    observed = revision.token;
}

EditorSceneTreeResult
InspectorServiceAdapter::sceneTree(const EditorSceneTreeRequest &request) {
    ++trace_.query_calls;
    return service_.sceneTree(request);
}

EditorSceneRevisionResult InspectorServiceAdapter::getSceneRevision() {
    ++trace_.query_calls;
    return service_.getSceneRevision();
}

EditorObjectQueryResult
InspectorServiceAdapter::getComponents(const EditorGetComponentsRequest &request) {
    ++trace_.query_calls;
    return service_.getComponents(request);
}

OrderedJson InspectorServiceAdapter::openEditorSession(const Json &params) {
    return service_.openEditorSession(params);
}

OrderedJson InspectorServiceAdapter::edit(const Json &params) {
    ++trace_.edit_enqueue_calls;
    return service_.edit(params);
}

OrderedJson InspectorServiceAdapter::undo(const Json &params) {
    ++trace_.edit_enqueue_calls;
    return service_.undo(params);
}

OrderedJson InspectorServiceAdapter::redo(const Json &params) {
    ++trace_.edit_enqueue_calls;
    return service_.redo(params);
}

OrderedJson InspectorServiceAdapter::openPreview(const Json &params) {
    ++trace_.edit_enqueue_calls;
    return service_.openPreview(params);
}

OrderedJson InspectorServiceAdapter::updatePreview(const Json &params) {
    ++trace_.edit_enqueue_calls;
    return service_.updatePreview(params);
}

OrderedJson InspectorServiceAdapter::commitPreview(const Json &params) {
    ++trace_.edit_enqueue_calls;
    return service_.commitPreview(params);
}

OrderedJson InspectorServiceAdapter::abortPreview(const Json &params) {
    ++trace_.edit_enqueue_calls;
    return service_.abortPreview(params);
}

OrderedJson InspectorServiceAdapter::getEditResult(const Json &params) {
    ++trace_.query_calls;
    return service_.getEditResult(params);
}

OrderedJson InspectorServiceAdapter::getPreviewResult(const Json &params) {
    ++trace_.query_calls;
    return service_.getPreviewResult(params);
}

SaveSceneResult InspectorServiceAdapter::saveScene() {
    ++trace_.save_calls;
    return service_.saveScene();
}

bool invokeInspectorPanelCallback(const EngineLaunchConfig &config,
                                  InspectorPanelTrace &trace,
                                  const std::function<void()> &callback) {
    return invokeImGuiRuntimeCallback(config, [&] {
        ++trace.panel_callback_calls;
        callback();
    });
}

struct InspectorPanel::Impl {
    struct PendingEdit {
        std::string ticket;
        std::string description;
        std::string component_slot;
    };

    enum class PreviewRequestKind : std::uint8_t { Open, Update, Commit, Abort };

    struct PreviewState {
        std::string field_key;
        std::string component_slot;
        std::string ticket;
        std::string outstanding_request;
        PreviewRequestKind outstanding_kind = PreviewRequestKind::Open;
        OrderedJson latest_operation;
        OrderedJson sent_operation;
        bool dirty = false;
        bool release_requested = false;
        bool abort_requested = false;
    };

    InspectorServiceAdapter commands;
    EditorSceneTreeResult tree;
    std::optional<EditorObjectQueryResult> selected;
    std::optional<AuthoringObjectId> selected_id;
    std::vector<PendingEdit> pending_edits;
    std::optional<PreviewState> preview;
    std::unordered_set<std::string> preview_unavailable;
    InspectorWatchState watch;
    std::uint64_t actor_id = 0;
    std::string message;
    bool message_is_error = false;
    bool initialized = false;

    Impl(EditorCommandService &service, InspectorPanelTrace &trace)
        : commands{service, trace} {}

    void setMessage(std::string text, bool error = false) {
        message = std::move(text);
        message_is_error = error;
    }

    void selectObject(AuthoringObjectId object_id) {
        try {
            selected = commands.getComponents(EditorGetComponentsRequest{
                .scene_id = tree.scene_id.empty()
                                ? std::optional<std::string>{}
                                : std::optional{tree.scene_id},
                .authoring_object_id = object_id,
            });
            selected_id = object_id;
        } catch (const std::exception &error) {
            selected.reset();
            selected_id.reset();
            setMessage(error.what(), true);
        }
    }

    void refresh() {
        try {
            tree = commands.sceneTree();
            auto next = selected_id;
            if (next && std::none_of(tree.objects.begin(), tree.objects.end(),
                                     [&](const auto &object) {
                                         return object.authoring_object_id == *next;
                                     })) {
                next.reset();
            }
            if (!next && !tree.objects.empty()) {
                next = tree.objects.front().authoring_object_id;
            }
            if (next) selectObject(*next);
            else {
                selected.reset();
                selected_id.reset();
            }
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
        }
    }

    void initialize() {
        if (initialized) return;
        initialized = true;
        try {
            const auto session = commands.openEditorSession(
                {{"display_name", "ImGui Inspector"}});
            actor_id = session.at("actor_id").get<std::uint64_t>();
            refresh();
            watch.observe(commands.getSceneRevision());
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
        }
    }

    void pollWatch() {
        try {
            (void)pollInspectorWatch(
                watch, [&] { return commands.getSceneRevision(); },
                [&] { refresh(); });
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
        }
    }

    void applyStaleResult(const OrderedJson &result,
                          std::string_view component_slot) {
        if (selected) {
            (void)applyInspectorStaleResult(*selected, tree, result,
                                            component_slot);
        }
    }

    void reportFailure(const OrderedJson &result, std::string_view description,
                       std::string_view component_slot = {}) {
        const auto code = resultErrorCode(result);
        if (code == "stale_revision") {
            applyStaleResult(result, component_slot);
            setMessage("Value changed elsewhere; current authored value was loaded. Try again.",
                       true);
            return;
        }
        if (code == "undo_conflict") {
            const auto &payload = result.at("error").at("payload");
            setMessage("Undo blocked: " + payload.value("domain", Json{}).dump() +
                           " is owned by " +
                           payload.value("owner_txn", std::string{"another transaction"}) +
                           " at revision " +
                           std::to_string(payload.value("revision", std::uint64_t{})) + ".",
                       true);
            return;
        }
        setMessage(std::string{description} + ": " +
                       (code.empty() ? resultErrorMessage(result)
                                     : code + " - " + resultErrorMessage(result)),
                   true);
    }

    void handleEditResult(const OrderedJson &result, const PendingEdit &pending) {
        const auto status = result.value("status", std::string{});
        if (status == "committed") {
            setMessage(pending.description + " committed at revision " +
                       std::to_string(result.value("committed_revision", std::uint64_t{})) +
                       ".");
            refresh();
        } else {
            reportFailure(result, pending.description, pending.component_slot);
        }
    }

    void pollEdits() {
        for (auto current = pending_edits.begin(); current != pending_edits.end();) {
            try {
                const auto result = commands.getEditResult({{"ticket", current->ticket}});
                if (result.value("status", std::string{}) == "accepted") {
                    ++current;
                    continue;
                }
                auto completed = *current;
                current = pending_edits.erase(current);
                handleEditResult(result, completed);
            } catch (const std::exception &error) {
                setMessage(error.what(), true);
                current = pending_edits.erase(current);
            }
        }
    }

    void enqueueEdit(const OrderedJson &operation, std::string description,
                     std::string component_slot) {
        if (!selected || actor_id == 0) return;
        try {
            const auto response = commands.edit(
                {{"actor_id", actor_id},
                 {"base_revision", selected->scene_revision.value},
                 {"operations", Json::array({operation})},
                 {"coalesce_key", "imgui-inspector"}});
            PendingEdit pending{response.value("ticket", std::string{}),
                                std::move(description),
                                std::move(component_slot)};
            if (response.value("status", std::string{}) == "accepted") {
                pending_edits.push_back(std::move(pending));
                setMessage("Edit accepted; waiting for the frame boundary.");
            } else {
                handleEditResult(response, pending);
            }
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
        }
    }

    void enqueueUndoRedo(bool redo) {
        if (!selected || actor_id == 0) return;
        const auto description = redo ? std::string{"Redo"} : std::string{"Undo"};
        try {
            const auto params = Json{{"actor_id", actor_id},
                                     {"base_revision", selected->scene_revision.value}};
            const auto response = redo ? commands.redo(params) : commands.undo(params);
            PendingEdit pending{response.value("ticket", std::string{}), description, {}};
            if (response.value("status", std::string{}) == "accepted") {
                pending_edits.push_back(std::move(pending));
                setMessage(description + " accepted; waiting for the frame boundary.");
            } else {
                handleEditResult(response, pending);
            }
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
        }
    }

    void saveScene() {
        try {
            const auto saved = commands.saveScene();
            setMessage("Scene saved at revision " +
                       std::to_string(saved.scene_revision.value) + ".");
            refresh();
        } catch (const EditorCommandError &error) {
            setMessage(std::string{editorCommandErrorCodeName(error.code())} +
                           " - " + error.what(),
                       true);
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
        }
    }

    void beginPreview(std::string field_key, std::string component_slot,
                      OrderedJson operation) {
        if (preview || actor_id == 0) return;
        try {
            const auto response = commands.openPreview(
                {{"actor_id", actor_id},
                 {"operations", Json::array({operation})}});
            if (response.value("status", std::string{}) != "accepted") {
                if (resultErrorCode(response) == "method_unavailable") {
                    preview_unavailable.insert(std::move(field_key));
                } else {
                    reportFailure(response, "Preview", component_slot);
                }
                return;
            }
            preview = PreviewState{
                .field_key = std::move(field_key),
                .component_slot = std::move(component_slot),
                .ticket = response.at("ticket").get<std::string>(),
                .outstanding_request = response.at("request_id").get<std::string>(),
                .outstanding_kind = PreviewRequestKind::Open,
                .latest_operation = operation,
                .sent_operation = std::move(operation),
            };
            setMessage("Live preview opening...");
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
        }
    }

    void sendPreviewRequest(PreviewRequestKind kind) {
        if (!preview || !preview->outstanding_request.empty()) return;
        try {
            OrderedJson response;
            if (kind == PreviewRequestKind::Update) {
                response = commands.updatePreview(
                    {{"actor_id", actor_id}, {"ticket", preview->ticket},
                     {"operations", Json::array({preview->latest_operation})}});
                preview->sent_operation = preview->latest_operation;
                preview->dirty = false;
            } else if (kind == PreviewRequestKind::Commit) {
                response = commands.commitPreview(
                    {{"actor_id", actor_id}, {"ticket", preview->ticket}});
            } else {
                response = commands.abortPreview(
                    {{"actor_id", actor_id}, {"ticket", preview->ticket}});
            }
            if (response.value("status", std::string{}) != "accepted") {
                reportFailure(response, "Preview", preview->component_slot);
                preview.reset();
                return;
            }
            preview->outstanding_request =
                response.at("request_id").get<std::string>();
            preview->outstanding_kind = kind;
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
            preview.reset();
        }
    }

    void drivePreview() {
        if (!preview || !preview->outstanding_request.empty()) return;
        if (preview->abort_requested) {
            sendPreviewRequest(PreviewRequestKind::Abort);
        } else if (preview->dirty) {
            sendPreviewRequest(PreviewRequestKind::Update);
        } else if (preview->release_requested) {
            sendPreviewRequest(PreviewRequestKind::Commit);
        }
    }

    void pollPreview() {
        if (!preview || preview->outstanding_request.empty()) return;
        try {
            const auto result = commands.getPreviewResult(
                {{"request_id", preview->outstanding_request}});
            if (result.value("status", std::string{}) == "accepted") return;
            const auto kind = preview->outstanding_kind;
            preview->outstanding_request.clear();
            const auto status = result.value("status", std::string{});
            if (status == "open" || status == "updated") {
                setMessage(status == "open" ? "Live preview active."
                                             : "Live preview updated.");
                drivePreview();
                return;
            }
            if (status == "committed") {
                setMessage("Preview committed at revision " +
                           std::to_string(result.value("committed_revision",
                                                       std::uint64_t{})) +
                           ".");
                preview.reset();
                refresh();
                return;
            }
            if (status == "succeeded" && kind == PreviewRequestKind::Abort) {
                setMessage("Live preview aborted; committed value restored.");
                preview.reset();
                refresh();
                return;
            }
            const auto field_key = preview->field_key;
            const auto operation = preview->latest_operation;
            const auto component_slot = preview->component_slot;
            const bool release = preview->release_requested;
            if (kind == PreviewRequestKind::Open &&
                resultErrorCode(result) == "method_unavailable") {
                preview_unavailable.insert(field_key);
                preview.reset();
                if (release) {
                    enqueueEdit(operation, "Edit " + field_key, component_slot);
                }
                return;
            }
            reportFailure(result, "Preview", component_slot);
            preview.reset();
        } catch (const std::exception &error) {
            setMessage(error.what(), true);
            preview.reset();
        }
    }

    OrderedJson fieldOperation(const EditorComponentQueryResult &component,
                               const InspectorWidgetDescriptor &widget,
                               const Json &value) const {
        OrderedJson result{{"op", "set_component_value"},
                           {"object_id", selected->authoring_object_id.value},
                           {"component_slot", component.name},
                           {"field_path", widget.json_pointer},
                           {"value", value}};
        if (component.name == "behavior") {
            result["attachment_index"] = component.component_index;
            if (component.behavior_attachment_handle) {
                result["attachment_handle"] =
                    *component.behavior_attachment_handle;
            }
        }
        return result;
    }

    void handleWidgetInteraction(EditorComponentQueryResult &component,
                                 const InspectorWidgetDescriptor &widget,
                                 const WidgetInteraction &interaction) {
        const auto field_key = std::to_string(selected->authoring_object_id.value) + ":" +
                               component.name + ":" +
                               std::to_string(component.component_index) + ":" +
                               widget.json_pointer;
        if (interaction.changed) {
            component.authored_json[Json::json_pointer{widget.json_pointer}] =
                interaction.value;
            auto operation = fieldOperation(component, widget, interaction.value);
            if (interaction.drag && !preview_unavailable.contains(field_key)) {
                if (!preview) {
                    beginPreview(field_key, component.name, operation);
                } else if (preview->field_key == field_key) {
                    preview->latest_operation = operation;
                    preview->dirty = preview->latest_operation != preview->sent_operation;
                }
            }
        }

        if (interaction.committed) {
            const auto operation = fieldOperation(component, widget, interaction.value);
            if (interaction.drag && preview && preview->field_key == field_key) {
                preview->latest_operation = operation;
                preview->dirty = preview->latest_operation != preview->sent_operation;
                preview->release_requested = true;
            } else {
                enqueueEdit(operation, "Edit " + component.name + "." +
                                           widget.field_name,
                            component.name);
            }
        }
    }

    void drawTree(bool *open) {
        ImGui::SetNextWindowPos({12.0F, 72.0F}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({300.0F, 620.0F}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Pelican Object Tree", open,
                          ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }
        if (ImGui::Button("Refresh")) refresh();
        ImGui::SameLine();
        ImGui::TextDisabled("scene %s | revision %llu", tree.scene_id.c_str(),
                            static_cast<unsigned long long>(tree.scene_revision.value));
        ImGui::Separator();

        std::optional<AuthoringObjectId> clicked;
        const auto has_children = [&](const EditorObjectQueryResult &object) {
            if (!object.name) return false;
            return std::any_of(tree.objects.begin(), tree.objects.end(),
                               [&](const auto &candidate) {
                                   return candidate.parent == object.name;
                               });
        };
        const auto draw_node = [&](auto &&self,
                                   const EditorObjectQueryResult &object) -> void {
            ImGui::PushID(static_cast<int>(object.authoring_object_id.value));
            auto flags = ImGuiTreeNodeFlags_OpenOnArrow |
                         ImGuiTreeNodeFlags_SpanAvailWidth |
                         ImGuiTreeNodeFlags_DefaultOpen;
            if (!has_children(object)) flags |= ImGuiTreeNodeFlags_Leaf;
            if (selected_id && *selected_id == object.authoring_object_id) {
                flags |= ImGuiTreeNodeFlags_Selected;
            }
            const auto label = objectLabel(object);
            const bool expanded = ImGui::TreeNodeEx(label.c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                clicked = object.authoring_object_id;
            }
            if (expanded) {
                if (object.name) {
                    for (const auto &candidate : tree.objects) {
                        if (candidate.parent == object.name) self(self, candidate);
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        for (const auto &object : tree.objects) {
            const bool parent_exists = object.parent &&
                std::any_of(tree.objects.begin(), tree.objects.end(),
                            [&](const auto &candidate) {
                                return candidate.name == object.parent;
                            });
            if (!parent_exists) draw_node(draw_node, object);
        }
        if (clicked) selectObject(*clicked);
        ImGui::End();
    }

    void drawReadonlyJson(std::string_view label, const Json &value) {
        const auto text = value.dump(2);
        ImGui::TextDisabled("%.*s", static_cast<int>(label.size()), label.data());
        ImGui::PushTextWrapPos();
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
    }

    void drawRuntimeDifference(const EditorComponentQueryResult &component) {
        if (!component.runtime_json) return;
        const auto &runtime = *component.runtime_json;
        if (runtime == component.authored_json) return;
        ImGui::SeparatorText("Runtime difference (read-only)");
        if (runtime.is_object() && runtime.contains("local_trs") &&
            runtime.contains("world_trs")) {
            drawReadonlyJson("local_trs", runtime.at("local_trs"));
            drawReadonlyJson("world_trs", runtime.at("world_trs"));
        } else {
            drawReadonlyJson("runtime_json", runtime);
        }
    }

    void drawComponent(EditorComponentQueryResult &component) {
        ImGui::PushID(static_cast<int>(component.component_index));
        std::string title = component.name;
        if (component.name == "behavior") {
            title += " " + component.authored_json.value("type", std::string{"<unknown>"});
            title += " [index " + std::to_string(component.component_index);
            if (component.behavior_attachment_handle) {
                title += ", handle " +
                         std::to_string(*component.behavior_attachment_handle);
            }
            title += "]";
        }
        if (component.pending) title += " (pending)";
        if (ImGui::CollapsingHeader(title.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (component.pending) {
                ImGui::TextColored({0.95F, 0.70F, 0.25F, 1.0F},
                                   "Pending | game-logic DLL unavailable | read-only");
                drawReadonlyJson("pending params", component.authored_json);
                ImGui::PopID();
                return;
            }
            if (!component.editable) {
                ImGui::TextColored({0.95F, 0.70F, 0.25F, 1.0F},
                                   "Read-only | %s",
                                   component.codec_state == ComponentCodecState::Missing
                                       ? "codec missing"
                                       : "codec is not editable");
                drawReadonlyJson("authored_json", component.authored_json);
                drawRuntimeDifference(component);
                ImGui::PopID();
                return;
            }
            if (component.schema_state != EditorComponentSchemaState::Available) {
                ImGui::TextColored({1.0F, 0.45F, 0.35F, 1.0F},
                                   "Read-only | schema missing");
                drawReadonlyJson("authored_json", component.authored_json);
                ImGui::PopID();
                return;
            }
            for (const auto &widget : makeInspectorWidgetPlan(component)) {
                try {
                    const auto pointer = Json::json_pointer{widget.json_pointer};
                    const auto &current = component.authored_json.at(pointer);
                    const auto interaction = drawSchemaWidget(widget, current);
                    if (!interaction.valid) {
                        ImGui::TextColored({1.0F, 0.45F, 0.35F, 1.0F},
                                           "%s: authored value does not match schema",
                                           widget.field_name.c_str());
                        continue;
                    }
                    handleWidgetInteraction(component, widget, interaction);
                } catch (const std::exception &) {
                    ImGui::TextDisabled("%s: not authored",
                                        widget.field_name.c_str());
                }
            }
            drawRuntimeDifference(component);
        }
        ImGui::PopID();
    }

    void drawInspector(bool *open) {
        ImGui::SetNextWindowPos({326.0F, 72.0F}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({520.0F, 620.0F}, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Pelican Inspector", open,
                          ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }
        const bool busy = !pending_edits.empty() || preview.has_value();
        if (busy) ImGui::BeginDisabled();
        if (ImGui::Button("Save")) saveScene();
        ImGui::SameLine();
        if (ImGui::Button("Undo")) enqueueUndoRedo(false);
        ImGui::SameLine();
        if (ImGui::Button("Redo")) enqueueUndoRedo(true);
        if (busy) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Ctrl+S / Ctrl+Z / Ctrl+Y");

        if (!busy && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
            saveScene();
        }
        if (!busy && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
            enqueueUndoRedo(false);
        }
        if (!busy && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
            enqueueUndoRedo(true);
        }
        if (preview && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            preview->abort_requested = true;
        }

        if (!message.empty()) {
            const ImVec4 color = message_is_error
                                     ? ImVec4{1.0F, 0.42F, 0.35F, 1.0F}
                                     : ImVec4{0.45F, 0.85F, 0.55F, 1.0F};
            ImGui::TextColored(color, "%s", message.c_str());
        }
        ImGui::TextDisabled("Scene files are not hot-reloaded automatically.");
        ImGui::Separator();
        if (!selected) {
            ImGui::TextDisabled("Select an object in the Object Tree.");
            ImGui::End();
            return;
        }
        ImGui::Text("%s", objectLabel(*selected).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("object %llu | revision %llu",
                            static_cast<unsigned long long>(
                                selected->authoring_object_id.value),
                            static_cast<unsigned long long>(
                                selected->scene_revision.value));
        if (selected->parent) {
            ImGui::TextDisabled("parent: %s", selected->parent->c_str());
        }
        ImGui::Separator();
        for (auto &component : selected->components) drawComponent(component);
        ImGui::End();
    }

    void draw(bool *tree_open, bool *inspector_open) {
        initialize();
        pollWatch();
        pollEdits();
        pollPreview();
        if (tree_open && *tree_open) drawTree(tree_open);
        if (inspector_open && *inspector_open) drawInspector(inspector_open);
        drivePreview();
    }
};

InspectorPanel::InspectorPanel(EditorCommandService &service,
                               InspectorPanelTrace &trace)
    : impl{std::make_unique<Impl>(service, trace)} {}

InspectorPanel::~InspectorPanel() = default;

void InspectorPanel::draw(bool *tree_open, bool *inspector_open) {
    impl->draw(tree_open, inspector_open);
}

} // namespace Pelican
