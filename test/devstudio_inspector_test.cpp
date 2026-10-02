#include "inspectormodel.hpp"
#include "gizmomodel.hpp"

#include "../src/core/communication/editorcommandservice.hpp"
#include "../src/core/container.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/editorprojectiontransaction.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "authoringscenetestsupport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
using PelicanStudio::SpawnOutcome;

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

        const auto tree = take("scene_outline");
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
        const auto tree = take("scene_outline");
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
    : public Pelican::test_support::SceneProjectionTarget {
  public:
    explicit PersistencePreviewTarget(
        const Pelican::AuthoringSceneDocument &source)
        : SceneProjectionTarget{Pelican::AuthoringSceneAuthority::stage(
              source,
              Pelican::AuthoringSceneAuthority::rawView(source).documentJson(),
              Pelican::SceneRevision{source.revision().value + 1U})} {}
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

    const GizmoTransformBinding binding{
        .selection = selection,
        .position = GizmoEditableField{
            .field_key = "1:transform:0:/pos",
            .value = Json::array({1.0, 2.0, 3.0})},
    };
    const auto modal_state = [](std::string_view phase,
                                std::uint64_t revision) {
        return Json{
            {"contract", 1},
            {"enabled", true},
            {"revision", revision},
            {"phase", phase},
            {"operation_id", 11},
            {"selection",
             {{"kind", "declaration"},
              {"scene_id", "main"},
              {"declaration_index", 0}}},
            {"mode", "translate"},
            {"axis", "x"},
            {"delta",
             {{"translation", {3.0, 0.0, 0.0}},
              {"rotation", {0.0, 0.0, 0.0, 1.0}},
              {"scale_exponent", {0.0, 0.0, 0.0}}}},
            {"reason", "test"},
            {"bindings",
             {{"translate", nullptr},
              {"rotate", nullptr},
              {"scale", nullptr}}},
        };
    };
    gizmo.pollModalTransform(binding);
    auto poll = takeGizmoRpc(gizmo, "get_modal_transform");
    gizmo.receiveRpcResult(poll.request_id,
                           modal_state("active", 2).dump());
    auto actions = gizmo.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == GizmoEditActionKind::Begin);
    gizmo.confirmEditStarted(actions.front().gesture_id, true);

    actions = gizmo.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == GizmoEditActionKind::Preview);
    const Json edited = actions.front().value;
    REQUIRE(edited == Json::array({4.0, 2.0, 3.0}));

    gizmo.pollModalTransform(binding);
    poll = takeGizmoRpc(gizmo, "get_modal_transform");
    gizmo.receiveRpcResult(poll.request_id,
                           modal_state("confirmed", 3).dump());
    actions = gizmo.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == GizmoEditActionKind::Finish);
    REQUIRE(actions.front().commit);
    (void)takeGizmoRpc(gizmo, "ack_modal_transform");
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

TEST_CASE("WP361 Studio ingests the real generated union without mutation widgets",
          "[devstudio][inspector][wp361][prefab][readonly]") {
    InspectorHarness harness;
    harness.model.selectObject(
        OutlinerObjectKey{.scene_id = "main", .declaration_index = 2});
    harness.model.startSession();
    const auto session = harness.take("open_editor_session");
    harness.reply(session, {{"actor_id", 9},
                            {"display_name", "Pelican Studio Inspector"}});
    const auto tree = harness.take("scene_outline");
    harness.reply(tree, sceneTreeResult());
    const auto components = harness.take("get_components");
    const Json result{
        {"scene_revision", 1},
        {"authoring_object_id", 41},
        {"declaration_index", 2},
        {"prefab_instance",
         {{"ref", "unit"},
          {"instance_id", "gi_main"},
          {"closure_generation", 3},
          {"provider_generation", 7},
          {"parameters",
           Json::array({{{"name", "tint"},
                         {"value_resolved", {1, 1, 1, 1}},
                         {"source", "default"}},
                        {{"name", "hp"},
                         {"value_resolved", 55},
                         {"source", "override"},
                         {"value_authored", 55}}})}}},
        {"components",
         Json::array({
             {{"generated",
               {{"stable_generated_id",
                 "gid_0123456789abcdef0123456789abcdef"},
                {"source",
                 {{"prefab", "unit"},
                  {"instance_id", "gi_main"},
                  {"component_key", "sprite"},
                  {"digest",
                   {{"algorithm", "sha256"},
                    {"hex",
                     "0f9c2a4b8d6e13577531fedcba9876543210abcdef0123456789abcdef012345"}}}}},
                {"resolved_json",
                 {{"name", "sprite_view"}, {"texture", "white"}}},
                {"editable", false}}}},
         })},
    };
    harness.reply(components, result);
    const auto watch = harness.take("get_scene_revision");
    harness.reply(watch, {{"scene_revision", 1}, {"preview_epoch", 0}});

    REQUIRE(harness.model.snapshot());
    REQUIRE(harness.model.snapshot()->prefab_instance);
    REQUIRE(harness.model.snapshot()->prefab_parameters.size() == 2);
    const auto &default_parameter =
        harness.model.snapshot()->prefab_parameters.at(0);
    REQUIRE(default_parameter.name == "tint");
    REQUIRE(default_parameter.source == "default");
    REQUIRE(default_parameter.value_resolved == Json::array({1, 1, 1, 1}));
    REQUIRE_FALSE(default_parameter.value_authored);
    const auto &override_parameter =
        harness.model.snapshot()->prefab_parameters.at(1);
    REQUIRE(override_parameter.name == "hp");
    REQUIRE(override_parameter.source == "override");
    REQUIRE(override_parameter.value_resolved == 55);
    REQUIRE(override_parameter.value_authored);
    REQUIRE(*override_parameter.value_authored == 55);
    REQUIRE(harness.model.snapshot()->components.size() == 1);
    const auto &generated = harness.model.snapshot()->components.front();
    REQUIRE(generated.generated);
    REQUIRE_FALSE(harness.model.canRemoveComponent(generated.component_index));
    REQUIRE_FALSE(harness.model.removeComponent(generated.component_index));
    REQUIRE_FALSE(generated.editable);
    REQUIRE(generated.widgets.empty());
    REQUIRE(generated.stable_generated_id ==
            "gid_0123456789abcdef0123456789abcdef");
    REQUIRE(generated.generated_source.at("component_key") == "sprite");
    REQUIRE_FALSE(harness.model.commitWidgetValue(
        "gid_0123456789abcdef0123456789abcdef", false));
    REQUIRE(harness.model.takeRpcRequests().empty());
}

TEST_CASE("WP358 Studio production preflight takes I8 bounds from the leaf",
          "[devstudio][inspector][schema][wp358]") {
    Json component{
        {"name", "schema_fixture"},
        {"component_index", 0},
        {"authored_json", {{"i8", 127}}},
        {"schema",
         {{"state", "available"},
          {"fields", Json::array({{{"name", "i8"}, {"type", "i8"}}})}}},
    };

    auto plan = PelicanStudio::makeInspectorWidgetPlan(component, 99);
    REQUIRE(plan.size() == 1);
    REQUIRE(plan.front().value == 127);
    REQUIRE(plan.front().value_matches_schema);

    component["authored_json"]["i8"] = 128;
    plan = PelicanStudio::makeInspectorWidgetPlan(component, 99);
    REQUIRE(plan.size() == 1);
    REQUIRE(plan.front().value == 128);
    REQUIRE_FALSE(plan.front().value_matches_schema);
}

TEST_CASE("WP358 Studio response ingestion reports unknown leaf type by name and path",
          "[devstudio][inspector][schema][rpc][wp358]") {
    InspectorHarness harness;
    harness.model.selectObject(OutlinerObjectKey{
        .scene_id = "main",
        .declaration_index = 2,
    });
    harness.model.startSession();
    const auto session = harness.take("open_editor_session");
    harness.reply(session,
                  {{"actor_id", 9},
                   {"display_name", "Pelican Studio Inspector"}});
    const auto tree = harness.take("scene_outline");
    harness.reply(tree, sceneTreeResult());
    const auto components = harness.take("get_components");
    auto response = componentResult();
    response["components"][0]["schema"]["fields"][0]["type"] =
        "future_vector";
    harness.reply(components, response);

    REQUIRE_FALSE(harness.model.snapshot());
    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Error);
    REQUIRE(harness.model.notice().message.find(
                "schema_error[unknown_type_name]") != std::string::npos);
    REQUIRE(harness.model.notice().message.find("schema field.type") !=
            std::string::npos);
    REQUIRE(harness.model.notice().message.find("future_vector") !=
            std::string::npos);
    std::cout << "WP358_STUDIO_UNKNOWN_STRING="
              << harness.model.notice().message << '\n';
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
    const auto tree = harness.take("scene_outline");
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
    REQUIRE(harness.take("scene_outline").method == "scene_outline");
}

TEST_CASE("Devstudio inspector still reflects an external watch change when idle",
          "[devstudio][inspector][refresh][external][wp245][wp266]") {
    InspectorHarness harness;
    harness.open();

    harness.model.pollExternalChanges();
    const auto watch = harness.take("get_scene_revision");
    harness.reply(watch, {{"scene_revision", 2}, {"preview_epoch", 0}});

    const auto tree = harness.take("scene_outline");
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
    const auto tree = harness.take("scene_outline");
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
    REQUIRE(harness.take("scene_outline").method == "scene_outline");
}

TEST_CASE("Devstudio watch sync retries when an external revision lands during refresh",
          "[devstudio][inspector][refresh][external][wp245][wp266]") {
    InspectorHarness harness;
    harness.open();

    harness.model.requestRefresh();
    const auto tree = harness.take("scene_outline");
    harness.reply(tree, sceneTreeResult(2));
    const auto components = harness.take("get_components");
    harness.reply(components, componentResult(2.0, 2));

    const auto watch = harness.take("get_scene_revision");
    harness.reply(watch, {{"scene_revision", 3}, {"preview_epoch", 0}});

    REQUIRE(harness.take("scene_outline").method == "scene_outline");
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
    REQUIRE(harness.take("scene_outline").method == "scene_outline");
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
    REQUIRE(harness.take("scene_outline").method == "scene_outline");
}

TEST_CASE("Devstudio undo uses the editor session revision after a modal gizmo preview",
          "[devstudio][inspector][gizmo][modal][undo][negative-contrast][wp266][wp286]") {
    InspectorHarness harness;
    harness.open();
    const Json modal_position = gizmoEditedPosition();
    harness.commitGizmoStylePosition(modal_position.at(0).get<double>());

    REQUIRE(harness.field("transform", "pos").value ==
            modal_position);

    harness.model.undo();
    const auto undo = harness.take("undo");
    REQUIRE(undo.params == Json{{"actor_id", 9}, {"base_revision", 2}});
    harness.reply(undo,
                  {{"status", "committed"}, {"committed_revision", 3}});

    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Success);
    REQUIRE(harness.take("scene_outline").method == "scene_outline");

    InspectorHarness no_modal_commit;
    no_modal_commit.open();
    no_modal_commit.model.undo();
    const auto no_commit_requests =
        no_modal_commit.model.takeRpcRequests();
    REQUIRE(no_commit_requests.size() == 1);
    REQUIRE(no_commit_requests.front().method == "undo");
    REQUIRE(no_commit_requests.front().params ==
            Json{{"actor_id", 9}, {"base_revision", 1}});
    REQUIRE(no_commit_requests.front().params != undo.params);
    REQUIRE(no_modal_commit.field("transform", "pos").value ==
            Json::array({1.0, 2.0, 3.0}));
    REQUIRE(no_modal_commit.field("transform", "pos").value !=
            modal_position);
}

TEST_CASE("Devstudio save scene RPC is serialized only while idle",
          "[devstudio][inspector][save][rpc][wp275]") {
    InspectorHarness harness;
    harness.open();
    REQUIRE_FALSE(harness.model.hasUnsavedChanges());
    harness.commitGizmoStylePosition(4.0);
    REQUIRE(harness.model.hasUnsavedChanges());

    REQUIRE(harness.model.canSave());
    REQUIRE(harness.model.saveScene());
    const auto save = harness.take("save_scene");
    REQUIRE(save.params == Json::object());
    REQUIRE(harness.model.busy());
    REQUIRE(harness.model.hasUnsavedChanges());
    REQUIRE(harness.model.sceneSaveCompletion() == 0);
    REQUIRE_FALSE(harness.model.saveScene());

    harness.reply(save,
                  {{"status", "saved"},
                   {"scene_revision", 2},
                   {"scene_hot_reload", false},
                   {"byte_count", 1234}});
    REQUIRE_FALSE(harness.model.busy());
    REQUIRE_FALSE(harness.model.hasUnsavedChanges());
    REQUIRE(harness.model.lastSceneSaveSucceeded());
    REQUIRE(harness.model.sceneSaveCompletion() == 1);
    REQUIRE(harness.model.notice().kind == InspectorNoticeKind::Success);
    REQUIRE(harness.model.notice().message.find("revision 2") !=
            std::string::npos);

    // Saving is scene-wide and must not depend on retaining an Inspector
    // object selection after the gizmo edit was committed.
    harness.model.selectObject(std::nullopt);
    REQUIRE(harness.model.canSave());
}

TEST_CASE("Studio preserves unsaved revisions across failed saves and selection changes",
          "[devstudio][inspector][save][unsaved]") {
    InspectorHarness harness;
    harness.open();
    harness.model.selectObject(std::nullopt);
    harness.model.observeSceneRevision(2);
    REQUIRE(harness.model.hasUnsavedChanges());
    REQUIRE(harness.model.saveScene());
    const auto save = harness.take("save_scene");

    SECTION("a failed save never clears dirty state") {
        harness.model.receiveRpcFailure(save.request_id, "external_modification");
        CHECK(harness.model.hasUnsavedChanges());
        CHECK_FALSE(harness.model.lastSceneSaveSucceeded());
        CHECK(harness.model.sceneSaveCompletion() == 1);
    }
    SECTION("a disconnected save is a completed failure") {
        harness.model.stopSession("engine stopped");
        CHECK(harness.model.hasUnsavedChanges());
        CHECK_FALSE(harness.model.lastSceneSaveSucceeded());
        CHECK(harness.model.sceneSaveCompletion() == 1);
        // A late success from the abandoned transport cannot release a close.
        harness.reply(save, {{"status", "saved"}, {"scene_revision", 2}});
        CHECK(harness.model.hasUnsavedChanges());
        CHECK_FALSE(harness.model.lastSceneSaveSucceeded());
    }
    SECTION("saving an older revision leaves a concurrent edit dirty") {
        harness.model.observeSceneRevision(3);
        harness.reply(save, {{"status", "saved"}, {"scene_revision", 2}});
        CHECK(harness.model.lastSceneSaveSucceeded());
        CHECK(harness.model.hasUnsavedChanges());
        harness.model.observeSceneRevision(1); // delayed pre-save response
        CHECK(harness.model.hasUnsavedChanges());
        REQUIRE(harness.model.saveScene());
        harness.reply(harness.take("save_scene"), {{"status", "saved"}, {"scene_revision", 3}});
        CHECK_FALSE(harness.model.hasUnsavedChanges());
    }
    SECTION("malformed success does not count as persistence") {
        harness.reply(save, {{"status", "saved"}});
        CHECK(harness.model.hasUnsavedChanges());
        CHECK_FALSE(harness.model.lastSceneSaveSucceeded());
        CHECK(harness.model.sceneSaveCompletion() == 1);
    }
}

TEST_CASE("Studio scene dirty state follows committed revisions rather than refreshes",
          "[devstudio][inspector][save][unsaved]") {
    InspectorHarness harness;
    harness.open();
    harness.model.requestRefresh();
    harness.completeRefresh(1.0, 1);
    CHECK_FALSE(harness.model.hasUnsavedChanges());
    harness.model.pollExternalChanges();
    harness.reply(harness.take("get_scene_revision"),
                  {{"scene_revision", 1}, {"preview_epoch", 1}});
    CHECK_FALSE(harness.model.hasUnsavedChanges());
    harness.model.observeSceneRevision(2);
    REQUIRE(harness.model.hasUnsavedChanges());
    harness.model.stopSession();
    CHECK(harness.model.hasUnsavedChanges());
    harness.model.resetSceneSaveState();
    harness.model.observeSceneRevision(1);
    CHECK_FALSE(harness.model.hasUnsavedChanges());
}

TEST_CASE("Studio retains edit tickets when the selected object changes",
          "[devstudio][inspector][save][unsaved]") {
    InspectorHarness harness;
    harness.open();
    const auto field = harness.field("fixture_flags", "enabled").field_key;
    REQUIRE(harness.model.commitWidgetValue(field, false));
    const auto edit = harness.take("edit");
    SECTION("selection changes before the edit is accepted") {
        harness.model.selectObject(std::nullopt);
        REQUIRE(harness.model.busy());
        REQUIRE_FALSE(harness.model.canSave());
        harness.reply(edit, {{"status", "accepted"}, {"ticket", "edit:changed-selection"}});
    }
    SECTION("selection changes after acceptance but before commit") {
        harness.reply(edit, {{"status", "accepted"}, {"ticket", "edit:changed-selection"}});
        harness.model.selectObject(std::nullopt);
        REQUIRE(harness.model.busy());
        REQUIRE_FALSE(harness.model.canSave());
    }
    REQUIRE(harness.model.busy());
    REQUIRE(harness.model.pendingPollWanted());
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_edit_result"),
                  {{"status", "committed"}, {"committed_revision", 2}});
    CHECK_FALSE(harness.model.busy());
    CHECK(harness.model.hasUnsavedChanges());
    REQUIRE(harness.model.saveScene());
    // SAVE0 publishes its own revision, even though the authored values did
    // not change. A delayed pre-save view response must not dirty it again.
    harness.reply(harness.take("save_scene"), {{"status", "saved"}, {"scene_revision", 3}});
    harness.model.observeSceneRevision(2);
    CHECK_FALSE(harness.model.hasUnsavedChanges());
}

TEST_CASE("Studio cannot infer a clean scene from a lost edit response or restarted counter",
          "[devstudio][inspector][save][unsaved]") {
    InspectorHarness harness;
    harness.open();
    const auto field = harness.field("fixture_flags", "enabled").field_key;
    REQUIRE(harness.model.commitWidgetValue(field, false));
    const auto edit = harness.take("edit");
    SECTION("disconnect while an edit result is unknown") {
        harness.model.stopSession("connection lost");
        CHECK(harness.model.hasUnsavedChanges());
    }
    SECTION("transport failure after sending the edit") {
        harness.model.receiveRpcFailure(edit.request_id, "connection lost");
        CHECK(harness.model.hasUnsavedChanges());
        harness.model.stopSession();
    }
    SECTION("a committed revision is lost when the process restarts") {
        harness.reply(edit, {{"status", "committed"}, {"committed_revision", 7}});
        REQUIRE(harness.model.hasUnsavedChanges());
        harness.model.stopSession();
    }
    CHECK(harness.model.hasUnsavedChanges());
    harness.model.selectObject(std::nullopt);
    harness.model.startSession();
    harness.reply(harness.take("open_editor_session"), {{"actor_id", 10}});
    harness.model.observeSceneRevision(1);
    CHECK(harness.model.hasUnsavedChanges());
    REQUIRE(harness.model.saveScene());
    harness.reply(harness.take("save_scene"), {{"status", "saved"}, {"scene_revision", 2}});
    CHECK_FALSE(harness.model.hasUnsavedChanges());
}

TEST_CASE("Discarded Inspector operations cannot dirty the next project",
          "[devstudio][inspector][save][unsaved]") {
    InspectorHarness harness;
    harness.open();

    const auto discardAndReconnect = [&](const InspectorRpcRequest &late) {
        REQUIRE(harness.model.busy());
        REQUIRE_FALSE(harness.model.canSave());

        // InspectorWidget discards a document by ending its session before
        // resetting the baseline. The viewport subsequently stops/restarts
        // that session again as the next project's process becomes ready.
        harness.model.stopSession();
        REQUIRE(harness.model.hasUnsavedChanges());
        harness.model.resetSceneSaveState();
        REQUIRE_FALSE(harness.model.hasUnsavedChanges());
        REQUIRE_FALSE(harness.model.busy());
        REQUIRE_FALSE(harness.model.refreshBlocked());
        harness.model.selectObject(std::nullopt);
        harness.model.stopSession();
        harness.model.startSession();
        const auto session = harness.take("open_editor_session");

        // A reply in flight from the discarded process must not establish
        // either the new project's saved baseline or its current revision.
        harness.reply(late, {{"status", "committed"}, {"committed_revision", 99}});
        harness.reply(session, {{"actor_id", 10}});
        harness.model.observeSceneRevision(1);
        CHECK_FALSE(harness.model.hasUnsavedChanges());
        CHECK_FALSE(harness.model.busy());
        harness.model.pollPendingOperations();
        CHECK(harness.model.takeRpcRequests().empty());
        harness.model.observeSceneRevision(2);
        CHECK(harness.model.hasUnsavedChanges());
    };

    SECTION("a pending edit ticket belongs only to the discarded document") {
        const auto field = harness.field("fixture_flags", "enabled").field_key;
        REQUIRE(harness.model.commitWidgetValue(field, false));
        harness.reply(harness.take("edit"),
                      {{"status", "accepted"}, {"ticket", "old-edit"}});
        harness.model.pollPendingOperations();
        discardAndReconnect(harness.take("get_edit_result"));
    }
    SECTION("an unresolved preview belongs only to the discarded document") {
        const auto field = harness.field("transform", "pos").field_key;
        REQUIRE(harness.model.previewWidgetValue(field, Json::array({4.0, 2.0, 3.0})));
        harness.reply(harness.take("open_preview"),
                      {{"status", "accepted"}, {"ticket", "old-preview"},
                       {"request_id", "old-preview-request"}});
        harness.model.pollPendingOperations();
        discardAndReconnect(harness.take("get_preview_result"));
    }
}

TEST_CASE("A restarted Inspector cannot apply selection deferred by an old preview",
          "[devstudio][inspector][preview][unsaved]") {
    InspectorHarness harness;
    harness.open();
    const auto original_selection = harness.model.selection();
    const auto field = harness.field("transform", "pos").field_key;
    REQUIRE(harness.model.previewWidgetValue(field, Json::array({4.0, 2.0, 3.0})));
    const auto old_preview = harness.take("open_preview");
    harness.model.selectObject(OutlinerObjectKey{
        .scene_id = "main", .declaration_index = 0});
    // Selection is deferred until this preview aborts. A replacement session
    // must discard that deferred action together with the old preview.
    REQUIRE(harness.model.selection() == original_selection);
    REQUIRE(harness.model.busy());

    const auto reconnectAndCommit = [&] {
        harness.model.startSession();
        harness.reply(harness.take("open_editor_session"), {{"actor_id", 10}});
        harness.completeRefresh(1.0, 1);
        harness.reply(old_preview,
                      {{"status", "accepted"}, {"ticket", "stale-preview"},
                       {"request_id", "stale-preview-request"}});
        REQUIRE(harness.model.selection() == original_selection);
        REQUIRE_FALSE(harness.model.busy());
        harness.commitGizmoStylePosition(5.0);
        REQUIRE(harness.model.selection() == original_selection);
        REQUIRE(harness.model.snapshot());
        CHECK(harness.model.snapshot()->declaration_index == 2);
        CHECK(harness.field("transform", "pos").value == Json::array({5.0, 2.0, 3.0}));
    };

    SECTION("an explicit stop discards the deferred selection") {
        harness.model.stopSession();
        reconnectAndCommit();
    }
    SECTION("replacing the session directly also discards the deferred selection") {
        reconnectAndCommit();
    }
}

TEST_CASE("Devstudio modal gizmo commit survives save and a fresh authoring reload",
          "[devstudio][inspector][gizmo][modal][save][reload][negative-contrast][wp286]") {
    Pelican::setupLogger(true);
    PersistenceProject project_dir;
    const Json source{
        {"schema", "pelican.scene"},
        {"version", 2},
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
        {"name", "WP286 persistence"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"default_scene_id", "main"},
          {"scene_data_json", "scene.json"}}},
    };
    const Json edited_position = gizmoEditedPosition();
    REQUIRE(source.at("scenes")
                .at("main")
                .at("objects")
                .at(0)
                .at("components")
                .at(0)
                .at("pos") != edited_position);

    {
        Pelican::FastModuleContainer modules;
        Pelican::FastModuleContainer::get<Pelican::PathResolver>()
            .setup(project_dir.root, false);
        Pelican::FastModuleContainer::get<Pelican::ProjectSource>()
            .setProjectData(project.dump());
        auto &config =
            Pelican::FastModuleContainer::get<Pelican::ProjectBasicConfig>();
        const auto &document = config.sceneDocument();
        const auto scenes =
            Pelican::test_support::authoring(document).query();
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
        REQUIRE(Pelican::test_support::authoring(reloaded)
                    .rawJson()
                    .at("scenes")
                    .at("main")
                    .at("objects")
                    .at(0)
                    .at("components")
                    .at(0)
                    .at("pos") == edited_position);
    }
}

TEST_CASE("Studio removes authored components through the editor journal",
          "[devstudio][inspector][component-removal]") {
    InspectorHarness harness;
    harness.open();
    auto response = componentResult();
    auto &component = response["components"][1];
    const auto exercise = [&](bool removable) {
        harness.model.requestRefresh();
        harness.reply(harness.take("scene_outline"), sceneTreeResult());
        harness.reply(harness.take("get_components"), response);
        harness.reply(harness.take("get_scene_revision"),
                      {{"scene_revision", 1}, {"preview_epoch", 0}});
        CHECK(harness.model.canRemoveComponent(1) == removable);
        CHECK(harness.model.removeComponent(1) == removable);
        if (removable) {
            const auto edit = harness.take("edit");
            CHECK(edit.params.at("actor_id") == 9);
            CHECK(edit.params.at("base_revision") == 1);
            Json expected{{"op", "remove_component"}, {"object_id", 41},
                          {"component_slot", component.at("name")}};
            if (component.at("name") == "behavior") expected["attachment_index"] = 1;
            CHECK(edit.params.at("operations") == Json::array({expected}));
            CHECK_FALSE(harness.model.removeComponent(1));
            CHECK(harness.model.takeRpcRequests().empty());
            harness.reply(edit, {{"status", "accepted"}, {"ticket", "remove:1"}});
            harness.model.pollPendingOperations();
            harness.reply(harness.take("get_edit_result"),
                          {{"status", "committed"}, {"committed_revision", 2}});
            harness.reply(harness.take("scene_outline"), sceneTreeResult(2));
            response["scene_revision"] = 2;
            response["components"].erase(1);
            harness.reply(harness.take("get_components"), response);
            harness.reply(harness.take("get_scene_revision"),
                          {{"scene_revision", 2}, {"preview_epoch", 0}});
            CHECK(harness.model.snapshot()->components.size() == 1);
            CHECK_FALSE(harness.model.canRemoveComponent(1));
            harness.model.undo();
            CHECK(harness.take("undo").params == Json{{"actor_id", 9}, {"base_revision", 2}});
        } else {
            CHECK(harness.model.takeRpcRequests().empty());
        }
    };
    SECTION("ordinary component") {
        exercise(true);
    }
    SECTION("behavior attachment uses its authored index") {
        component["name"] = "behavior";
        component["authored_json"]["type"] = "fixture";
        exercise(true);
    }
    SECTION("pending component remains disabled") {
        component["pending"] = true;
        exercise(false);
    }
    SECTION("noneditable component remains disabled") {
        component["editable"] = false;
        exercise(false);
    }
}

TEST_CASE("Studio adds a catalog component through the existing revision and ticket flow",
          "[devstudio][inspector][component-addition]") {
    InspectorHarness harness;
    harness.open();
    const Json component{{"name", "collider"}, {"type", "box"}};
    Json catalog{{"object_id", 41}, {"scene_revision", 1},
                 {"candidates", Json::array({
                     {{"key", "collider"}, {"name", "collider"}, {"available", true}, {"component", component},
                      {"component_template", component}, {"schema_fields", Json::array()}},
                     {{"key", "simplemodelview"}, {"name", "simplemodelview"}, {"available", false},
                      {"component_template", {{"name", "simplemodelview"}}},
                      {"schema_fields", Json::array({{{"name", "model"}, {"type", "string"}}})},
                      {"unavailable_reason", "A model reference is required."}}})}};
    REQUIRE(harness.model.requestComponentAddCandidates());
    const auto query = harness.take("get_component_add_candidates");
    CHECK(query.params == Json{{"scene_id", "main"}, {"authoring_object_id", 41}});
    CHECK_FALSE(harness.model.requestComponentAddCandidates());

    SECTION("available choice submits once and refreshes after commit") {
        harness.reply(query, catalog);
        CHECK_FALSE(harness.model.addComponent("simplemodelview"));
        REQUIRE(harness.model.addComponent("collider"));
        const auto edit = harness.take("edit");
        CHECK(edit.params.at("actor_id") == 9);
        CHECK(edit.params.at("base_revision") == 1);
        CHECK(edit.params.at("operations") == Json::array({
            {{"op", "add_component"}, {"object_id", 41}, {"component", component}}}));
        CHECK_FALSE(harness.model.addComponent("collider"));
        harness.reply(edit, {{"status", "accepted"}, {"ticket", "add:1"}});
        harness.model.pollPendingOperations();
        harness.reply(harness.take("get_edit_result"),
                      {{"status", "committed"}, {"committed_revision", 2}});
        harness.completeRefresh(1.0, 2);
        CHECK_FALSE(harness.model.componentAddCandidates());
        harness.model.undo();
        CHECK(harness.take("undo").params == Json{{"actor_id", 9}, {"base_revision", 2}});
    }
    SECTION("custom required fields use the same edit and allow retry after rejection") {
        harness.reply(query, catalog);
        const Json declaration{{"name", "simplemodelview"}, {"model", "chair"}};
        CHECK_FALSE(harness.model.addComponent("unknown", declaration));
        CHECK_FALSE(harness.model.addComponent("collider", declaration));
        REQUIRE(harness.model.addComponent("simplemodelview", declaration));
        const auto edit = harness.take("edit");
        CHECK(edit.params.at("base_revision") == 1);
        CHECK(edit.params.at("operations") == Json::array({
            {{"op", "add_component"}, {"object_id", 41}, {"component", declaration}}}));
        harness.reply(edit, {{"status", "accepted"}, {"ticket", "custom:1"}});
        harness.model.pollPendingOperations();
        harness.reply(harness.take("get_edit_result"),
                      {{"status", "failed"}, {"error", {{"code", "invalid_model"}, {"message", "Model does not exist"}}}});
        CHECK(harness.model.notice().kind == InspectorNoticeKind::Error);
        CHECK(harness.model.snapshot()->scene_revision == 1);
        harness.completeRefresh(1.0, 1);
        REQUIRE(harness.model.requestComponentAddCandidates());
        harness.reply(harness.take("get_component_add_candidates"), catalog);
        REQUIRE(harness.model.addComponent("simplemodelview", declaration));
        CHECK(harness.take("edit").params.at("operations").at(0).at("component") == declaration);
    }
    SECTION("a newer scene revision requires refreshing the Inspector") {
        catalog["scene_revision"] = 2;
        harness.reply(query, catalog);
        CHECK_FALSE(harness.model.componentAddCandidates());
        CHECK_FALSE(harness.model.addComponent("collider"));
        CHECK(harness.model.takeRpcRequests().empty());
    }
    SECTION("selection changes discard an outstanding catalog") {
        harness.model.selectObject(std::nullopt);
        harness.reply(query, catalog);
        CHECK_FALSE(harness.model.componentAddCandidates());
        CHECK_FALSE(harness.model.addComponent("collider"));
    }
}

TEST_CASE("Studio adds behavior attachments by catalog key without replacing existing attachments",
          "[devstudio][inspector][component-addition][behavior]") {
    for (const auto &type : {std::string{"existing_behavior"}, std::string{"other_behavior"}}) {
        InspectorHarness harness;
        harness.open();
        auto components = componentResult();
        components["components"][1]["name"] = "behavior";
        components["components"][1]["authored_json"] = {{"name", "behavior"}, {"type", "existing_behavior"}, {"params", Json::object()}};
        harness.model.requestRefresh();
        harness.reply(harness.take("scene_outline"), sceneTreeResult());
        harness.reply(harness.take("get_components"), components);
        harness.reply(harness.take("get_scene_revision"), {{"scene_revision", 1}, {"preview_epoch", 0}});
        const std::string key = "behavior:" + type;
        const Json declaration{{"name", "behavior"}, {"type", type}, {"params", {{"speed", 2.5}}}};
        const Json catalog{{"object_id", 41}, {"scene_revision", 1},
            {"candidates", Json::array({{{"key", key}, {"name", "behavior"}, {"behavior_type", type},
                {"available", false}, {"component_template", {{"name", "behavior"}, {"type", type}, {"params", Json::object()}}},
                {"schema_fields", Json::array()}, {"unavailable_reason", "Set speed."}}})}};
        REQUIRE(harness.model.requestComponentAddCandidates());
        harness.reply(harness.take("get_component_add_candidates"), catalog);
        auto wrong = declaration;
        wrong["type"] = "unregistered_behavior";
        CHECK_FALSE(harness.model.addComponent(key, wrong));
        CHECK_FALSE(harness.model.addComponent("behavior", declaration));
        REQUIRE(harness.model.addComponent(key, declaration));
        const auto edit = harness.take("edit");
        CHECK(edit.params.at("operations") == Json::array({
            {{"op", "add_component"}, {"object_id", 41}, {"component", declaration}}}));
        CHECK(harness.model.snapshot()->components[1].name == "behavior");
    }
}

TEST_CASE("Inspector materializes only the selected omitted default",
          "[devstudio][inspector][insert-default]") {
    InspectorHarness harness;
    harness.open();
    auto components = componentResult();
    auto &component = components["components"][1];
    component["name"] = "behavior";
    component["authored_json"] = {{"name", "behavior"}, {"type", "default_behavior"}};
    component["behavior"] = {{"attachment_handle", 901}};
    component["schema"]["node"] = {{"tag", "struct"}, {"children", Json::array({
        {{"tag", "leaf"}, {"name", "enabled"}, {"presence", "defaulted"}, {"default", false}}
    })}};
    // Each variant only shapes the component the shared body then exercises;
    // the assertions are common to all of them, so these are generator runs.
    // An assertionless section is rejected by `-w NoAssertions`.
    const std::string variant = GENERATE(
        std::string{"registered default"},
        std::string{"flat schema lets the codec choose its default"},
        std::string{"no default literal"},
        std::string{"pending component"},
        std::string{"already authored"});
    CAPTURE(variant);
    bool allowed = true;
    if (variant == "flat schema lets the codec choose its default") {
        component["schema"].erase("node");
    } else if (variant == "no default literal") {
        component["schema"]["node"]["children"][0].erase("default");
        allowed = false;
    } else if (variant == "pending component") {
        component["pending"] = true;
        allowed = false;
    } else if (variant == "already authored") {
        component["authored_json"]["params"] = {{"enabled", true}};
        allowed = false;
    }
    harness.model.requestRefresh();
    harness.reply(harness.take("scene_outline"), sceneTreeResult());
    harness.reply(harness.take("get_components"), components);
    harness.reply(harness.take("get_scene_revision"), {{"scene_revision", 1}, {"preview_epoch", 0}});
    REQUIRE(harness.model.snapshot());
    // Pending components intentionally have no editable widgets. Build the
    // requested field identity independently to test that submission is denied.
    const auto descriptors = PelicanStudio::makeInspectorWidgetPlan(component, 41);
    REQUIRE(descriptors.size() == 1);
    const auto key = descriptors.front().field_key;
    REQUIRE(harness.model.canInsertField(key) == allowed);
    REQUIRE(harness.model.insertField(key) == allowed);
    if (allowed) {
        const auto edit = harness.take("edit");
        REQUIRE(edit.params.at("base_revision") == 1);
        REQUIRE(edit.params.at("operations") == Json::array({
            {{"op", "insert_component_field"}, {"object_id", 41},
             {"component_slot", "behavior"}, {"attachment_index", 1},
             {"attachment_handle", 901}, {"field_path", "/params/enabled"}}
        }));
        REQUIRE_FALSE(harness.model.canInsertField(key));
        REQUIRE_FALSE(harness.model.snapshot()->components[1].authored_json.contains("params"));
    } else {
        REQUIRE(harness.model.takeRpcRequests().empty());
    }
}

TEST_CASE("Studio Inspector edits schema lists with stable addressed identities",
          "[devstudio][inspector][list-edit]") {
    InspectorHarness harness;
    harness.open();
    auto components = componentResult();
    auto &component = components["components"][1];
    component["name"] = "behavior";
    component["authored_json"] = {{"name", "behavior"}, {"type", "list_behavior"},
        {"params", {{"settings", {{"entries", {{"$list", {{"v", 1}, {"next_id", 9}}},
            {"rows", Json::array({
                {{"$row", {{"v", 1}, {"id", 3}}}, {"label", "first"}, {"enabled", true}},
                {{"$row", {{"v", 1}, {"id", 8}}}, {"label", "second"}}
            })}}}}}}}};
    const Json row_schema = {{"tag", "struct"}, {"name", ""}, {"children", Json::array({
        {{"tag", "leaf"}, {"name", "label"}, {"type", "string"}, {"presence", "required"}},
        {{"tag", "leaf"}, {"name", "enabled"}, {"type", "bool"}, {"presence", "defaulted"}, {"default", false}}
    })}};
    const Json list_schema = {{"tag", "list"}, {"name", "entries"}, {"presence", "defaulted"},
        {"children", Json::array({row_schema})}};
    component["schema"] = {{"state", "available"}, {"fields", Json::array()},
        {"node", {{"tag", "struct"}, {"name", ""}, {"children", Json::array({
            {{"tag", "struct"}, {"name", "settings"}, {"children", Json::array({list_schema})}}
        })}}}};
    const auto refresh = [&] {
        harness.model.requestRefresh();
        harness.reply(harness.take("scene_outline"), sceneTreeResult());
        harness.reply(harness.take("get_components"), components);
        harness.reply(harness.take("get_scene_revision"), {{"scene_revision", 1}, {"preview_epoch", 0}});
    };
    refresh();
    REQUIRE(harness.model.snapshot());
    const auto &lists = harness.model.snapshot()->components[1].lists;
    REQUIRE(lists.size() == 1);
    CHECK(lists[0].path == "/settings/entries");
    CHECK(lists[0].rows.size() == 2);
    const auto key = lists[0].key;
    REQUIRE(harness.model.canEditList(key));
    const Json address = {{"authoring_object_id", 41},
        {"component", {{"type", "list_behavior"}, {"occurrence", 1},
            {"origin", {{"kind", "authored"}, {"authoring_component_index", 1}}}}},
        {"list_path", "/settings/entries"}};
    SECTION("insert preserves address and delegates ID allocation") {
        REQUIRE(harness.model.insertListRow(key, {{"label", "third"}}));
        const auto edit = harness.take("edit");
        CHECK(edit.params.at("base_revision") == 1);
        CHECK(edit.params.at("operations") == Json::array({
            {{"op", "insert_component_field"}, {"address", address},
             {"value", {{"label", "third"}}}, {"position", {{"end", true}}}}
        }));
        CHECK_FALSE(harness.model.canEditList(key));
    }
    SECTION("remove targets row ID rather than array index") {
        REQUIRE(harness.model.removeListRow(key, 8));
        auto expected = address;
        expected["row_id"] = 8;
        CHECK(harness.take("edit").params.at("operations") == Json::array({
            {{"op", "remove_component_field"}, {"address", expected}}
        }));
    }
    SECTION("move uses identity preserving addressed operation") {
        REQUIRE(harness.model.moveListRow(key, 8, 3));
        auto expected = address;
        expected["row_id"] = 8;
        CHECK(harness.take("edit").params.at("operations") == Json::array({
            {{"op", "move_component_field"}, {"address", expected}, {"position", {{"before", 3}}}}
        }));
    }
    SECTION("row edit removes optional fields and changes required leaf atomically") {
        REQUIRE(harness.model.editListRow(key, 3, {{"label", "renamed"}}));
        const auto operations = harness.take("edit").params.at("operations");
        REQUIRE(operations.size() == 2);
        CHECK(operations[0].at("op") == "set_component_value");
        CHECK(operations[0].at("address").at("leaf_path") == "/label");
        CHECK(operations[0].at("value") == "renamed");
        CHECK(operations[1].at("op") == "remove_component_field");
        CHECK(operations[1].at("address").at("row_id") == 3);
        CHECK(operations[1].at("address").at("leaf_path") == "/enabled");
    }
    SECTION("absent optional leaf inserts its default then sets the requested value") {
        REQUIRE(harness.model.editListRow(key, 8, {{"label", "second"}, {"enabled", true}}));
        const auto operations = harness.take("edit").params.at("operations");
        REQUIRE(operations.size() == 2);
        CHECK(operations[0].at("op") == "insert_component_field");
        CHECK(operations[1].at("op") == "set_component_value");
        CHECK(operations[1].at("value") == true);
        CHECK(operations[0].at("address") == operations[1].at("address"));
    }
    SECTION("missing list remains visible and insertable") {
        component["authored_json"]["params"]["settings"].erase("entries");
        refresh();
        CHECK(harness.model.snapshot()->components[1].lists[0].rows.empty());
        REQUIRE(harness.model.insertListRow(key, {{"label", "first"}}));
    }
    SECTION("invalid identities and reserved fields never submit") {
        CHECK(harness.model.editListRow(key, 3, {{"label", "first"}, {"enabled", true}}));
        CHECK_FALSE(harness.model.removeListRow(key, 0));
        CHECK_FALSE(harness.model.removeListRow(key, 1));
        CHECK_FALSE(harness.model.moveListRow(key, 3, 3));
        CHECK_FALSE(harness.model.moveListRow(key, 3, 99));
        CHECK_FALSE(harness.model.editListRow(key, 3, {{"$row", {{"id", 8}}}, {"label", "first"}}));
        CHECK_FALSE(harness.model.editListRow(key, 3, {{"unknown", true}}));
        CHECK(harness.model.takeRpcRequests().empty());
        const auto field = harness.field("transform", "pos").field_key;
        REQUIRE(harness.model.beginWidgetEdit(field));
        CHECK_FALSE(harness.model.canEditList(key));
    }
    SECTION("pending and read-only components cannot mutate lists") {
        component["pending"] = true;
        refresh();
        CHECK_FALSE(harness.model.canEditList(key));
        component["pending"] = false;
        component["editable"] = false;
        refresh();
        CHECK_FALSE(harness.model.insertListRow(key, {{"label", "first"}}));
    }
    SECTION("selection refresh invalidates stale list actions") {
        harness.model.selectObject(OutlinerObjectKey{.scene_id = "main", .declaration_index = 0});
        CHECK_FALSE(harness.model.canEditList(key));
    }
}

TEST_CASE("Studio Inspector ray occurrence catalog is tied to the current object snapshot",
          "[devstudio][inspector][ray-occurrences]") {
    InspectorHarness harness;
    harness.open();
    auto components = componentResult();
    auto &component = components["components"][1];
    component["name"] = "pelican_ray_instance";
    component["authored_json"] = {{"name", "pelican_ray_instance"}};
    component["schema"] = {{"state", "available"}, {"fields", Json::array()},
        {"node", {{"tag", "struct"}, {"name", ""}, {"children", Json::array({
            {{"tag", "list"}, {"name", "overrides"}, {"presence", "defaulted"}, {"children", Json::array({
                {{"tag", "struct"}, {"name", ""}, {"children", Json::array({
                    {{"tag", "leaf"}, {"name", "occurrence_id"}, {"type", "string"}, {"presence", "required"}}
                })}}
            })}}
        })}}}};
    harness.model.requestRefresh();
    harness.reply(harness.take("scene_outline"), sceneTreeResult());
    harness.reply(harness.take("get_components"), components);
    harness.reply(harness.take("get_scene_revision"), {{"scene_revision", 1}, {"preview_epoch", 0}});
    REQUIRE(harness.model.snapshot()->components[1].lists.size() == 1);
    const auto key = harness.model.snapshot()->components[1].lists[0].key;
    const Json catalog{{"generation", 12}, {"entries", Json::array({
        {{"occurrence_id", "visible"}, {"display_name", "Mesh / primitive 0"}, {"eligibility", "ok"}, {"authoring_object_id", 41}},
        {{"occurrence_id", "ambiguous"}, {"eligibility", {{"state", "ambiguous"}}}, {"authoring_object_id", 41}},
        {{"occurrence_id", "other-object"}, {"eligibility", "ok"}, {"authoring_object_id", 7}}
    })}, {"overrides", Json::array()}, {"rejected_rows", Json::array()}};
    CHECK_FALSE(harness.model.addRayOccurrenceOverride(key, "visible"));
    REQUIRE(harness.model.requestRayOccurrences());
    const auto request = harness.take("get_ray_occurrences");
    CHECK(request.params == Json{{"scene_id", "main"}, {"authoring_object_id", 41}});
    CHECK_FALSE(harness.model.requestRayOccurrences());
    SECTION("eligible selection uses the existing list edit transaction") {
        harness.reply(request, catalog);
        REQUIRE(harness.model.rayOccurrences());
        CHECK(*harness.model.rayOccurrences() == catalog);
        CHECK_FALSE(harness.model.addRayOccurrenceOverride(key, "unknown"));
        CHECK_FALSE(harness.model.addRayOccurrenceOverride(key, "ambiguous"));
        CHECK_FALSE(harness.model.addRayOccurrenceOverride(key, "other-object"));
        REQUIRE(harness.model.addRayOccurrenceOverride(key, "visible"));
        const auto edit = harness.take("edit");
        CHECK(edit.params.at("base_revision") == 1);
        const auto &operation = edit.params.at("operations")[0];
        CHECK(operation.at("op") == "insert_component_field");
        CHECK(operation.at("address").at("authoring_object_id") == 41);
        CHECK(operation.at("address").at("component").at("type") == "pelican_ray_instance");
        CHECK(operation.at("address").at("list_path") == "/overrides");
        CHECK(operation.at("value") == Json{{"occurrence_id", "visible"}});
        CHECK_FALSE(harness.model.rayOccurrences());
    }
    SECTION("same revision refresh still invalidates an older catalog response") {
        harness.model.requestRefresh();
        harness.reply(request, catalog);
        CHECK_FALSE(harness.model.rayOccurrences());
        CHECK_FALSE(harness.model.addRayOccurrenceOverride(key, "visible"));
    }
    SECTION("malformed catalog rows are rejected before display") {
        auto invalid = catalog;
        invalid["overrides"].push_back("not an object");
        harness.reply(request, invalid);
        CHECK_FALSE(harness.model.rayOccurrences());
        CHECK(harness.model.notice().kind == InspectorNoticeKind::Error);
    }
    SECTION("selection changes ignore delayed catalog responses") {
        harness.model.selectObject(OutlinerObjectKey{.scene_id = "main", .declaration_index = 0});
        harness.reply(request, catalog);
        CHECK_FALSE(harness.model.rayOccurrences());
    }
    SECTION("transport errors and session stop discard the catalog") {
        harness.model.receiveRpcFailure(request.request_id, "query failed");
        CHECK_FALSE(harness.model.rayOccurrences());
        CHECK(harness.model.notice().kind == InspectorNoticeKind::Error);
        REQUIRE(harness.model.requestRayOccurrences());
        harness.reply(harness.take("get_ray_occurrences"), catalog);
        REQUIRE(harness.model.rayOccurrences());
        harness.model.stopSession();
        CHECK_FALSE(harness.model.rayOccurrences());
    }
}

// Q-2. The Inspector's two clocks keep running -- they are also what flushes
// the model's request queue after a widget edit begins or a value is previewed
// -- so what matters is that a tick asks the engine for something only when
// there is an edit to chase or a selection to watch. These are the two
// questions the polls now ask themselves first, and they are the reason the
// panel costs zero RPC per second while idle.
TEST_CASE("Inspector polls only for an edit in flight and a watched selection",
          "[devstudio][inspector][poll][idle][negative-contrast][q2]") {
    InspectorHarness harness;
    // Nothing open: no session, no selection, nothing to chase.
    REQUIRE_FALSE(harness.model.pendingPollWanted());
    REQUIRE_FALSE(harness.model.externalWatchWanted());
    harness.model.pollPendingOperations();
    REQUIRE(harness.model.takeRpcRequests().empty());

    harness.open();
    // A selection under an open session is what the watch exists for; there is
    // still no edit to chase.
    REQUIRE(harness.model.externalWatchWanted());
    REQUIRE_FALSE(harness.model.pendingPollWanted());
    harness.model.pollPendingOperations();
    REQUIRE(harness.model.takeRpcRequests().empty());

    // An accepted edit is exactly what the 25 ms clock is for.
    const auto field_key = harness.field("transform", "pos").field_key;
    REQUIRE(harness.model.commitWidgetValue(
        field_key, Json::array({5.0, 2.0, 3.0})));
    const auto edit = harness.take("edit");
    REQUIRE_FALSE(harness.model.pendingPollWanted());
    harness.reply(edit, {{"status", "accepted"}, {"ticket", "edit:poll"}});
    REQUIRE(harness.model.pendingPollWanted());
    harness.model.pollPendingOperations();
    const auto chase = harness.take("get_edit_result");
    REQUIRE(chase.params.at("ticket") == "edit:poll");
    harness.reply(chase,
                  {{"status", "committed"}, {"committed_revision", 2}});
    (void)harness.model.takeRpcRequests();
    // Committed: nothing left to chase, and the selection is still watched.
    REQUIRE_FALSE(harness.model.pendingPollWanted());
    REQUIRE(harness.model.externalWatchWanted());

    // Losing the engine ends both.
    harness.model.stopSession("engine stopped");
    REQUIRE_FALSE(harness.model.externalWatchWanted());
    REQUIRE_FALSE(harness.model.pendingPollWanted());
}

TEST_CASE("G-2 Add object spawns once, named by kind with Blender's numbering",
          "[devstudio][inspector][add-object][g2]") {
    using PelicanStudio::AddObjectKind;
    using PelicanStudio::addObjectBaseName;
    using PelicanStudio::addObjectDeclaration;
    using PelicanStudio::uniqueObjectName;

    CHECK(uniqueObjectName("Cube", {}) == "Cube");
    CHECK(uniqueObjectName("Cube", {"Camera"}) == "Cube");
    CHECK(uniqueObjectName("Cube", {"Cube"}) == "Cube_001");
    CHECK(uniqueObjectName("Cube", {"Cube", "Cube_001"}) == "Cube_002");
    // The lowest free number, as Blender does, not one past the highest.
    CHECK(uniqueObjectName("Cube", {"Cube", "Cube_002"}) == "Cube_001");
    CHECK(uniqueObjectName("Sphere", {"Cube"}) == "Sphere");

    CHECK(addObjectDeclaration(AddObjectKind::Cube, "Cube") ==
          Json::parse(R"({"name":"Cube","components":[
              {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
              {"name":"simplemodelview","model":"engine://primitives/cube"}]})"));
    CHECK(addObjectDeclaration(AddObjectKind::Plane, "Plane").at("components").at(1).at("model") ==
          "engine://primitives/plane");
    CHECK(addObjectDeclaration(AddObjectKind::Sphere, "Sphere").at("components").at(1).at("model") ==
          "engine://primitives/sphere");
    const auto empty = addObjectDeclaration(AddObjectKind::Empty, "Empty");
    REQUIRE(empty.at("components").size() == 1);
    CHECK(empty.at("components").at(0).at("name") == "transform");

    // An editor session with nothing selected: Add object needs no selection.
    InspectorHarness harness;
    harness.model.startSession();
    harness.reply(harness.take("open_editor_session"),
                  {{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}});
    REQUIRE(harness.model.takeRpcRequests().empty());
    CHECK(harness.model.spawnObject("default_scene",
                                    addObjectDeclaration(AddObjectKind::Cube, "Cube")) ==
          SpawnOutcome::Refused);
    CHECK(harness.model.takeRpcRequests().empty());
    // The Outliner feeds the revision it read with scene_outline.
    harness.model.observeSceneRevision(3);

    // What MainWindow::addObject does: the names the Outliner shows, the
    // next free one, one spawn.
    std::vector<std::string> outliner{"Camera"};
    const auto add = [&](AddObjectKind kind) {
        const auto name = uniqueObjectName(addObjectBaseName(kind), outliner);
        REQUIRE(harness.model.spawnObject("default_scene", addObjectDeclaration(kind, name)) ==
                SpawnOutcome::Sent);
        return harness.take("edit");
    };

    const auto first = add(AddObjectKind::Cube);
    CHECK(first.params.at("actor_id") == 9);
    CHECK(first.params.at("base_revision") == 3);
    // Its own undo step: never coalesced into an Inspector property edit.
    CHECK_FALSE(first.params.contains("coalesce_key"));
    REQUIRE(first.params.at("operations").size() == 1);
    CHECK(first.params.at("operations").at(0) ==
          Json{{"op", "spawn"},
               {"scene_id", "default_scene"},
               {"object", addObjectDeclaration(AddObjectKind::Cube, "Cube")}});
    // One at a time: the next spawn would be stale against this one, so
    // (Q-7) it waits for it and goes out on the revision it leaves.
    CHECK(harness.model.spawnObject("default_scene",
                                    addObjectDeclaration(AddObjectKind::Cube, "Cube_001")) ==
          SpawnOutcome::Queued);
    CHECK(harness.model.takeRpcRequests().empty());
    harness.reply(first, {{"status", "accepted"}, {"ticket", "spawn:1"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_edit_result"),
                  {{"status", "committed"}, {"committed_revision", 4}});
    CHECK(harness.model.notice().kind == InspectorNoticeKind::Success);

    const auto second = harness.take("edit");
    CHECK(second.params.at("base_revision") == 4);
    CHECK(second.params.at("operations").at(0).at("object").at("name") == "Cube_001");
    CHECK(second.params.at("operations").at(0).at("object").at("components").at(1) ==
          Json{{"name", "simplemodelview"}, {"model", "engine://primitives/cube"}});
}

TEST_CASE("T-1 Add Object > Floor spawns a wide plane just under the origin",
          "[devstudio][inspector][add-object][t1]") {
    using PelicanStudio::AddObjectKind;
    using PelicanStudio::addObjectBaseName;
    using PelicanStudio::addObjectDeclaration;
    using PelicanStudio::uniqueObjectName;

    CHECK(std::string{addObjectBaseName(AddObjectKind::Floor)} == "Floor");
    CHECK(addObjectDeclaration(AddObjectKind::Floor, "Floor") ==
          Json::parse(R"({"name":"Floor","components":[
              {"name":"transform","pos":[0,-0.5,0],"rotation":[0,0,0,1],"scale":[8,1,8]},
              {"name":"simplemodelview","model":"engine://primitives/plane"}]})"));
    // Numbered as every other kind is.
    CHECK(uniqueObjectName(addObjectBaseName(AddObjectKind::Floor), {"Floor"}) == "Floor_001");
    // The other kinds are where they were.
    CHECK(addObjectDeclaration(AddObjectKind::Plane, "Plane").at("components").at(0).at("pos") ==
          Json::array({0.0, 0.0, 0.0}));
    CHECK(addObjectDeclaration(AddObjectKind::Sphere, "Sphere").at("components").at(1).at("model") ==
          "engine://primitives/sphere");
}

TEST_CASE("T-1 rotation angles and the stored quaternion round trip",
          "[devstudio][inspector][t1][euler]") {
    using PelicanStudio::eulerDegreesFromQuaternion;
    using PelicanStudio::quaternionFromEulerDegrees;
    const auto near = [](double a, double b, double tolerance = 1.0e-9) {
        return std::abs(a - b) <= tolerance;
    };

    // The rotation G-5 typed as two quaternion components: 45 degrees about Y.
    const auto turned = quaternionFromEulerDegrees({0.0, 45.0, 0.0});
    CHECK(turned[0] == 0.0);
    CHECK(near(turned[1], 0.38268343236508984));
    CHECK(turned[2] == 0.0);
    CHECK(near(turned[3], 0.92387953251128674));
    // No rotation is exactly what the four fields held before.
    CHECK(quaternionFromEulerDegrees({0.0, 0.0, 0.0}) == std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
    CHECK(eulerDegreesFromQuaternion({0.0, 0.0, 0.0, 1.0}) == std::array<double, 3>{0.0, 0.0, 0.0});

    // X is applied first: 90 about X then 90 about Z is Rz * Rx.
    const auto order = quaternionFromEulerDegrees({90.0, 0.0, 90.0});
    CHECK(near(order[0], 0.5));
    CHECK(near(order[1], 0.5));
    CHECK(near(order[2], 0.5));
    CHECK(near(order[3], 0.5));

    // Angles -> quaternion -> angles gives the angles back, over the range the
    // fields show (X, Z in (-180, 180], Y in (-90, 90)).
    for (const double x : {-179.0, -90.0, -33.5, 0.0, 12.25, 90.0, 179.5})
        for (const double y : {-89.0, -45.0, 0.0, 30.0, 89.0})
            for (const double z : {-170.0, -1.0, 0.0, 45.0, 135.0, 179.5}) {
                const auto back = eulerDegreesFromQuaternion(quaternionFromEulerDegrees({x, y, z}));
                INFO(x << ", " << y << ", " << z << " -> " << back[0] << ", " << back[1] << ", "
                       << back[2]);
                CHECK(near(back[0], x, 1.0e-7));
                CHECK(near(back[1], y, 1.0e-7));
                CHECK(near(back[2], z, 1.0e-7));
            }

    // Quaternion -> angles -> quaternion gives the stored value back (w >= 0),
    // gimbal lock included.
    const std::array<std::array<double, 4>, 6> stored{{
        {0.0, 0.38268343236508984, 0.0, 0.92387953251128674},
        {0.1, -0.2, 0.3, 0.9273618495495703},
        {-0.5, 0.5, -0.5, 0.5},
        {0.0, 0.70710678118654757, 0.0, 0.70710678118654757},   // Y = +90
        {0.0, -0.70710678118654757, 0.0, 0.70710678118654757},  // Y = -90
        {0.2705980500730985, 0.6532814824381882, 0.2705980500730985, 0.6532814824381883},
    }};
    for (const auto &quaternion : stored) {
        const double length = std::sqrt(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
                                        quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
        const auto back = quaternionFromEulerDegrees(eulerDegreesFromQuaternion(quaternion));
        for (std::size_t index = 0; index < 4; ++index) {
            INFO("component " << index);
            CHECK(near(back[index], quaternion[index] / length, 1.0e-9));
        }
    }
    // A quaternion stored unnormalised reads as the rotation it scales; a
    // zero one as no rotation.
    const auto scaled = eulerDegreesFromQuaternion({0.0, 0.76536686473017968, 0.0, 1.8477590650225735});
    CHECK(near(scaled[1], 45.0, 1.0e-7));
    CHECK(eulerDegreesFromQuaternion({0.0, 0.0, 0.0, 0.0}) == std::array<double, 3>{0.0, 0.0, 0.0});
    // -q is q: read back with w >= 0.
    const auto negated = quaternionFromEulerDegrees(eulerDegreesFromQuaternion({0.0, -0.38268343236508984, 0.0, -0.92387953251128674}));
    CHECK(near(negated[1], 0.38268343236508984));
    CHECK(near(negated[3], 0.92387953251128674));
}

TEST_CASE("UX-14 decision 3: a light's azimuth turns as a camera.controller's yaw does -- 0 is +Z, "
          "growing towards -X -- and the yaw, pitch and azimuth fields say so",
          "[devstudio][inspector][ux14]") {
    using PelicanStudio::directionFromAnglesDegrees;
    const auto near = [](double a, double b) { return std::abs(a - b) <= 1.0e-9; };
    // The engine's controller (CC-1): direction(yaw, pitch) =
    // (+Z cos yaw + (-X) sin yaw) cos pitch + (+Y) sin pitch. The same angles
    // in a light's fields point the same way.
    constexpr double DegreesPerRadian = 57.295779513082320876798154814105;
    for (const double yaw : {-150.0, -63.43, 0.0, 30.0, 90.0, 179.0})
        for (const double pitch : {-60.0, 0.0, 20.0}) {
            const double y = yaw / DegreesPerRadian;
            const double p = pitch / DegreesPerRadian;
            const std::array<double, 3> engine{-std::sin(y) * std::cos(p), std::sin(p),
                                               std::cos(y) * std::cos(p)};
            const auto light = directionFromAnglesDegrees({yaw, pitch});
            INFO(yaw << ", " << pitch);
            CHECK(near(light[0], engine[0]));
            CHECK(near(light[1], engine[1]));
            CHECK(near(light[2], engine[2]));
        }
    // The blank template's KeyLight (-0.5, -1, 0.25) travels towards -X: it
    // comes from the screen's right of the template's camera, azimuth +63.43.
    CHECK(std::abs(PelicanStudio::directionAnglesDegrees({-0.5, -1.0, 0.25})[0] - 63.43) < 0.01);

    InspectorWidgetDescriptor yaw;
    yaw.component_slot = "camera";
    yaw.field_name = "controller.yaw";
    yaw.kind = InspectorWidgetKind::FloatingPointDrag;
    CHECK(PelicanStudio::inspectorAngleTip(yaw).find("from +Z towards -X") != std::string::npos);
    InspectorWidgetDescriptor pitch = yaw;
    pitch.field_name = "controller.pitch";
    CHECK(PelicanStudio::inspectorAngleTip(pitch).find("towards +Y") != std::string::npos);
    InspectorWidgetDescriptor other = yaw;
    other.field_name = "znear";
    CHECK(PelicanStudio::inspectorAngleTip(other).empty());
    CHECK(PelicanStudio::inspectorDirectionAngleTip(0).find("from +Z towards -X") != std::string::npos);
}

TEST_CASE("UX-10 a light's direction reads as azimuth and elevation, its colour as a swatch, "
          "and a sprite's texture is picked from the textures list_assets reports",
          "[devstudio][inspector][ux10]") {
    using PelicanStudio::directionAnglesDegrees;
    using PelicanStudio::directionFromAnglesDegrees;
    const auto near = [](double a, double b, double tolerance = 1.0e-9) {
        return std::abs(a - b) <= tolerance;
    };
    // Which fields: by name and by type, not by component.
    InspectorWidgetDescriptor field;
    field.component_slot = "light";
    field.field_name = "direction";
    field.json_pointer = "/direction";
    field.kind = InspectorWidgetKind::VectorDrag;
    field.columns = 3;
    CHECK(PelicanStudio::isDirectionField(field));
    CHECK_FALSE(PelicanStudio::isColorField(field));
    field.field_name = "color";
    CHECK(PelicanStudio::isColorField(field));
    CHECK_FALSE(PelicanStudio::isDirectionField(field));
    field.columns = 4;
    CHECK(PelicanStudio::isColorField(field));
    field.field_name = "position";
    field.columns = 3;
    CHECK_FALSE(PelicanStudio::isColorField(field));
    CHECK_FALSE(PelicanStudio::isDirectionField(field));
    field.field_name = "direction";
    field.kind = InspectorWidgetKind::StringInput;
    CHECK_FALSE(PelicanStudio::isDirectionField(field));

    // The light's default (0, -1, 0) is straight down: elevation -90, and the
    // angles give it back exactly.
    const auto down = directionAnglesDegrees({0.0, -1.0, 0.0});
    CHECK(down[0] == 0.0);
    CHECK(near(down[1], -90.0));
    CHECK(directionFromAnglesDegrees({0.0, -90.0}) == std::array<double, 3>{0.0, -1.0, 0.0});
    // +Z is azimuth 0, -X azimuth 90 (UX-14 decision 3: as a camera's yaw),
    // on the horizon.
    CHECK(directionFromAnglesDegrees({0.0, 0.0}) == std::array<double, 3>{0.0, 0.0, 1.0});
    CHECK(directionFromAnglesDegrees({90.0, 0.0}) == std::array<double, 3>{-1.0, 0.0, 0.0});
    // A vector that is not unit reads as its direction; zero as (0, 0).
    const auto slanted = directionAnglesDegrees({-2.0, -2.0, 0.0});
    CHECK(near(slanted[0], 90.0));
    CHECK(near(slanted[1], -45.0));
    CHECK(directionAnglesDegrees({0.0, 0.0, 0.0}) == std::array<double, 2>{0.0, 0.0});
    // Angles -> vector -> angles round trip over what the fields show.
    for (const double azimuth : {-179.0, -90.0, -30.5, 0.0, 45.0, 135.0, 180.0})
        for (const double elevation : {-89.0, -45.0, 0.0, 10.25, 89.0}) {
            const auto vector = directionFromAnglesDegrees({azimuth, elevation});
            CHECK(near(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2], 1.0));
            const auto back = directionAnglesDegrees(vector);
            INFO(azimuth << ", " << elevation << " -> " << back[0] << ", " << back[1]);
            CHECK(near(back[0], azimuth, 1.0e-7));
            CHECK(near(back[1], elevation, 1.0e-7));
        }

    // UX-10 decision 5: sprite_view.texture is a choice, from list_assets'
    // textures, the authored frame kept.
    InspectorWidgetDescriptor texture;
    texture.component_slot = "sprite_view";
    texture.field_name = "texture";
    texture.json_pointer = "/texture";
    texture.kind = InspectorWidgetKind::StringInput;
    CHECK(PelicanStudio::isTextureReferenceField(texture));
    texture.component_slot = "simplemodelview";
    CHECK_FALSE(PelicanStudio::isTextureReferenceField(texture));
    const Json listed{{"assets",
                       Json::array({{{"id", "engine://primitives/cube"}, {"kind", "model"}},
                                    {{"id", "demo_atlas"}, {"kind", "texture"}},
                                    {{"id", "grass"}, {"kind", "texture"}}})}};
    const auto textures = PelicanStudio::assetIdsOfKind(listed, "texture");
    CHECK(textures == std::vector<std::string>{"demo_atlas", "grass"});
    CHECK(PelicanStudio::modelAssetIds(listed) == std::vector<std::string>{"engine://primitives/cube"});
    CHECK(PelicanStudio::textureReferenceChoices("demo_atlas#sprite/full", textures) ==
          std::vector<std::string>{"demo_atlas", "grass", "demo_atlas#sprite/full"});
    CHECK(PelicanStudio::textureReferenceChoices("grass", textures) ==
          std::vector<std::string>{"demo_atlas", "grass"});
}

TEST_CASE("UX-10 decision 6: camera.controller's values are shown for the controller.type that "
          "reads them, a value it requires and lacks is marked, and yaw and pitch read in degrees",
          "[devstudio][inspector][ux10]") {
    // SC-1's wire, written by hand (the engine this worktree builds has not
    // got it yet).
    const auto value = [](const char *name, const char *type, Json applies, Json required) {
        return Json{{"name", name},
                    {"type", type},
                    {"discriminant", "controller.type"},
                    {"applies_to", std::move(applies)},
                    {"required_for", std::move(required)}};
    };
    Json component{
        {"name", "camera"},
        {"component_index", 0},
        {"authored_json", {{"controller", {{"type", "follow"}}}}},
        {"schema",
         {{"state", "available"},
          {"fields",
           Json::array({{{"name", "controller.type"},
                         {"type", "enum"},
                         {"enum", Json::array({"orbit", "follow", "fly"})},
                         {"default", "orbit"}},
                        value("controller.target", "string", {"orbit", "follow"}, {"follow"}),
                        value("controller.distance", "f32", {"orbit", "follow"}, Json::array()),
                        value("controller.yaw", "f32", {"orbit", "fly"}, Json::array()),
                        value("controller.speed", "f32", {"fly"}, Json::array()),
                        {{"name", "znear"}, {"type", "f32"}}})}}},
    };
    const auto shown = [](const std::vector<InspectorWidgetDescriptor> &plan) {
        std::vector<std::string> names;
        for (const auto &field : plan)
            if (PelicanStudio::inspectorFieldShown(field, plan)) names.push_back(field.field_name);
        return names;
    };
    const auto find = [](const std::vector<InspectorWidgetDescriptor> &plan, std::string_view name)
        -> const InspectorWidgetDescriptor & {
        const auto found = std::ranges::find(plan, name, &InspectorWidgetDescriptor::field_name);
        REQUIRE(found != plan.end());
        return *found;
    };

    auto plan = PelicanStudio::makeInspectorWidgetPlan(component, 7);
    REQUIRE(plan.size() == 6);
    CHECK(find(plan, "controller.target").discriminant == "controller.type");
    CHECK(find(plan, "controller.target").required_for == std::vector<std::string>{"follow"});
    // follow: its target (required, not given: marked) and its distance.
    CHECK(shown(plan) == std::vector<std::string>{"controller.type", "controller.target",
                                                  "controller.distance", "znear"});
    CHECK(PelicanStudio::inspectorFieldRequiredMissing(find(plan, "controller.target"), plan));
    CHECK_FALSE(PelicanStudio::inspectorFieldRequiredMissing(find(plan, "controller.distance"), plan));
    component["authored_json"]["controller"]["target"] = "Player";
    plan = PelicanStudio::makeInspectorWidgetPlan(component, 7);
    CHECK_FALSE(PelicanStudio::inspectorFieldRequiredMissing(find(plan, "controller.target"), plan));
    component["authored_json"]["controller"]["target"] = "";
    plan = PelicanStudio::makeInspectorWidgetPlan(component, 7);
    CHECK(PelicanStudio::inspectorFieldRequiredMissing(find(plan, "controller.target"), plan));

    // fly: its yaw and speed; nothing of follow's.
    component["authored_json"]["controller"] = {{"type", "fly"}};
    plan = PelicanStudio::makeInspectorWidgetPlan(component, 7);
    CHECK(shown(plan) == std::vector<std::string>{"controller.type", "controller.yaw",
                                                  "controller.speed", "znear"});
    // No type written: the schema's default (orbit) decides.
    component["authored_json"] = Json::object();
    plan = PelicanStudio::makeInspectorWidgetPlan(component, 7);
    CHECK(shown(plan) == std::vector<std::string>{"controller.type", "controller.target",
                                                  "controller.distance", "controller.yaw", "znear"});
    // The yaw is stored in radians and read in degrees; another number is not.
    CHECK(std::abs(PelicanStudio::inspectorDisplayScale(find(plan, "controller.yaw")) -
                   57.295779513082320876798154814105) < 1e-12);
    CHECK(PelicanStudio::inspectorDisplayScale(find(plan, "controller.speed")) == 1.0);
    CHECK(PelicanStudio::inspectorDisplayScale(find(plan, "znear")) == 1.0);
}

TEST_CASE("Q-7 Add object during an edit waits for it and spawns once",
          "[devstudio][inspector][add-object][q7][negative-contrast]") {
    using PelicanStudio::AddObjectKind;
    using PelicanStudio::addObjectDeclaration;

    InspectorHarness harness;
    harness.open();
    const std::string field_key = harness.field("transform", "pos").field_key;

    // A transform edit is committing: a live preview, released.
    REQUIRE(harness.model.previewWidgetValue(field_key, Json::array({4.0, 2.0, 3.0})));
    const auto open = harness.take("open_preview");
    harness.reply(open, {{"status", "accepted"},
                         {"ticket", "preview:1"},
                         {"request_id", "preview-request:open"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_preview_result"), {{"status", "open"}});
    REQUIRE(harness.model.finishWidgetEdit(field_key, true));
    const auto commit = harness.take("commit_preview");
    REQUIRE(harness.model.busy());

    // Add object now does not refuse: it waits. Nothing goes out yet -- the
    // spawn's base revision would be stale against the commit.
    const auto cube = addObjectDeclaration(AddObjectKind::Cube, "Cube");
    CHECK(harness.model.spawnObject("default_scene", cube) == PelicanStudio::SpawnOutcome::Queued);
    CHECK(harness.model.queuedSpawnName() == std::optional<std::string>{"Cube"});
    CHECK(harness.model.notice().kind == InspectorNoticeKind::Information);
    CHECK(harness.model.takeRpcRequests().empty());

    // One waits. The second is refused, and says which one is waiting.
    CHECK(harness.model.spawnObject("default_scene",
                                    addObjectDeclaration(AddObjectKind::Plane, "Plane")) ==
          PelicanStudio::SpawnOutcome::Refused);
    CHECK(harness.model.notice().kind == InspectorNoticeKind::Error);
    CHECK(harness.model.notice().message.find("Cube is already waiting") != std::string::npos);
    CHECK(harness.model.queuedSpawnName() == std::optional<std::string>{"Cube"});

    // The commit lands at revision 2: the spawn goes out right after it, on
    // that revision, next to the refresh the commit asks for.
    harness.reply(commit, {{"status", "accepted"}, {"request_id", "preview-request:commit"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_preview_result"),
                  {{"status", "committed"}, {"committed_revision", 2}});
    auto requests = harness.model.takeRpcRequests();
    int spawns = 0;
    for (const auto &request : requests) {
        if (request.method != "edit") continue;
        ++spawns;
        CHECK(request.params.at("base_revision") == 2);
        CHECK(request.params.at("operations") ==
              Json::array({Json{{"op", "spawn"}, {"scene_id", "default_scene"}, {"object", cube}}}));
        harness.reply(request, {{"status", "accepted"}, {"ticket", "spawn:1"}});
    }
    CHECK(spawns == 1);
    CHECK_FALSE(harness.model.queuedSpawnName());
    for (const auto &request : requests)
        if (request.method == "scene_outline") harness.reply(request, sceneTreeResult(2));

    // Once: nothing else is sent for it, however often the panel asks.
    harness.model.pollPendingOperations();
    requests = harness.model.takeRpcRequests();
    for (const auto &request : requests) CHECK(request.method != "edit");

    // An engine that stops takes a waiting spawn with it.
    CHECK(harness.model.spawnObject("default_scene",
                                    addObjectDeclaration(AddObjectKind::Sphere, "Sphere")) ==
          PelicanStudio::SpawnOutcome::Queued);
    harness.model.stopSession("engine stopped");
    CHECK_FALSE(harness.model.queuedSpawnName());
}

TEST_CASE("Q-7 a released edit cannot be begun again while it commits",
          "[devstudio][inspector][preview][q7][negative-contrast]") {
    InspectorHarness harness;
    harness.open();
    const std::string field_key = harness.field("transform", "pos").field_key;

    REQUIRE(harness.model.previewWidgetValue(field_key, Json::array({4.0, 2.0, 3.0})));
    harness.reply(harness.take("open_preview"), {{"status", "accepted"},
                                                 {"ticket", "preview:1"},
                                                 {"request_id", "preview-request:open"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_preview_result"), {{"status", "open"}});
    REQUIRE(harness.model.finishWidgetEdit(field_key, true));
    const auto commit = harness.take("commit_preview");

    // The panel let go of the field. A begin now -- what focus arriving on
    // the next column used to do -- would hold the Inspector for an edit
    // nobody is making, and nothing would ever finish it.
    CHECK_FALSE(harness.model.beginWidgetEdit(field_key));

    harness.reply(commit, {{"status", "accepted"}, {"request_id", "preview-request:commit"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_preview_result"),
                  {{"status", "committed"}, {"committed_revision", 2}});
    harness.completeRefresh(4.0, 2);
    CHECK_FALSE(harness.model.busy());
    CHECK_FALSE(harness.model.refreshBlocked());
    // And afterwards the field begins as any field does.
    CHECK(harness.model.beginWidgetEdit(field_key));
}

TEST_CASE("G-2 the simplemodelview model field offers the three primitives",
          "[devstudio][inspector][add-object][g2]") {
    using PelicanStudio::builtinModelLabel;
    using PelicanStudio::isModelReferenceField;
    using PelicanStudio::modelReferenceChoices;

    const std::vector<std::string> primitives{
        "engine://primitives/cube", "engine://primitives/plane", "engine://primitives/sphere"};
    CHECK(PelicanStudio::enginePrimitiveModelReferences() == primitives);
    CHECK(builtinModelLabel("engine://primitives/cube") == "Cube (built-in)");
    CHECK(builtinModelLabel("engine://primitives/sphere") == "Sphere (built-in)");
    CHECK(builtinModelLabel("checker_box").empty());

    const auto values = [](const auto &choices) {
        std::vector<std::string> result;
        for (const auto &choice : choices) result.push_back(choice.value);
        return result;
    };
    // Studio's fallback: the three, labelled, before the engine says anything.
    const auto fallback = modelReferenceChoices("engine://primitives/plane");
    CHECK(values(fallback) == primitives);
    CHECK(fallback.at(0).label == "Cube (built-in)");
    CHECK(fallback.at(0).builtin);
    CHECK(values(modelReferenceChoices("")) == primitives);
    // The authored value stays choosable whatever it is.
    auto with_asset = primitives;
    with_asset.emplace_back("checker_box");
    CHECK(values(modelReferenceChoices("checker_box")) == with_asset);
    CHECK(modelReferenceChoices("checker_box").back().label == "checker_box");
    CHECK_FALSE(modelReferenceChoices("checker_box").back().builtin);
    // GP-5b: the engine lists the three first too. Each is shown once, and the
    // project's models follow them.
    const Json listed{{"assets", Json::array({
        {{"id", "engine://primitives/cube"}, {"kind", "model"}, {"path", "engine://primitives/cube"},
         {"store", "engine"}, {"status", "builtin"}},
        {{"id", "engine://primitives/plane"}, {"kind", "model"}, {"path", "engine://primitives/plane"},
         {"store", "engine"}, {"status", "builtin"}},
        {{"id", "engine://primitives/sphere"}, {"kind", "model"}, {"path", "engine://primitives/sphere"},
         {"store", "engine"}, {"status", "builtin"}},
        {{"id", "checker_box"}, {"kind", "model"}, {"path", "project://checker_box.glb"},
         {"store", "project"}, {"status", "ok"}},
        {{"id", "sky"}, {"kind", "texture"}, {"path", "project://sky.png"},
         {"store", "project"}, {"status", "ok"}}})}};
    const auto engine_models = PelicanStudio::modelAssetIds(listed);
    CHECK(engine_models == std::vector<std::string>{
        "engine://primitives/cube", "engine://primitives/plane", "engine://primitives/sphere",
        "checker_box"});
    const auto merged = modelReferenceChoices("engine://primitives/cube", engine_models);
    CHECK(values(merged) == with_asset);

    const Json component{
        {"name", "simplemodelview"},
        {"component_index", 1},
        {"authored_json", {{"model", "engine://primitives/cube"}, {"label", "box"}}},
        {"schema", {{"state", "available"},
                    {"fields", Json::array({{{"name", "model"}, {"type", "string"}},
                                            {{"name", "label"}, {"type", "string"}}})}}}};
    const auto plan = PelicanStudio::makeInspectorWidgetPlan(component, 41);
    REQUIRE(plan.size() == 2);
    CHECK(plan.at(0).kind == InspectorWidgetKind::StringInput);
    CHECK(isModelReferenceField(plan.at(0)));
    CHECK_FALSE(isModelReferenceField(plan.at(1)));
    auto other = plan.at(0);
    other.component_slot = "transform";
    CHECK_FALSE(isModelReferenceField(other));

    // Loading an object with a simplemodelview asks list_assets once, and
    // its answer is what the field lists next to the primitives.
    InspectorHarness harness;
    harness.model.selectObject(OutlinerObjectKey{.scene_id = "main", .declaration_index = 2});
    harness.model.startSession();
    harness.reply(harness.take("open_editor_session"),
                  {{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}});
    harness.reply(harness.take("scene_outline"), sceneTreeResult());
    auto result = componentResult();
    auto model_view = component;
    model_view["component_index"] = 2;
    model_view["editable"] = true;
    model_view["pending"] = false;
    model_view["codec"] = {{"state", "registered"}};
    result.at("components").push_back(model_view);
    harness.reply(harness.take("get_components"), result);
    auto requests = harness.model.takeRpcRequests();
    REQUIRE(requests.size() == 2);
    const auto assets = std::find_if(requests.begin(), requests.end(),
                                     [](const auto &request) { return request.method == "list_assets"; });
    REQUIRE(assets != requests.end());
    CHECK(assets->params == Json::object());
    CHECK_FALSE(harness.model.modelAssets());
    const auto revision = harness.model.modelAssetsRevision();
    harness.reply(*assets, listed);
    REQUIRE(harness.model.modelAssets());
    CHECK(*harness.model.modelAssets() == engine_models);
    CHECK(harness.model.modelAssetsRevision() != revision);
}

TEST_CASE("Q-6 a component this build cannot take reads as a sentence and its code",
          "[devstudio][inspector][component-addition][q6]") {
    using PelicanStudio::componentAddRows;
    using PelicanStudio::componentUnavailableDescription;
    using PelicanStudio::componentUnavailableSentence;
    using PelicanStudio::componentUnavailableTag;
    using PelicanStudio::isComponentUnavailableCode;

    struct Expected {
        std::string code;
        std::string sentence;
        std::string tag;
    };
    const std::vector<Expected> known{
        {"animation.module_unavailable@1", "This build has no animation module", "not in this build"},
        {"audio.module_unavailable@1", "This build has no audio module", "not in this build"},
        {"physics.module_unavailable@1", "This build has no physics module", "not in this build"},
        {"xr.module_unavailable@1", "This build has no XR module", "not in this build"},
        {"vat.module_unavailable@1", "This build has no VAT module", "not in this build"},
        {"seqplayer.module_unavailable@1", "This build has no sequence player module", "not in this build"},
        // MG-2a: layouts are v3 only; the code now means "no layout at all".
        {"pelican.ray_instance_requires_layout_v2@1", "This project has no ray group layout",
         "needs a ray layout"},
    };
    for (const auto &entry : known) {
        CAPTURE(entry.code);
        CHECK(isComponentUnavailableCode(entry.code));
        CHECK(componentUnavailableSentence(entry.code) == entry.sentence);
        const auto description = componentUnavailableDescription(entry.code);
        CHECK(description == entry.sentence + " (" + entry.code + ")");
        // The description panel shows one line of at most 90 characters and
        // cuts at the first ". "; the whole line has to survive both.
        CHECK(description.size() <= 90);
        CHECK(description.find(". ") == std::string::npos);
        CHECK(componentUnavailableTag(entry.code) == entry.tag);
    }

    // A code Studio does not know reads as itself.
    CHECK(isComponentUnavailableCode("teleport.module_unavailable@2"));
    CHECK(componentUnavailableSentence("teleport.module_unavailable@2").empty());
    CHECK(componentUnavailableDescription("teleport.module_unavailable@2") ==
          "teleport.module_unavailable@2");
    CHECK(componentUnavailableTag("teleport.module_unavailable@2") == "unavailable");
    // A default that only needs input is a sentence, not a code.
    CHECK_FALSE(isComponentUnavailableCode("A model reference is required."));
    CHECK_FALSE(isComponentUnavailableCode("Set speed."));
    CHECK_FALSE(isComponentUnavailableCode(""));
    CHECK_FALSE(isComponentUnavailableCode("module@"));
    CHECK_FALSE(isComponentUnavailableCode("physics@1"));

    const Json catalog{{"object_id", 41}, {"scene_revision", 1}, {"candidates", Json::array({
        {{"key", "physics_body"}, {"name", "physics_body"}, {"available", false},
         {"unavailable_reason", "physics.module_unavailable@1"}},
        {{"key", "collider"}, {"name", "collider"}, {"available", true}},
        {{"key", "pelican_ray_instance"}, {"name", "pelican_ray_instance"}, {"available", false},
         {"unavailable_reason", "pelican.ray_instance_requires_layout_v2@1"}},
        {{"key", "simplemodelview"}, {"name", "simplemodelview"}, {"available", false},
         {"unavailable_reason", "A model reference is required."}},
        {{"key", "animator"}, {"name", "animator"}, {"available", false},
         {"unavailable_reason", "teleport.module_unavailable@2"}},
        {{"key", "behavior:spin"}, {"name", "behavior"}, {"behavior_type", "spin"}, {"available", true}},
    })}};
    const auto rows = componentAddRows(catalog);
    REQUIRE(rows.size() == 6);
    const std::string dash = "\xE2\x80\x94";
    // The addable ones first, then the rest, each in the catalog's order.
    std::vector<std::string> keys;
    for (const auto &row : rows) keys.push_back(row.key);
    CHECK(keys == std::vector<std::string>{"collider", "simplemodelview", "behavior:spin",
                                           "physics_body", "pelican_ray_instance", "animator"});
    CHECK(rows.at(0).candidate == 1);
    CHECK(rows.at(0).label == "collider");
    CHECK(rows.at(0).addable);
    CHECK(rows.at(0).blocked.empty());
    // Needs input, stays addable: the declaration is where the input goes.
    CHECK(rows.at(1).label == "simplemodelview");
    CHECK(rows.at(1).addable);
    CHECK(rows.at(2).label == "Behavior: spin");
    CHECK(rows.at(3).candidate == 0);
    CHECK(rows.at(3).label == "physics_body " + dash + " not in this build");
    CHECK_FALSE(rows.at(3).addable);
    CHECK(rows.at(3).blocked == "This build has no physics module (physics.module_unavailable@1)");
    CHECK(rows.at(4).label == "pelican_ray_instance " + dash + " needs a ray layout");
    CHECK(rows.at(4).blocked ==
          "This project has no ray group layout (pelican.ray_instance_requires_layout_v2@1)");
    CHECK_FALSE(rows.at(4).addable);
    CHECK(rows.at(5).label == "animator " + dash + " unavailable");
    CHECK(rows.at(5).blocked == "teleport.module_unavailable@2");
    CHECK(componentAddRows(Json::object()).empty());
}

TEST_CASE("Q-10 a sound whose extension and contents disagree reads as one sentence",
          "[devstudio][inspector][component-addition][q10]") {
    using PelicanStudio::componentAddRows;
    using PelicanStudio::componentUnavailableDescription;
    using PelicanStudio::knownRefusalDescription;

    const std::string code = "audio.format_unsupported@1";
    const std::string sentence = "Sound extension and contents disagree (WAV or Ogg Vorbis)";
    CHECK(PelicanStudio::isComponentUnavailableCode(code));
    CHECK(PelicanStudio::componentUnavailableSentence(code) == sentence);
    const auto description = componentUnavailableDescription(code);
    CHECK(description == sentence + " (" + code + ")");
    // The same one-line limits as the rest of Q-6's table.
    CHECK(description.size() <= 90);
    CHECK(description.find(". ") == std::string::npos);
    CHECK(PelicanStudio::componentUnavailableTag(code) == "unsupported sound");

    // Add component: a candidate refused with the code reads as the sentence.
    const Json catalog{{"candidates", Json::array({
        {{"key", "voice"}, {"name", "voice"}, {"available", false},
         {"unavailable_reason", code}},
    })}};
    const auto rows = componentAddRows(catalog);
    REQUIRE(rows.size() == 1);
    CHECK_FALSE(rows.at(0).addable);
    CHECK(rows.at(0).blocked == description);
    CHECK(rows.at(0).label == "voice \xE2\x80\x94 unsupported sound");

    // As its own code, or inside the message of a broader refusal -- the way
    // a graph edit's render pipeline preparation carries it.
    CHECK(knownRefusalDescription(code, "") == description);
    CHECK(knownRefusalDescription(
              "render_pipeline_preflight_failed",
              code + R"( {"asset":"project://a.ogg","extension":".ogg","signature":"wav"})") ==
          description);
    // Nothing Studio knows: nothing claimed.
    CHECK(knownRefusalDescription("render_pipeline_preflight_failed", "Invalid WAV").empty());
    CHECK(knownRefusalDescription("", "").empty());
}

TEST_CASE("OL-1 an Outliner edit is one edit on the session, and its end names what it created",
          "[devstudio][inspector][outliner][ol1]") {
    using PelicanStudio::SceneEditKind;
    InspectorHarness harness;
    harness.model.startSession();
    harness.reply(harness.take("open_editor_session"),
                  {{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}});
    REQUIRE(harness.model.takeRpcRequests().empty());
    // Nothing selected in the Inspector, and no revision read yet: not ready.
    CHECK(harness.model.submitSceneEdit(SceneEditKind::Duplicate, 2,
                                        Json{{"op", "duplicate"}, {"object_id", 2}},
                                        "Duplicate Cube") == SpawnOutcome::Refused);
    CHECK(harness.model.takeRpcRequests().empty());
    harness.model.observeSceneRevision(5);

    // Duplicate Cube: exactly the op the Outliner built, on the revision it
    // read, as its own undo step.
    REQUIRE(harness.model.submitSceneEdit(SceneEditKind::Duplicate, 2,
                                          Json{{"op", "duplicate"}, {"object_id", 2}},
                                          "Duplicate Cube") == SpawnOutcome::Sent);
    const auto duplicate = harness.take("edit");
    CHECK(duplicate.params == Json{{"actor_id", 9},
                                   {"base_revision", 5},
                                   {"operations", Json::array({{{"op", "duplicate"},
                                                                {"object_id", 2}}})}});
    // One at a time: a second Outliner edit while this one is in flight is
    // refused, not queued behind it.
    CHECK(harness.model.submitSceneEdit(SceneEditKind::Destroy, 2,
                                        Json{{"op", "destroy"}, {"object_id", 2}},
                                        "Delete Cube") == SpawnOutcome::Refused);
    CHECK(harness.model.takeRpcRequests().empty());
    CHECK_FALSE(harness.model.lastSceneEdit());

    harness.reply(duplicate, {{"status", "accepted"}, {"ticket", "edit:1"}});
    harness.model.pollPendingOperations();
    const Json created = Json::array({{{"object_id", 6},
                                       {"scene_id", "main"},
                                       {"declaration_index", 2},
                                       {"name", "Cube_copy1"},
                                       {"source_object_id", 2}}});
    harness.reply(harness.take("get_edit_result"),
                  {{"status", "committed"},
                   {"committed_revision", 6},
                   {"affected_authoring_ids", Json::array({6})},
                   {"created_objects", created}});
    const auto &done = harness.model.lastSceneEdit();
    REQUIRE(done);
    CHECK(done->kind == SceneEditKind::Duplicate);
    CHECK(done->subject_id == 2);
    CHECK(done->committed);
    CHECK(done->created_objects == created);
    const auto first_serial = done->serial;

    // A move under its own child: the engine's code comes back with it.
    REQUIRE(harness.model.submitSceneEdit(
                SceneEditKind::Reparent, 3,
                Json{{"op", "reparent"}, {"object_id", 3}, {"new_parent_id", 5},
                     {"preserve", "world"}},
                "Move Floor under Toe") == SpawnOutcome::Sent);
    const auto move = harness.take("edit");
    CHECK(move.params.at("base_revision") == 6);
    harness.reply(move, {{"status", "rejected"},
                         {"error", {{"code", "cycle_detected"}, {"message", "cycle"}}}});
    const auto &refused = harness.model.lastSceneEdit();
    REQUIRE(refused);
    CHECK(refused->serial > first_serial);
    CHECK(refused->kind == SceneEditKind::Reparent);
    CHECK_FALSE(refused->committed);
    CHECK(refused->code == "cycle_detected");
    CHECK(PelicanStudio::sceneEditRefusalSentence(*refused, "Floor", "Toe") ==
          "Cannot move Floor under Toe: Toe is inside Floor.");
}

TEST_CASE("OL-1 a spawn's end names the object it created",
          "[devstudio][inspector][add-object][ol1]") {
    using PelicanStudio::AddObjectKind;
    using PelicanStudio::SceneEditKind;
    using PelicanStudio::addObjectDeclaration;
    InspectorHarness harness;
    harness.model.startSession();
    harness.reply(harness.take("open_editor_session"),
                  {{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}});
    harness.model.observeSceneRevision(3);
    REQUIRE(harness.model.spawnObject("main", addObjectDeclaration(AddObjectKind::Cube, "Cube")) ==
            SpawnOutcome::Sent);
    harness.reply(harness.take("edit"), {{"status", "accepted"}, {"ticket", "spawn:1"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_edit_result"),
                  {{"status", "committed"},
                   {"committed_revision", 4},
                   {"created_objects", Json::array({{{"object_id", 12},
                                                     {"scene_id", "main"},
                                                     {"declaration_index", 2},
                                                     {"name", "Cube"}}})}});
    const auto &done = harness.model.lastSceneEdit();
    REQUIRE(done);
    CHECK(done->kind == SceneEditKind::Spawn);
    CHECK(done->committed);
    CHECK(done->created_objects.at(0).at("object_id") == 12);
}

TEST_CASE("OL-1 a refused Outliner edit reads as one sentence",
          "[devstudio][inspector][outliner][ol1]") {
    using PelicanStudio::SceneEditKind;
    using PelicanStudio::SceneEditOutcome;
    using PelicanStudio::sceneEditRefusalSentence;
    // Physics OFF: the same sentence Add component shows (Q-6).
    const SceneEditOutcome physics{.kind = SceneEditKind::Duplicate,
                                   .code = "physics.module_unavailable@1",
                                   .message = "physics component requires the physics module"};
    CHECK(sceneEditRefusalSentence(physics, "Crate") ==
          "Cannot duplicate Crate: " +
              PelicanStudio::componentUnavailableDescription("physics.module_unavailable@1") +
              ".");
    CHECK(sceneEditRefusalSentence(physics, "Crate").find("This build has no physics module") !=
          std::string::npos);
    CHECK(sceneEditRefusalSentence({.kind = SceneEditKind::Destroy,
                                    .code = "closure_unresolvable"},
                                   "Cube") == "Cube is no longer in the scene.");
    CHECK(sceneEditRefusalSentence({.kind = SceneEditKind::Reparent,
                                    .code = "parent_not_found"},
                                   "Cube", "Floor") ==
          "Cannot move Cube under Floor: Floor is not a named object in this scene.");
    // A code with no sentence of its own still says what failed and why.
    CHECK(sceneEditRefusalSentence({.kind = SceneEditKind::Duplicate,
                                    .code = "invalid_params",
                                    .message = "unknown field"},
                                   "Cube") == "Cannot duplicate Cube: invalid_params - unknown field");
}

// Q-16: the Inspector's own commit is not another client's edit. Its watch
// token moves with the commit, so a watch answer that reports the revision
// the commit reached does not read the object again -- a read that switched
// Undo off for its length, long enough for a click to land on it switched off.
TEST_CASE("Q-16 the Inspector's own commit does not read as an external change",
          "[devstudio][inspector][refresh][external][q16]") {
    const auto commitEdit = [](InspectorHarness &harness,
                               std::uint64_t revision) {
        const std::string field_key =
            harness.field("fixture_flags", "enabled").field_key;
        REQUIRE(harness.model.commitWidgetValue(field_key, false));
        harness.reply(harness.take("edit"),
                      {{"status", "accepted"}, {"ticket", "edit:1"}});
        harness.model.pollPendingOperations();
        harness.reply(harness.take("get_edit_result"),
                      {{"status", "committed"}, {"committed_revision", revision}});
    };

    SECTION("a watch sent while the commit's read is in flight") {
        InspectorHarness harness;
        harness.open();
        commitEdit(harness, 2);
        const auto tree = harness.take("scene_outline");
        // The 500 ms watch goes out while the read after the commit runs.
        harness.model.pollExternalChanges();
        const auto watch = harness.take("get_scene_revision");
        harness.reply(tree, sceneTreeResult(2));
        harness.reply(harness.take("get_components"), componentResult(1.0, 2));
        // The read is done, and the synchronized watch waits behind the poll.
        REQUIRE(harness.model.takeRpcRequests().empty());
        REQUIRE(harness.model.canUndoRedo());

        harness.reply(watch, {{"scene_revision", 2}, {"preview_epoch", 0}});
        // No second read: Undo stays usable.
        CHECK(harness.model.canUndoRedo());
        const auto sync = harness.take("get_scene_revision");
        harness.reply(sync, {{"scene_revision", 2}, {"preview_epoch", 0}});
        CHECK(harness.model.takeRpcRequests().empty());
        CHECK(harness.model.canUndoRedo());

        // The control: another client's edit moves the scene to 3, and that
        // is read.
        harness.model.pollExternalChanges();
        harness.reply(harness.take("get_scene_revision"),
                      {{"scene_revision", 3}, {"preview_epoch", 0}});
        CHECK_FALSE(harness.model.canUndoRedo());
        REQUIRE(harness.take("scene_outline").method == "scene_outline");
    }

    SECTION("a watch sent before the commit and answered after it") {
        // The engine may have answered before the commit or after it.
        const std::uint64_t answered = GENERATE(1U, 2U);
        INFO("the watch reports revision " << answered);
        InspectorHarness harness;
        harness.open();
        harness.model.pollExternalChanges();
        const auto watch = harness.take("get_scene_revision");
        commitEdit(harness, 2);
        harness.reply(harness.take("scene_outline"), sceneTreeResult(2));
        harness.reply(harness.take("get_components"), componentResult(1.0, 2));
        REQUIRE(harness.model.takeRpcRequests().empty());
        REQUIRE(harness.model.canUndoRedo());

        harness.reply(watch, {{"scene_revision", answered}, {"preview_epoch", 0}});
        CHECK(harness.model.canUndoRedo());
        const auto sync = harness.take("get_scene_revision");
        harness.reply(sync, {{"scene_revision", 2}, {"preview_epoch", 0}});
        CHECK(harness.model.takeRpcRequests().empty());
        CHECK(harness.model.canUndoRedo());

        // The next poll still sees another client's edit.
        harness.model.pollExternalChanges();
        harness.reply(harness.take("get_scene_revision"),
                      {{"scene_revision", 3}, {"preview_epoch", 0}});
        CHECK_FALSE(harness.model.canUndoRedo());
        REQUIRE(harness.take("scene_outline").method == "scene_outline");
    }

    SECTION("undo is the Inspector's own commit too") {
        InspectorHarness harness;
        harness.open();
        commitEdit(harness, 2);
        harness.completeRefresh(1.0, 2);
        harness.model.undo();
        harness.reply(harness.take("undo"),
                      {{"status", "committed"}, {"committed_revision", 3}});
        const auto tree = harness.take("scene_outline");
        harness.model.pollExternalChanges();
        const auto watch = harness.take("get_scene_revision");
        harness.reply(tree, sceneTreeResult(3));
        harness.reply(harness.take("get_components"), componentResult(1.0, 3));
        REQUIRE(harness.model.canUndoRedo());
        harness.reply(watch, {{"scene_revision", 3}, {"preview_epoch", 0}});
        CHECK(harness.model.canUndoRedo());
        REQUIRE(harness.take("get_scene_revision").method == "get_scene_revision");
    }

    SECTION("a save stages the scene under a revision of its own") {
        InspectorHarness harness;
        harness.open();
        commitEdit(harness, 2);
        harness.completeRefresh(1.0, 2);
        REQUIRE(harness.model.saveScene());
        harness.reply(harness.take("save_scene"),
                      {{"status", "saved"},
                       {"scene_revision", 3},
                       {"scene_hot_reload", false},
                       {"byte_count", 1234}});
        REQUIRE(harness.model.canUndoRedo());
        harness.model.pollExternalChanges();
        harness.reply(harness.take("get_scene_revision"),
                      {{"scene_revision", 3}, {"preview_epoch", 0}});
        CHECK(harness.model.canUndoRedo());
        CHECK(harness.model.takeRpcRequests().empty());

        // Another client after the save is still read.
        harness.model.pollExternalChanges();
        harness.reply(harness.take("get_scene_revision"),
                      {{"scene_revision", 4}, {"preview_epoch", 0}});
        CHECK_FALSE(harness.model.canUndoRedo());
        REQUIRE(harness.take("scene_outline").method == "scene_outline");
    }
}

TEST_CASE("PM-3b the Inspector names what it committed by transaction: edits, previews, undo, redo",
          "[devstudio][inspector][pm3b][negative-contrast]") {
    using PelicanStudio::CommittedEditKind;
    InspectorHarness harness;
    harness.open();
    CHECK(harness.model.takeCommittedEdits().empty());

    // A live preview committed: named by its field.
    const std::string field_key = harness.field("transform", "pos").field_key;
    REQUIRE(harness.model.previewWidgetValue(field_key, Json::array({2.0, 2.0, 3.0})));
    harness.reply(harness.take("open_preview"), {{"status", "accepted"},
                                                 {"ticket", "preview:1"},
                                                 {"request_id", "preview-request:open"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_preview_result"), {{"status", "open"}});
    REQUIRE(harness.model.finishWidgetEdit(field_key, true));
    harness.reply(harness.take("commit_preview"),
                  {{"status", "accepted"}, {"request_id", "preview-request:commit"}});
    harness.model.pollPendingOperations();
    harness.reply(harness.take("get_preview_result"),
                  {{"status", "committed"}, {"committed_revision", 2}, {"transaction_id", "txn-1"}});
    auto committed = harness.model.takeCommittedEdits();
    REQUIRE(committed.size() == 1);
    CHECK(committed.front().transaction_id == "txn-1");
    CHECK(committed.front().label == "Edit transform.pos");
    CHECK(committed.front().kind == CommittedEditKind::edit);
    CHECK(harness.model.noticeTransaction() == "txn-1");
    harness.completeRefresh(2.0, 2);

    // Its undo, and the redo of that undo: each with its source.
    harness.model.undo();
    harness.reply(harness.take("undo"), {{"status", "committed"},
                                         {"committed_revision", 3},
                                         {"operation", "undo"},
                                         {"source_transaction_id", "txn-1"},
                                         {"transaction_id", "txn-2"}});
    committed = harness.model.takeCommittedEdits();
    REQUIRE(committed.size() == 1);
    CHECK(committed.front().kind == CommittedEditKind::undo);
    CHECK(committed.front().source_transaction_id == "txn-1");
    harness.completeRefresh(1.0, 3);
    harness.model.redo();
    harness.reply(harness.take("redo"), {{"status", "committed"},
                                         {"committed_revision", 4},
                                         {"operation", "redo"},
                                         {"source_transaction_id", "txn-2"},
                                         {"transaction_id", "txn-3"}});
    committed = harness.model.takeCommittedEdits();
    REQUIRE(committed.size() == 1);
    CHECK(committed.front().kind == CommittedEditKind::redo);
    CHECK(committed.front().source_transaction_id == "txn-2");
    CHECK(committed.front().transaction_id == "txn-3");

    // A refusal names nothing.
    harness.completeRefresh(2.0, 4);
    const std::string flag = harness.field("fixture_flags", "enabled").field_key;
    REQUIRE(harness.model.commitWidgetValue(flag, false));
    harness.reply(harness.take("edit"),
                  {{"status", "rejected"},
                   {"error", {{"code", "stale_revision"}, {"message", "stale"}}}});
    CHECK(harness.model.takeCommittedEdits().empty());
    CHECK(harness.model.noticeTransaction().empty());
}

TEST_CASE("PM-3b a Stop that kept nothing on a saved scene leaves it saved",
          "[devstudio][inspector][pm3b][save][negative-contrast]") {
    InspectorModel adopted;
    adopted.observeSceneRevision(5);
    REQUIRE_FALSE(adopted.hasUnsavedChanges());
    adopted.adoptRestoredRevisionAsSaved();
    // Stop moved the revision on (6.3): the scene it put back is the file's.
    adopted.observeSceneRevision(6);
    CHECK_FALSE(adopted.hasUnsavedChanges());
    // Only that one: an edit after it is unsaved again.
    adopted.observeSceneRevision(7);
    CHECK(adopted.hasUnsavedChanges());

    // Without it, the new revision reads as unsaved.
    InspectorModel plain;
    plain.observeSceneRevision(5);
    plain.observeSceneRevision(6);
    CHECK(plain.hasUnsavedChanges());
}
