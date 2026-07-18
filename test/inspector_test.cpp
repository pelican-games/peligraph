#include <catch2/catch_test_macros.hpp>

#include "../src/core/imgui/inspector.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

Json inspectorFixture() {
    return Json::parse(R"json({
      "schema":"pelican.scene",
      "version":1,
      "scenes":{"main":{"objects":[
        {"name":"Root","components":[
          {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
          {"name":"light","type":"directional","direction":[0,-1,0],"intensity":1,"color":[1,1,1]},
          {"name":"collider","shape":"sphere","radius":0.5}
        ]},
        {"name":"ReadOnly","parent":"Root","components":[
          {"name":"future_component","payload":{"kept":true}}
        ]}
      ]}}
    })json");
}

class DocumentTarget final : public EditorProjectionDocumentTarget {
  public:
    AuthoringSceneDocument document{AuthoringSceneDocument::load(
        inspectorFixture().dump(), SceneRevision{1})};

    const AuthoringSceneDocument &projectionDocument() const override {
        return document;
    }
    SceneRevision nextProjectionRevision() const override {
        return SceneRevision{document.revision().value + 1U};
    }
    void publishProjectionDocument(AuthoringSceneDocument &&next) noexcept override {
        document.swap(next);
    }
};

class PreviewTarget final : public EditorProjectionDocumentTarget {
  public:
    AuthoringSceneDocument document;

    explicit PreviewTarget(const AuthoringSceneDocument &source)
        : document{source.stage(source.rawJson(),
                                SceneRevision{source.revision().value + 1U})} {}

    const AuthoringSceneDocument &projectionDocument() const override {
        return document;
    }
    SceneRevision nextProjectionRevision() const override {
        return SceneRevision{document.revision().value + 1U};
    }
    void publishProjectionDocument(AuthoringSceneDocument &&next) noexcept override {
        document.swap(next);
    }
};

struct ServiceHarness {
    DocumentTarget target;
    std::unique_ptr<EditorCommandService> service;

    ServiceHarness() {
        service = std::make_unique<EditorCommandService>(
            EditorCommandServiceDependencies{
                .document = [this]() -> const AuthoringSceneDocument & {
                    return target.document;
                },
                .current_scene_id = [] { return std::string{"main"}; },
                .edit = EditorEditRuntimeDependencies{
                    .document = [this]() -> const AuthoringSceneDocument & {
                        return target.document;
                    },
                    .current_scene_id = [] { return std::string{"main"}; },
                    .execute = [this](const EditorEditExecutionRequest &request) {
                        EditorProjectionTransaction transaction{target,
                                                                request.base_revision};
                        std::vector<EditorProjectionAdapter *> adapters;
                        return transaction.commit(request.commands, adapters);
                    },
                    .execute_preview =
                        [this](const EditorPreviewExecutionRequest &request) {
                            PreviewTarget preview{target.document};
                            EditorProjectionTransaction transaction{
                                preview, preview.document.revision()};
                            std::vector<EditorProjectionAdapter *> adapters;
                            return transaction.commit(request.commands, adapters);
                        },
                    .gate = [] { return EditorGateObservation{}; },
                },
                .save_scene = [this] {
                    auto next = target.document.stage(
                        target.document.rawJson(),
                        SceneRevision{target.document.revision().value + 1U});
                    const auto revision = next.revision();
                    const auto bytes = next.encodeSemantic().size();
                    target.document.swap(next);
                    return SaveSceneResult{
                        .scene_revision = revision,
                        .digest = {.algorithm = "sha256",
                                   .hex = std::string(64, 'b')},
                        .byte_count = bytes,
                        .scene_hot_reload = false,
                    };
                },
            });
    }
};

EditorComponentQueryResult &componentNamed(EditorObjectQueryResult &object,
                                           std::string_view name) {
    const auto found = std::find_if(
        object.components.begin(), object.components.end(),
        [&](const auto &component) { return component.name == name; });
    REQUIRE(found != object.components.end());
    return *found;
}

const EditorComponentQueryResult &componentNamed(
    const EditorObjectQueryResult &object, std::string_view name) {
    const auto found = std::find_if(
        object.components.begin(), object.components.end(),
        [&](const auto &component) { return component.name == name; });
    REQUIRE(found != object.components.end());
    return *found;
}

} // namespace

TEST_CASE("WP164 enum labels are exposed in declaration order with deterministic query bytes",
          "[imgui][inspector][schema][enum][wp164]") {
    ServiceHarness harness;
    const auto first = harness.service->getComponents(
        {.name = std::string{"Root"}});
    const auto second = harness.service->getComponents(
        {.name = std::string{"Root"}});
    REQUIRE(editorQueryJson(first).dump() == editorQueryJson(second).dump());

    const auto &light = componentNamed(first, "light");
    const auto light_json = editorQueryJson(light);
    const auto &fields = light_json.at("schema").at("fields");
    const auto type = std::find_if(fields.begin(), fields.end(), [](const auto &field) {
        return field.at("name") == "type";
    });
    REQUIRE(type != fields.end());
    REQUIRE(type->at("enum") == Json::array({"directional", "point", "spot"}));
    const auto intensity = std::find_if(fields.begin(), fields.end(), [](const auto &field) {
        return field.at("name") == "intensity";
    });
    REQUIRE(intensity != fields.end());
    REQUIRE_FALSE(intensity->contains("enum"));
}

TEST_CASE("WP164 widget planning is schema-only and covers every inspector field kind",
          "[imgui][inspector][schema][widgets][wp164]") {
    ServiceHarness harness;
    const auto object = harness.service->getComponents(
        {.name = std::string{"Root"}});
    const auto &light = componentNamed(object, "light");
    const auto plan = makeInspectorWidgetPlan(light);
    REQUIRE(plan.size() == light.schema_fields.size());
    REQUIRE(plan.front().kind == InspectorWidgetKind::EnumCombo);
    REQUIRE(plan.front().enum_values ==
            std::vector<std::string>{"directional", "point", "spot"});
    REQUIRE(plan.front().json_pointer == "/type");
    const auto intensity = std::find_if(plan.begin(), plan.end(), [](const auto &widget) {
        return widget.field_name == "intensity";
    });
    REQUIRE(intensity != plan.end());
    REQUIRE(intensity->kind == InspectorWidgetKind::FloatingPointDrag);
    REQUIRE(intensity->range_min == 0.0);
    REQUIRE(intensity->range_max == 1.0e12);

    auto future_component_name = light;
    future_component_name.name = "component_added_after_wp164";
    const auto future_plan = makeInspectorWidgetPlan(future_component_name);
    REQUIRE(future_plan.size() == plan.size());
    for (std::size_t index = 0; index < plan.size(); ++index) {
        REQUIRE(future_plan[index].field_name == plan[index].field_name);
        REQUIRE(future_plan[index].json_pointer == plan[index].json_pointer);
        REQUIRE(future_plan[index].kind == plan[index].kind);
        REQUIRE(future_plan[index].columns == plan[index].columns);
        REQUIRE(future_plan[index].enum_values == plan[index].enum_values);
    }

    REQUIRE(inspectorJsonPointer("sprite.pixel_perfect") ==
            "/sprite/pixel_perfect");
    REQUIRE(inspectorJsonPointer("flip[1]") == "/flip/1");

    const auto &transform = componentNamed(object, "transform");
    const auto transform_plan = makeInspectorWidgetPlan(transform);
    REQUIRE(transform_plan[0].kind == InspectorWidgetKind::VectorDrag);
    REQUIRE(transform_plan[0].columns == 3);
    REQUIRE(transform_plan[1].kind == InspectorWidgetKind::QuaternionDrag);
    REQUIRE(transform_plan[1].columns == 4);

    EditorComponentQueryResult synthetic{
        .name = "synthetic",
        .editable = true,
        .schema_state = EditorComponentSchemaState::Available,
        .schema_fields = {
            StructFieldSchema{"signed", StructFieldType::I32, {}},
            StructFieldSchema{"unsigned", StructFieldType::U32, {}},
            StructFieldSchema{"flag", StructFieldType::Bool, {}},
            StructFieldSchema{"text", StructFieldType::String, {}},
        },
    };
    const auto synthetic_plan = makeInspectorWidgetPlan(synthetic);
    REQUIRE(synthetic_plan[0].kind == InspectorWidgetKind::SignedIntegerDrag);
    REQUIRE(synthetic_plan[1].kind == InspectorWidgetKind::UnsignedIntegerDrag);
    REQUIRE(synthetic_plan[2].kind == InspectorWidgetKind::BooleanCheckbox);
    REQUIRE(synthetic_plan[3].kind == InspectorWidgetKind::StringInput);
}

TEST_CASE("WP164 UI and RPC adapters preserve query edit undo and preview results",
          "[imgui][inspector][fake-service][equivalence][wp164]") {
    ServiceHarness rpc_harness;
    ServiceHarness ui_harness;
    EditorCommandRpcAdapter rpc{*rpc_harness.service};
    InspectorPanelTrace trace;
    InspectorServiceAdapter ui{*ui_harness.service, trace};

    REQUIRE(rpc.sceneTree(Json::object()).dump() ==
            editorQueryJson(ui.sceneTree()).dump());
    const auto rpc_session = rpc.openEditorSession({{"display_name", "fixture"}});
    const auto ui_session = ui.openEditorSession({{"display_name", "fixture"}});
    REQUIRE(rpc_session.at("actor_id") == ui_session.at("actor_id"));
    const auto actor = ui_session.at("actor_id").get<std::uint64_t>();

    const Json edit_operation{{"op", "set_component_value"},
                              {"object_id", 1},
                              {"component_slot", "light"},
                              {"field_path", "/intensity"},
                              {"value", 2.0}};
    const Json edit_params{{"actor_id", actor},
                           {"base_revision", 1},
                           {"operations", Json::array({edit_operation})},
                           {"coalesce_key", "fixture"}};
    const auto rpc_edit = rpc.edit(edit_params);
    const auto ui_edit = ui.edit(edit_params);
    REQUIRE(rpc_edit.dump() == ui_edit.dump());
    rpc_harness.service->commitPendingEdits();
    ui_harness.service->commitPendingEdits();
    REQUIRE(rpc.getEditResult({{"ticket", rpc_edit.at("ticket")}}).dump() ==
            ui.getEditResult({{"ticket", ui_edit.at("ticket")}}).dump());

    auto stale_display = ui_harness.service->getComponents(
        {.name = std::string{"Root"}});
    stale_display.scene_revision = SceneRevision{1};
    auto &stale_light = componentNamed(stale_display, "light");
    stale_light.authored_json["intensity"] = -1.0;
    EditorSceneTreeResult stale_tree{.scene_revision = SceneRevision{1},
                                     .scene_id = "main"};
    auto stale_operation = edit_operation;
    stale_operation["value"] = 9.0;
    const Json stale_params{{"actor_id", actor},
                            {"base_revision", 1},
                            {"operations", Json::array({stale_operation})}};
    const auto rpc_stale = rpc.edit(stale_params);
    const auto ui_stale = ui.edit(stale_params);
    REQUIRE(rpc_stale.dump() == ui_stale.dump());
    REQUIRE(rpc_stale.at("error").at("code") == "stale_revision");
    REQUIRE(applyInspectorStaleResult(stale_display, stale_tree, ui_stale,
                                      "light"));
    REQUIRE(stale_display.scene_revision.value == 2);
    REQUIRE(stale_tree.scene_revision.value == 2);
    REQUIRE(componentNamed(stale_display, "light")
                .authored_json.at("intensity") == 2.0);

    const Json undo_params{{"actor_id", actor}, {"base_revision", 2}};
    const auto rpc_undo = rpc.undo(undo_params);
    const auto ui_undo = ui.undo(undo_params);
    REQUIRE(rpc_undo.dump() == ui_undo.dump());
    rpc_harness.service->commitPendingEdits();
    ui_harness.service->commitPendingEdits();
    REQUIRE(rpc.getEditResult({{"ticket", rpc_undo.at("ticket")}}).dump() ==
            ui.getEditResult({{"ticket", ui_undo.at("ticket")}}).dump());

    const Json preview_operation{{"op", "set_component_value"},
                                 {"object_id", 1},
                                 {"component_slot", "light"},
                                 {"field_path", "/intensity"},
                                 {"value", 4.0}};
    const Json open_params{{"actor_id", actor},
                           {"operations", Json::array({preview_operation})}};
    const auto rpc_open = rpc.openPreview(open_params);
    const auto ui_open = ui.openPreview(open_params);
    REQUIRE(rpc_open.dump() == ui_open.dump());
    rpc_harness.service->commitPendingEdits();
    ui_harness.service->commitPendingEdits();
    REQUIRE(rpc.getPreviewResult({{"request_id", rpc_open.at("request_id")}}).dump() ==
            ui.getPreviewResult({{"request_id", ui_open.at("request_id")}}).dump());

    auto update_operation = preview_operation;
    update_operation["value"] = 5.0;
    const Json update_params{{"actor_id", actor},
                             {"ticket", ui_open.at("ticket")},
                             {"operations", Json::array({update_operation})}};
    const auto rpc_update = rpc.updatePreview(update_params);
    const auto ui_update = ui.updatePreview(update_params);
    REQUIRE(rpc_update.dump() == ui_update.dump());
    rpc_harness.service->commitPendingEdits();
    ui_harness.service->commitPendingEdits();
    REQUIRE(rpc.getPreviewResult({{"request_id", rpc_update.at("request_id")}}).dump() ==
            ui.getPreviewResult({{"request_id", ui_update.at("request_id")}}).dump());

    const Json commit_params{{"actor_id", actor}, {"ticket", ui_open.at("ticket")}};
    const auto rpc_commit = rpc.commitPreview(commit_params);
    const auto ui_commit = ui.commitPreview(commit_params);
    REQUIRE(rpc_commit.dump() == ui_commit.dump());
    rpc_harness.service->commitPendingEdits();
    ui_harness.service->commitPendingEdits();
    REQUIRE(rpc.getPreviewResult({{"request_id", rpc_commit.at("request_id")}}).dump() ==
            ui.getPreviewResult({{"request_id", ui_commit.at("request_id")}}).dump());

    const Json unavailable_operation{{"op", "set_component_value"},
                                     {"object_id", 1},
                                     {"component_slot", "collider"},
                                     {"field_path", "/radius"},
                                     {"value", 1.0}};
    const Json unavailable_params{
        {"actor_id", actor},
        {"operations", Json::array({unavailable_operation})}};
    const auto rpc_unavailable = rpc.openPreview(unavailable_params);
    const auto ui_unavailable = ui.openPreview(unavailable_params);
    REQUIRE(rpc_unavailable.dump() == ui_unavailable.dump());
    REQUIRE(rpc_unavailable.at("error").at("code") == "method_unavailable");

    REQUIRE(trace.query_calls == 6);
    REQUIRE(trace.edit_enqueue_calls == 7);
}

TEST_CASE("deterministic drivers execute zero inspector callbacks queries and enqueues",
          "[imgui][inspector][isolation][wp164]") {
    ServiceHarness harness;
    std::vector<EngineLaunchConfig> isolated(5);
    isolated[0].headless = true;
    isolated[1].rpc = true;
    isolated[2].input_replay = true;
    isolated[3].golden_mode = true;
    isolated[4].xr_active = true;

    for (const auto &config : isolated) {
        InspectorPanelTrace trace;
        InspectorServiceAdapter ui{*harness.service, trace};
        REQUIRE_FALSE(invokeInspectorPanelCallback(config, trace, [&] {
            (void)ui.sceneTree();
        }));
        REQUIRE(trace.panel_callback_calls == 0);
        REQUIRE(trace.query_calls == 0);
        REQUIRE(trace.edit_enqueue_calls == 0);
        REQUIRE(trace.save_calls == 0);
    }

    EngineLaunchConfig interactive;
    InspectorPanelTrace trace;
    InspectorServiceAdapter ui{*harness.service, trace};
    REQUIRE(invokeInspectorPanelCallback(interactive, trace, [&] {
        (void)ui.sceneTree();
    }));
    REQUIRE(trace.panel_callback_calls == 1);
    REQUIRE(trace.query_calls == 1);
    REQUIRE(trace.edit_enqueue_calls == 0);
    REQUIRE(trace.save_calls == 0);
}

TEST_CASE("WP166 interactive Save uses the typed service and matches RPC",
          "[imgui][inspector][save][equivalence][wp166]") {
    ServiceHarness rpc_harness;
    ServiceHarness ui_harness;
    EditorCommandRpcAdapter rpc{*rpc_harness.service};
    InspectorPanelTrace trace;
    InspectorServiceAdapter ui{*ui_harness.service, trace};

    const auto rpc_result = rpc.saveScene(Json::object());
    const auto ui_result = ui.saveScene();
    REQUIRE(rpc_result.dump() == editorQueryJson(ui_result).dump());
    REQUIRE(rpc_result.at("scene_hot_reload") == false);
    REQUIRE(trace.save_calls == 1);
    REQUIRE(trace.query_calls == 0);
    REQUIRE(trace.edit_enqueue_calls == 0);
}

TEST_CASE("WP166 Save is busy for pending tickets and an open preview lease",
          "[imgui][inspector][save][busy][wp166]") {
    ServiceHarness harness;
    const auto session = harness.service->openEditorSession(
        {{"display_name", "save-busy-fixture"}});
    const auto actor = session.at("actor_id").get<std::uint64_t>();
    const Json operation{{"op", "set_component_value"},
                         {"object_id", 1},
                         {"component_slot", "light"},
                         {"field_path", "/intensity"},
                         {"value", 3.0}};

    const auto require_save_busy = [&] {
        std::optional<EditorCommandErrorCode> code;
        try {
            (void)harness.service->saveScene();
        } catch (const EditorCommandError &error) {
            code = error.code();
        }
        REQUIRE(code == EditorCommandErrorCode::SaveBusy);
    };

    const auto edit = harness.service->edit(
        {{"actor_id", actor},
         {"base_revision", 1},
         {"operations", Json::array({operation})}});
    REQUIRE(edit.at("status") == "accepted");
    require_save_busy();
    harness.service->commitPendingEdits();
    REQUIRE(harness.service->saveScene().scene_revision.value == 3);

    const auto preview = harness.service->openPreview(
        {{"actor_id", actor}, {"operations", Json::array({operation})}});
    REQUIRE(preview.at("status") == "accepted");
    require_save_busy();
    harness.service->commitPendingEdits();
    require_save_busy();

    const auto abort = harness.service->abortPreview(
        {{"actor_id", actor}, {"ticket", preview.at("ticket")}});
    REQUIRE(abort.at("status") == "accepted");
    harness.service->commitPendingEdits();
    REQUIRE(harness.service->saveScene().scene_revision.value == 4);
}

} // namespace Pelican
