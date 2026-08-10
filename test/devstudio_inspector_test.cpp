#include "inspectormodel.hpp"
#include "gizmomodel.hpp"

#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/container.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/editorprojectiontransaction.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using Json = nlohmann::json;
using PelicanStudio::InspectorModel;
using PelicanStudio::InspectorNoticeKind;
using PelicanStudio::InspectorRpcRequest;
using PelicanStudio::InspectorWidgetDescriptor;
using PelicanStudio::InspectorWidgetKind;
using PelicanStudio::OutlinerObjectKey;

Json componentResult(double position_x = 1.0,
                     std::uint64_t scene_revision = 1) {
    return Json{
        {"scene_revision", scene_revision},
        {"authoring_object_id", 41},
        {"declaration_index", 2},
        {"components",
         Json::array(
             {{{"name", "transform"},
               {"component_index", 0},
               {"authored_json",
                {{"pos", Json::array({position_x, 2.0, 3.0})},
                 {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
                 {"scale", Json::array({1.0, 1.0, 1.0})}}},
               {"runtime_json",
                {{"pos", Json::array({position_x, 2.0, 3.0})},
                 {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
                 {"scale", Json::array({1.0, 1.0, 1.0})}}},
               {"editable", true},
               {"codec", {{"state", "registered"}, {"name", "transform"}}},
               {"schema",
                {{"state", "available"},
                 {"fields",
                  Json::array(
                      {{{"name", "pos"}, {"type", "vec3"}},
                       {{"name", "rotation"}, {"type", "quat"}},
                       {{"name", "scale"}, {"type", "vec3"}}})}}},
               {"pending", false}},
              {{"name", "fixture_flags"},
               {"component_index", 1},
               {"authored_json", {{"enabled", true}}},
               {"editable", true},
               {"codec", {{"state", "registered"}}},
               {"schema",
                {{"state", "available"},
                 {"fields",
                  Json::array(
                      {{{"name", "enabled"}, {"type", "bool"}}})}}},
               {"pending", false}}})},
    };
}

Json sceneTreeResult(std::uint64_t scene_revision = 1) {
    return Json{
        {"scene_revision", scene_revision},
        {"scene_id", "main"},
        {"objects",
         Json::array({{{"authoring_object_id", 7},
                       {"declaration_index", 0}},
                      {{"authoring_object_id", 41},
                       {"declaration_index", 2}}})},
    };
}

struct InspectorHarness {
    InspectorModel model;

    InspectorRpcRequest take(std::string_view method) {
        auto requests = model.takeRpcRequests();
        REQUIRE(requests.size() == 1);
        REQUIRE(requests.front().method == std::string{method});
        return std::move(requests.front());
    }

    void reply(const InspectorRpcRequest &request, const Json &result) {
        model.receiveRpcResult(request.request_id, result.dump());
    }

    void open(double position_x = 1.0) {
        model.selectObject(OutlinerObjectKey{
            .scene_id = "main",
            .declaration_index = 2,
        });
        REQUIRE(model.takeRpcRequests().empty());

        model.startSession();
        const auto session = take("open_editor_session");
        REQUIRE(session.params.at("display_name") ==
                "Pelican Studio Inspector");
        reply(session, {{"actor_id", 9},
                        {"display_name", "Pelican Studio Inspector"}});

        const auto tree = take("scene_tree");
        REQUIRE(tree.params == Json{{"scene_id", "main"}});
        reply(tree, sceneTreeResult());

        const auto components = take("get_components");
        REQUIRE(components.params.at("scene_id") == "main");
        REQUIRE(components.params.at("authoring_object_id") == 41);
        reply(components, componentResult(position_x));

        const auto watch = take("get_scene_revision");
        reply(watch, {{"scene_revision", 1}, {"preview_epoch", 0}});
        REQUIRE(model.takeRpcRequests().empty());
        REQUIRE(model.snapshot());
    }

    const InspectorWidgetDescriptor &field(std::string_view component,
                                           std::string_view name) const {
        REQUIRE(model.snapshot());
        for (const auto &candidate : model.snapshot()->components) {
            if (candidate.name != component) {
                continue;
            }
            const auto found = std::find_if(
                candidate.widgets.begin(), candidate.widgets.end(),
                [name](const InspectorWidgetDescriptor &widget) {
                    return widget.field_name == name;
                });
            REQUIRE(found != candidate.widgets.end());
            return *found;
        }
        FAIL("missing component " << std::string{component});
    }

    void completeRefresh(double position_x, std::uint64_t scene_revision) {
        const auto tree = take("scene_tree");
        reply(tree, sceneTreeResult(scene_revision));
        const auto components = take("get_components");
        reply(components, componentResult(position_x, scene_revision));
        const auto watch = take("get_scene_revision");
        reply(watch, {{"scene_revision", scene_revision},
                      {"preview_epoch", 0}});
    }

    void commitGizmoStylePosition(double position_x,
                                  std::uint64_t committed_revision = 2) {
        const std::string field_key = field("transform", "pos").field_key;
        REQUIRE(model.previewWidgetValue(
            field_key, Json::array({position_x - 0.5, 2.0, 3.0})));
        const auto open = take("open_preview");
        reply(open, {{"status", "accepted"},
                     {"ticket", "preview:1"},
                     {"request_id", "preview-request:open"}});
        model.pollPendingOperations();
        const auto open_result = take("get_preview_result");
        reply(open_result, {{"status", "open"}});

        REQUIRE(model.previewWidgetValue(
            field_key, Json::array({position_x, 2.0, 3.0})));
        const auto update = take("update_preview");
        REQUIRE(update.params.at("operations").at(0).at("value") ==
                Json::array({position_x, 2.0, 3.0}));
        REQUIRE(model.finishWidgetEdit(field_key, true));
        reply(update, {{"status", "accepted"},
                       {"request_id", "preview-request:update"}});
        model.pollPendingOperations();
        const auto update_result = take("get_preview_result");
        reply(update_result, {{"status", "updated"}});

        const auto commit = take("commit_preview");
        reply(commit, {{"status", "accepted"},
                       {"request_id", "preview-request:commit"}});
        model.pollPendingOperations();
        const auto commit_result = take("get_preview_result");
        reply(commit_result,
              {{"status", "committed"},
               {"committed_revision", committed_revision}});
        completeRefresh(position_x, committed_revision);
    }
};

struct PersistenceProject {
    std::filesystem::path root;

    PersistenceProject() {
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("pelican_wp276_gizmo_save_" + std::to_string(suffix));
        std::filesystem::create_directories(root);
    }

    ~PersistenceProject() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

class PersistencePreviewTarget final
    : public Pelican::EditorProjectionDocumentTarget {
    Pelican::AuthoringSceneDocument document_;

  public:
    explicit PersistencePreviewTarget(
        const Pelican::AuthoringSceneDocument &source)
        : document_{source.stage(
              source.rawJson(),
              Pelican::SceneRevision{source.revision().value + 1U})} {}

    const Pelican::AuthoringSceneDocument &projectionDocument()
        const override {
        return document_;
    }

    Pelican::SceneRevision nextProjectionRevision() const override {
        return Pelican::SceneRevision{document_.revision().value + 1U};
    }

    void publishProjectionDocument(
        Pelican::AuthoringSceneDocument &&document) noexcept override {
        document_.swap(document);
    }
};

void writePersistenceFile(const std::filesystem::path &path,
                          std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output.is_open()) {
        throw std::runtime_error("failed to create persistence fixture: " +
                                 path.string());
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("failed to write persistence fixture: " +
                                 path.string());
    }
}

PelicanStudio::GizmoRpcRequest takeGizmoRpc(
    PelicanStudio::GizmoModel &model, std::string_view method) {
    auto requests = model.takeRpcRequests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().method == std::string{method});
    return std::move(requests.front());
}

Json gizmoEditedPosition() {
    using PelicanStudio::GizmoEditActionKind;
    using PelicanStudio::GizmoEditableField;
    using PelicanStudio::GizmoModel;
    using PelicanStudio::GizmoTransformBinding;

    const OutlinerObjectKey selection{.scene_id = "main",
                                      .declaration_index = 0};
    GizmoModel gizmo;
    gizmo.setSelection(selection);
    gizmo.startSession();
    const auto display = takeGizmoRpc(gizmo, "set_gizmo");
    gizmo.receiveRpcResult(
        display.request_id,
        Json{{"contract", 1},
             {"visible", true},
             {"selection",
              {{"kind", "declaration"},
               {"scene_id", "main"},
               {"declaration_index", 0}}},
             {"mode", "translate"}}
            .dump());

    gizmo.pointerPressed(
        {100, 100},
        GizmoTransformBinding{
            .selection = selection,
            .position = GizmoEditableField{
                .field_key = "1:transform:0:/pos",
                .value = Json::array({1.0, 2.0, 3.0})},
        });
    const auto query = takeGizmoRpc(gizmo, "query_gizmo_handle");
    gizmo.receiveRpcResult(
        query.request_id,
        Json{{"contract", 2},
             {"selection",
              {{"kind", "declaration"},
               {"scene_id", "main"},
               {"declaration_index", 0}}},
             {"mode", "translate"},
             {"coordinate", {{"x", 100}, {"y", 100}}},
             {"extent", {{"width", 640}, {"height", 480}}},
             {"content_scale", 1.0},
             {"grab_radius_pixels", 10.0},
             {"handle",
              {{"id", "translate_x"},
               {"axis", "x"},
               {"drag_direction", {{"x", 1.0}, {"y", 0.0}}},
               {"value_per_logical_pixel", 0.25}}}}
            .dump());
    auto actions = gizmo.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == GizmoEditActionKind::Begin);
    gizmo.confirmEditStarted(actions.front().gesture_id, true);

    gizmo.pointerMoved({112, 100});
    actions = gizmo.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == GizmoEditActionKind::Preview);
    const Json edited = actions.front().value;
    REQUIRE(edited == Json::array({4.0, 2.0, 3.0}));

    gizmo.pointerReleased({112, 100});
    actions = gizmo.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == GizmoEditActionKind::Finish);
    REQUIRE(actions.front().commit);
    return edited;
}

} // namespace

TEST_CASE("Devstudio inspector derives every widget kind from RPC schema",
          "[devstudio][inspector][schema][wp266]") {
    const Json component{
        {"name", "schema_fixture"},
        {"component_index", 3},
        {"authored_json",
         {{"i8", -1},
          {"i16", -2},
          {"i32", -3},
          {"i64", -4},
          {"u8", 1},
          {"u16", 2},
          {"u32", 3},
          {"u64", 4},
          {"f32", 1.25},
          {"f64", 2.5},
          {"flag", true},
          {"choice", "b"},
          {"text", "hello"},
          {"v2", Json::array({1.0, 2.0})},
          {"v3", Json::array({1.0, 2.0, 3.0})},
          {"v4", Json::array({1.0, 2.0, 3.0, 4.0})},
          {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})}}},
        {"schema",
         {{"state", "available"},
          {"fields",
           Json::array(
               {{{"name", "i8"}, {"type", "i8"}},
                {{"name", "i16"}, {"type", "i16"}},
                {{"name", "i32"}, {"type", "i32"}},
                {{"name", "i64"}, {"type", "i64"}},
                {{"name", "u8"}, {"type", "u8"}},
                {{"name", "u16"}, {"type", "u16"}},
                {{"name", "u32"}, {"type", "u32"}},
                {{"name", "u64"}, {"type", "u64"}},
                {{"name", "f32"}, {"type", "f32"}},
                {{"name", "f64"}, {"type", "f64"}},
                {{"name", "flag"}, {"type", "bool"}},
                {{"name", "choice"},
                 {"type", "enum"},
                 {"enum", Json::array({"a", "b"})}},
                {{"name", "text"}, {"type", "string"}},
                {{"name", "v2"}, {"type", "vec2"}},
                {{"name", "v3"}, {"type", "vec3"}},
                {{"name", "v4"}, {"type", "vec4"}},
                {{"name", "rotation"}, {"type", "quat"}}})}}},
    };

    const auto plan = PelicanStudio::makeInspectorWidgetPlan(component, 99);
    REQUIRE(plan.size() == 17);
    const std::vector expected{
        InspectorWidgetKind::SignedIntegerDrag,
        InspectorWidgetKind::SignedIntegerDrag,
        InspectorWidgetKind::SignedIntegerDrag,
        InspectorWidgetKind::SignedIntegerDrag,
        InspectorWidgetKind::UnsignedIntegerDrag,
        InspectorWidgetKind::UnsignedIntegerDrag,
        InspectorWidgetKind::UnsignedIntegerDrag,
        InspectorWidgetKind::UnsignedIntegerDrag,
        InspectorWidgetKind::FloatingPointDrag,
        InspectorWidgetKind::FloatingPointDrag,
        InspectorWidgetKind::BooleanCheckbox,
        InspectorWidgetKind::EnumCombo,
        InspectorWidgetKind::StringInput,
        InspectorWidgetKind::VectorDrag,
        InspectorWidgetKind::VectorDrag,
        InspectorWidgetKind::VectorDrag,
        InspectorWidgetKind::QuaternionDrag,
    };
    for (std::size_t index = 0; index < plan.size(); ++index) {
        REQUIRE(plan[index].kind == expected[index]);
        REQUIRE(plan[index].authored);
        REQUIRE(plan[index].value_matches_schema);
        REQUIRE(plan[index].field_key.starts_with("99:schema_fixture:3:"));
    }
    REQUIRE(plan[13].columns == 2);
    REQUIRE(plan[14].columns == 3);
    REQUIRE(plan[15].columns == 4);
    REQUIRE(plan[16].columns == 4);
    REQUIRE(plan[11].enum_values == std::vector<std::string>{"a", "b"});
    REQUIRE(PelicanStudio::inspectorJsonPointer(
                "params.weights[2].a/b~c") ==
            "/params/weights/2/a~1b~0c");
}

TEST_CASE("Devstudio inspector resolves declaration identity before querying components",
          "[devstudio][inspector][selection][wp266]") {
    InspectorHarness harness;
    harness.open();

    REQUIRE(harness.model.snapshot()->scene_id == "main");
    REQUIRE(harness.model.snapshot()->declaration_index == 2);
    REQUIRE(harness.model.snapshot()->authoring_object_id == 41);
    const auto &position = harness.field("transform", "pos");
    REQUIRE(position.kind == InspectorWidgetKind::VectorDrag);
    REQUIRE(position.value == Json::array({1.0, 2.0, 3.0}));
}

TEST_CASE("Devstudio inspector does not apply an in-flight refresh while a widget is editing",
          "[devstudio][inspector][refresh][preview][wp245][wp266]") {
    InspectorHarness harness;
    harness.open();
    const std::string field_key = harness.field("transform", "pos").field_key;

    harness.model.requestRefresh();
    const auto tree = harness.take("scene_tree");
    harness.reply(tree, sceneTreeResult());
    const auto old_components = harness.take("get_components");

    REQUIRE(harness.model.previewWidgetValue(
        field_key, Json::array({9.0, 2.0, 3.0})));
    const auto open = harness.take("open_preview");
    REQUIRE(harness.model.refreshBlocked());

    // This response was already in flight before editing started. It must be
    // discarded rather than replacing the locally staged value.
    harness.reply(old_components, componentResult(1.0));
    REQUIRE(harness.field("transform", "pos").value ==
            Json::array({9.0, 2.0, 3.0}));
    REQUIRE(harness.model.takeRpcRequests().empty());

    harness.model.pollExternalChanges();
    REQUIRE(harness.model.takeRpcRequests().empty());

    harness.reply(open, {{"status", "accepted"},
                         {"ticket", "preview:1"},
                         {"request_id", "preview-request:open"}});
    harness.model.pollPendingOperations();
    const auto open_result = harness.take("get_preview_result");
    harness.reply(open_result, {{"status", "open"}});

    REQUIRE(harness.model.finishWidgetEdit(field_key, false));
    const auto abort = harness.take("abort_preview");
    harness.reply(abort, {{"status", "accepted"},
                          {"request_id", "preview-request:abort"}});
    harness.model.pollPendingOperations();
    const auto abort_result = harness.take("get_preview_result");
    harness.reply(abort_result, {{"status", "succeeded"}});

    REQUIRE_FALSE(harness.model.refreshBlocked());
    REQUIRE(harness.take("scene_tree").method == "scene_tree");
}

TEST_CASE("Devstudio inspector still reflects an external watch change when idle",
          "[devstudio][inspector][refresh][external][wp245][wp266]") {
    InspectorHarness harness;
    harness.open();

    harness.model.pollExternalChanges();
    const auto watch = harness.take("get_scene_revision");
    harness.reply(watch, {{"scene_revision", 2}, {"preview_epoch", 0}});

    const auto tree = harness.take("scene_tree");
    harness.reply(tree, sceneTreeResult(2));
    const auto components = harness.take("get_components");
    harness.reply(components, componentResult(7.0, 2));

    REQUIRE(harness.field("transform", "pos").value ==
            Json::array({7.0, 2.0, 3.0}));
    REQUIRE(harness.model.snapshot()->scene_revision == 2);
}

TEST_CASE("Devstudio direct edit keeps an in-flight refresh from restoring the old value",
          "[devstudio][inspector][refresh][edit][wp245][wp266]") {
    InspectorHarness harness;
    harness.open();
    const std::string field_key =
        harness.field("fixture_flags", "enabled").field_key;

    harness.model.requestRefresh();
    const auto tree = harness.take("scene_tree");
    harness.reply(tree, sceneTreeResult());
    const auto old_components = harness.take("get_components");

    REQUIRE(harness.model.commitWidgetValue(field_key, false));
    const auto edit = harness.take("edit");
    REQUIRE(harness.model.busy());

    harness.reply(old_components, componentResult());
    REQUIRE(harness.field("fixture_flags", "enabled").value == false);
    REQUIRE(harness.model.takeRpcRequests().empty());

    harness.reply(edit, {{"status", "accepted"}, {"ticket", "edit:1"}});
    harness.model.pollPendingOperations();
    const auto result = harness.take("get_edit_result");
    harness.reply(result,
                  {{"status", "committed"}, {"committed_revision", 2}});

    REQUIRE_FALSE(harness.model.busy());
    REQUIRE(harness.take("scene_tree").method == "scene_tree");
}

TEST_CASE("Devstudio watch sync retries when an external revision lands during refresh",
          "[devstudio][inspector][refresh][external][wp245][wp266]") {
    InspectorHarness harness;
    harness.open();

    harness.model.requestRefresh();
    const auto tree = harness.take("scene_tree");
    harness.reply(tree, sceneTreeResult(2));
    const auto components = harness.take("get_components");
    harness.reply(components, componentResult(2.0, 2));

    const auto watch = harness.take("get_scene_revision");
    harness.reply(watch, {{"scene_revision", 3}, {"preview_epoch", 0}});

    REQUIRE(harness.take("scene_tree").method == "scene_tree");
}

TEST_CASE("Devstudio inspector reports a terminal edit failure instead of accepted as success",
          "[devstudio][inspector][failure][wp244][wp266]") {
    InspectorHarness harness;
    harness.open();
    const std::string field_key =
        harness.field("fixture_flags", "enabled").field_key;

    REQUIRE(harness.model.commitWidgetValue(field_key, false));
    const auto edit = harness.take("edit");
    REQUIRE(edit.params.at("operations").at(0).at("object_id") == 41);
    harness.reply(edit, {{"status", "accepted"}, {"ticket", "edit:1"}});
    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Information);
    REQUIRE(harness.model.notice().kind != InspectorNoticeKind::Success);

    harness.model.pollPendingOperations();
    const auto result = harness.take("get_edit_result");
    harness.reply(result,
                  {{"status", "failed"},
                   {"error",
                    {{"code", "runtime_binding_missing"},
                     {"message", "the edit did not reach the runtime"}}}});
    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Error);
    REQUIRE(harness.model.notice().message.find("runtime_binding_missing") !=
            std::string::npos);
}

TEST_CASE("Devstudio numeric editing drives preview update then commit",
          "[devstudio][inspector][preview][wp266]") {
    InspectorHarness harness;
    harness.open();
    const std::string field_key = harness.field("transform", "pos").field_key;

    REQUIRE(harness.model.previewWidgetValue(
        field_key, Json::array({2.0, 2.0, 3.0})));
    const auto open = harness.take("open_preview");
    harness.reply(open, {{"status", "accepted"},
                         {"ticket", "preview:1"},
                         {"request_id", "preview-request:open"}});
    harness.model.pollPendingOperations();
    const auto open_result = harness.take("get_preview_result");
    harness.reply(open_result, {{"status", "open"}});

    REQUIRE(harness.model.previewWidgetValue(
        field_key, Json::array({3.0, 2.0, 3.0})));
    const auto update = harness.take("update_preview");
    REQUIRE(update.params.at("operations").at(0).at("value") ==
            Json::array({3.0, 2.0, 3.0}));
    REQUIRE(harness.model.finishWidgetEdit(field_key, true));
    harness.reply(update, {{"status", "accepted"},
                           {"request_id", "preview-request:update"}});
    harness.model.pollPendingOperations();
    const auto update_result = harness.take("get_preview_result");
    harness.reply(update_result, {{"status", "updated"}});

    const auto commit = harness.take("commit_preview");
    harness.reply(commit, {{"status", "accepted"},
                           {"request_id", "preview-request:commit"}});
    harness.model.pollPendingOperations();
    const auto commit_result = harness.take("get_preview_result");
    harness.reply(commit_result,
                  {{"status", "committed"}, {"committed_revision", 2}});

    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Success);
    REQUIRE_FALSE(harness.model.refreshBlocked());
    REQUIRE(harness.take("scene_tree").method == "scene_tree");
}

TEST_CASE("Devstudio aborts the live lease after a terminal preview failure",
          "[devstudio][inspector][preview][failure][wp244][wp266]") {
    InspectorHarness harness;
    harness.open();
    const std::string field_key = harness.field("transform", "pos").field_key;

    REQUIRE(harness.model.previewWidgetValue(
        field_key, Json::array({2.0, 2.0, 3.0})));
    const auto open = harness.take("open_preview");
    harness.reply(open, {{"status", "accepted"},
                         {"ticket", "preview:1"},
                         {"request_id", "preview-request:open"}});
    harness.model.pollPendingOperations();
    const auto open_result = harness.take("get_preview_result");
    harness.reply(open_result, {{"status", "open"}});

    REQUIRE(harness.model.previewWidgetValue(
        field_key, Json::array({3.0, 2.0, 3.0})));
    const auto update = harness.take("update_preview");
    REQUIRE(harness.model.finishWidgetEdit(field_key, true));
    harness.reply(update, {{"status", "accepted"},
                           {"request_id", "preview-request:update"}});
    harness.model.pollPendingOperations();
    const auto update_result = harness.take("get_preview_result");
    harness.reply(update_result,
                  {{"status", "failed"},
                   {"error",
                    {{"code", "runtime_binding_missing"},
                     {"message", "preview did not reach the runtime"}}}});

    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Error);
    const auto abort = harness.take("abort_preview");
    harness.reply(abort, {{"status", "accepted"},
                          {"request_id", "preview-request:abort"}});
    harness.model.pollPendingOperations();
    const auto abort_result = harness.take("get_preview_result");
    harness.reply(abort_result, {{"status", "succeeded"}});

    REQUIRE_FALSE(harness.model.busy());
    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Error);
    REQUIRE(harness.model.notice().message.find("runtime_binding_missing") !=
            std::string::npos);
    REQUIRE(harness.take("scene_tree").method == "scene_tree");
}

TEST_CASE("Devstudio undo uses the editor session revision after a gizmo-style preview",
          "[devstudio][inspector][gizmo][undo][wp266][wp275]") {
    InspectorHarness harness;
    harness.open();
    harness.commitGizmoStylePosition(3.0);

    REQUIRE(harness.field("transform", "pos").value ==
            Json::array({3.0, 2.0, 3.0}));

    harness.model.undo();
    const auto undo = harness.take("undo");
    REQUIRE(undo.params == Json{{"actor_id", 9}, {"base_revision", 2}});
    harness.reply(undo,
                  {{"status", "committed"}, {"committed_revision", 3}});

    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Success);
    REQUIRE(harness.take("scene_tree").method == "scene_tree");
}

TEST_CASE("Devstudio save scene RPC is serialized only while idle",
          "[devstudio][inspector][save][rpc][wp275]") {
    InspectorHarness harness;
    harness.open();
    harness.commitGizmoStylePosition(4.0);

    REQUIRE(harness.model.canSave());
    REQUIRE(harness.model.saveScene());
    const auto save = harness.take("save_scene");
    REQUIRE(save.params == Json::object());
    REQUIRE(harness.model.busy());
    REQUIRE_FALSE(harness.model.saveScene());

    harness.reply(save,
                  {{"status", "saved"},
                   {"scene_revision", 2},
                   {"scene_hot_reload", false},
                   {"byte_count", 1234}});
    REQUIRE_FALSE(harness.model.busy());
    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Success);
    REQUIRE(harness.model.notice().message.find("revision 2") !=
            std::string::npos);

    // Saving is scene-wide and must not depend on retaining an Inspector
    // object selection after the gizmo edit was committed.
    harness.model.selectObject(std::nullopt);
    REQUIRE(harness.model.canSave());
}

TEST_CASE("Devstudio gizmo commit survives save and a fresh authoring reload",
          "[devstudio][inspector][gizmo][save][reload][wp276]") {
    Pelican::setupLogger(true);
    PersistenceProject project_dir;
    const Json source{
        {"schema", "pelican.scene"},
        {"version", 1},
        {"scenes",
         {{"main",
           {{"objects",
             Json::array(
                 {{{"name", "Target"},
                   {"components",
                    Json::array(
                        {{{"name", "transform"},
                          {"pos", {1.0, 2.0, 3.0}},
                          {"rotation", {0.0, 0.0, 0.0, 1.0}},
                          {"scale", {1.0, 1.0, 1.0}}}})}}})}}}}},
    };
    writePersistenceFile(project_dir.root / "scene.json", source.dump(2));
    const Json project{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "WP276 persistence"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"default_scene_id", "main"},
          {"scene_data_json", "scene.json"}}},
    };
    const Json edited_position = gizmoEditedPosition();

    {
        Pelican::FastModuleContainer modules;
        Pelican::FastModuleContainer::get<Pelican::PathResolver>()
            .setup(project_dir.root, false);
        Pelican::FastModuleContainer::get<Pelican::ProjectSource>()
            .setProjectData(project.dump());
        auto &config =
            Pelican::FastModuleContainer::get<Pelican::ProjectBasicConfig>();
        const auto &document = config.sceneDocument();
        const auto scenes = document.query();
        REQUIRE(scenes.size() == 1);
        REQUIRE(scenes.front().objects.size() == 1);
        const auto object_id =
            scenes.front().objects.front().authoring_object_id.value;
        const auto base_revision = document.revision();

        Pelican::EditorCommandService service{
            Pelican::EditorCommandServiceDependencies{
                .document = [&config]()
                    -> const Pelican::AuthoringSceneDocument & {
                    return config.sceneDocument();
                },
                .current_scene_id = [] { return std::string{"main"}; },
                .snapshot_state = [] {
                    return Pelican::EditorSnapshotState{};
                },
                .edit = Pelican::EditorEditRuntimeDependencies{
                    .document = [&config]()
                        -> const Pelican::AuthoringSceneDocument & {
                        return config.sceneDocument();
                    },
                    .current_scene_id = [] { return std::string{"main"}; },
                    .execute = [&config](
                                   const Pelican::EditorEditExecutionRequest
                                       &request) {
                        Pelican::ProjectBasicConfigProjectionTarget target{
                            config};
                        Pelican::EditorProjectionTransaction transaction{
                            target, request.base_revision};
                        std::vector<Pelican::EditorProjectionAdapter *>
                            adapters;
                        return transaction.commit(request.commands,
                                                  adapters);
                    },
                    .execute_preview = [&config](
                                           const Pelican::EditorPreviewExecutionRequest
                                               &request) {
                        PersistencePreviewTarget target{
                            config.sceneDocument()};
                        Pelican::EditorProjectionTransaction transaction{
                            target, target.projectionDocument().revision()};
                        std::vector<Pelican::EditorProjectionAdapter *>
                            adapters;
                        return transaction.commit(request.commands,
                                                  adapters);
                    },
                    .gate = [] { return Pelican::EditorGateObservation{}; },
                },
                .save_scene = [&config] {
                    const auto saved = config.saveSceneDocument();
                    return Pelican::SaveSceneResult{
                        .scene_revision = saved.scene_revision,
                        .digest = {.algorithm = "sha256",
                                   .hex = saved.digest},
                        .byte_count = saved.byte_count,
                        .scene_hot_reload = false,
                    };
                },
            }};
        const auto session = service.openEditorSession(
            Json{{"display_name", "WP276 persistence fixture"}});
        const Json operations = Json::array(
            {{{"op", "set_component_value"},
              {"object_id", object_id},
              {"component_slot", "transform"},
              {"field_path", "/pos"},
              {"value", edited_position}}});
        const auto opened = service.openPreview(
            Json{{"actor_id", session.at("actor_id")},
                 {"operations", operations}});
        REQUIRE(opened.at("status") == "accepted");
        service.commitPendingEdits();
        const auto open_result = service.getPreviewResult(
            Json{{"request_id", opened.at("request_id")}});
        REQUIRE(open_result.at("status") == "open");

        const auto commit = service.commitPreview(
            Json{{"actor_id", session.at("actor_id")},
                 {"ticket", opened.at("ticket")}});
        REQUIRE(commit.at("status") == "accepted");
        service.commitPendingEdits();
        const auto committed = service.getPreviewResult(
            Json{{"request_id", commit.at("request_id")}});
        REQUIRE(committed.at("status") == "committed");
        REQUIRE(committed.at("committed_revision") ==
                base_revision.value + 1U);
        const auto saved = service.saveScene();
        REQUIRE(saved.byte_count > 0);
    }

    // A new module generation must decode the replaced file. Inspecting the
    // RPC request or the old in-memory document cannot satisfy this check.
    {
        Pelican::FastModuleContainer modules;
        Pelican::FastModuleContainer::get<Pelican::PathResolver>()
            .setup(project_dir.root, false);
        Pelican::FastModuleContainer::get<Pelican::ProjectSource>()
            .setProjectData(project.dump());
        const auto &reloaded =
            Pelican::FastModuleContainer::get<Pelican::ProjectBasicConfig>()
                .sceneDocument();
        REQUIRE(reloaded.rawJson()
                    .at("scenes")
                    .at("main")
                    .at("objects")
                    .at(0)
                    .at("components")
                    .at(0)
                    .at("pos") == edited_position);
    }
}
