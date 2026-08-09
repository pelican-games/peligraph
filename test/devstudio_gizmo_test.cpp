#include "../src/devstudio/model/gizmomodel.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string_view>

namespace {

using Json = nlohmann::json;
using PelicanStudio::GizmoEditAction;
using PelicanStudio::GizmoEditActionKind;
using PelicanStudio::GizmoEditableField;
using PelicanStudio::GizmoMode;
using PelicanStudio::GizmoModel;
using PelicanStudio::GizmoPixelPosition;
using PelicanStudio::GizmoRpcRequest;
using PelicanStudio::GizmoTransformBinding;
using PelicanStudio::OutlinerObjectKey;

const OutlinerObjectKey Selection{.scene_id = "main",
                                  .declaration_index = 2};

GizmoTransformBinding transformBinding() {
    return {
        .selection = Selection,
        .position = GizmoEditableField{
            .field_key = "41:transform:0:/pos",
            .value = Json::array({1.0, 2.0, 3.0})},
        .rotation = GizmoEditableField{
            .field_key = "41:transform:0:/rotation",
            .value = Json::array({0.0, 0.0, 0.0, 1.0})},
        .scale = GizmoEditableField{
            .field_key = "41:transform:0:/scale",
            .value = Json::array({1.0, 1.0, 1.0})},
    };
}

GizmoRpcRequest takeRpc(GizmoModel &model, std::string_view method) {
    auto requests = model.takeRpcRequests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().method == std::string{method});
    return std::move(requests.front());
}

GizmoEditAction takeEdit(GizmoModel &model, GizmoEditActionKind kind) {
    auto actions = model.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == kind);
    return std::move(actions.front());
}

Json displayResult(GizmoMode mode, bool visible = true) {
    return {
        {"contract", 1},
        {"visible", visible},
        {"selection", visible
                          ? Json{{"kind", "declaration"},
                                 {"scene_id", Selection.scene_id},
                                 {"declaration_index",
                                  Selection.declaration_index}}
                          : Json(nullptr)},
        {"mode", PelicanStudio::gizmoModeName(mode)},
    };
}

Json queryResult(GizmoMode mode, std::string_view handle,
                 std::string_view axis, double grab_radius = 10.0,
                 std::array<double, 2> drag_direction = {1.0, 0.0},
                 double value_per_logical_pixel = 0.01) {
    return {
        {"contract", 2},
        {"selection",
         {{"kind", "declaration"},
          {"scene_id", Selection.scene_id},
          {"declaration_index", Selection.declaration_index}}},
        {"mode", PelicanStudio::gizmoModeName(mode)},
        {"coordinate", {{"x", 100}, {"y", 100}}},
        {"extent", {{"width", 800}, {"height", 600}}},
        {"grab_radius_pixels", grab_radius},
        {"handle",
         {{"id", handle},
          {"axis", axis},
          {"drag_direction",
           {{"x", drag_direction[0]}, {"y", drag_direction[1]}}},
          {"value_per_logical_pixel", value_per_logical_pixel}}},
    };
}

void openModel(GizmoModel &model) {
    model.setSelection(Selection);
    model.startSession();
    const auto display = takeRpc(model, "set_gizmo");
    REQUIRE(display.params ==
            Json{{"selection",
                  {{"kind", "declaration"},
                   {"scene_id", "main"},
                   {"declaration_index", 2}}},
                 {"mode", "translate"}});
    model.receiveRpcResult(display.request_id,
                           displayResult(GizmoMode::Translate).dump());
}

} // namespace

TEST_CASE("Devstudio gizmo synchronizes selection and toolbar mode over the public RPC",
          "[devstudio][gizmo][rpc][wp275]") {
    GizmoModel model;
    openModel(model);

    model.setMode(GizmoMode::Rotate);
    const auto rotate = takeRpc(model, "set_gizmo");
    REQUIRE(rotate.params.at("mode") == "rotate");
    REQUIRE(rotate.params.at("selection").at("scene_id") == "main");
    model.receiveRpcResult(rotate.request_id,
                           displayResult(GizmoMode::Rotate).dump());

    model.setSelection(std::nullopt);
    const auto clear = takeRpc(model, "set_gizmo");
    REQUIRE(clear.params ==
            Json{{"selection", nullptr}, {"mode", "rotate"}});
}

TEST_CASE("Devstudio gizmo queries once then previews and commits the captured axis",
          "[devstudio][gizmo][preview][wp275]") {
    GizmoModel model;
    openModel(model);

    model.pointerPressed({100, 100}, transformBinding());
    const auto query = takeRpc(model, "query_gizmo_handle");
    REQUIRE(query.params ==
            Json{{"selection",
                  {{"kind", "declaration"},
                   {"scene_id", "main"},
                   {"declaration_index", 2}}},
                 {"mode", "translate"},
                 {"x", 100},
                 {"y", 100}});

    model.pointerMoved({120, 100});
    model.pointerMoved({140, 100});
    REQUIRE(model.takeRpcRequests().empty());

    model.receiveRpcResult(
        query.request_id,
        queryResult(GizmoMode::Translate, "translate_x", "x").dump());
    const auto begin = takeEdit(model, GizmoEditActionKind::Begin);
    REQUIRE(begin.field_key == "41:transform:0:/pos");
    model.confirmEditStarted(begin.gesture_id, true);

    const auto first_preview =
        takeEdit(model, GizmoEditActionKind::Preview);
    REQUIRE(first_preview.value.at(0).get<double>() ==
            Catch::Approx(1.4));
    REQUIRE(first_preview.value.at(1) == 2.0);
    REQUIRE(first_preview.value.at(2) == 3.0);

    model.pointerMoved({150, 100});
    REQUIRE(takeEdit(model, GizmoEditActionKind::Preview)
                .value.at(0)
                .get<double>() == Catch::Approx(1.5));
    REQUIRE(model.takeRpcRequests().empty());

    model.pointerReleased({160, 100});
    auto release_actions = model.takeEditActions();
    REQUIRE(release_actions.size() == 2);
    REQUIRE(release_actions[0].kind == GizmoEditActionKind::Preview);
    REQUIRE(release_actions[0].value.at(0).get<double>() ==
            Catch::Approx(1.6));
    REQUIRE(release_actions[1].kind == GizmoEditActionKind::Finish);
    REQUIRE(release_actions[1].commit);
    REQUIRE_FALSE(model.gestureActive());
    REQUIRE(model.takeRpcRequests().empty());
}

TEST_CASE("Devstudio gizmo rotation normalizes quaternion and scale follows DPI",
          "[devstudio][gizmo][rotation][scale][dpi][wp275]") {
    GizmoModel model;
    openModel(model);

    model.setMode(GizmoMode::Rotate);
    (void)takeRpc(model, "set_gizmo");
    model.pointerPressed({100, 100}, transformBinding());
    const auto rotate_query = takeRpc(model, "query_gizmo_handle");
    model.receiveRpcResult(
        rotate_query.request_id,
        queryResult(GizmoMode::Rotate, "rotate_z", "z").dump());
    auto begin = takeEdit(model, GizmoEditActionKind::Begin);
    model.confirmEditStarted(begin.gesture_id, true);
    model.pointerMoved({200, 100});
    const Json rotation =
        takeEdit(model, GizmoEditActionKind::Preview).value;
    const double norm = std::sqrt(
        rotation[0].get<double>() * rotation[0].get<double>() +
        rotation[1].get<double>() * rotation[1].get<double>() +
        rotation[2].get<double>() * rotation[2].get<double>() +
        rotation[3].get<double>() * rotation[3].get<double>());
    REQUIRE(norm == Catch::Approx(1.0));
    REQUIRE(rotation[2].get<double>() > 0.0);
    model.pointerReleased({200, 100});
    (void)model.takeEditActions();

    model.setMode(GizmoMode::Scale);
    (void)takeRpc(model, "set_gizmo");
    model.pointerPressed({100, 100}, transformBinding());
    const auto scale_query = takeRpc(model, "query_gizmo_handle");
    model.receiveRpcResult(
        scale_query.request_id,
        queryResult(GizmoMode::Scale, "scale_x", "x", 20.0).dump());
    begin = takeEdit(model, GizmoEditActionKind::Begin);
    model.confirmEditStarted(begin.gesture_id, true);
    model.pointerMoved({300, 100});
    const Json scale = takeEdit(model, GizmoEditActionKind::Preview).value;
    // 200 physical px at 2x content scale is 100 logical px: exp(1).
    REQUIRE(scale[0].get<double>() == Catch::Approx(std::exp(1.0)));
    REQUIRE(scale[1] == 1.0);
    REQUIRE(scale[2] == 1.0);
}

TEST_CASE("Devstudio gizmo applies the engine projected direction and magnitude",
          "[devstudio][gizmo][projection][wp276]") {
    GizmoModel model;
    openModel(model);

    model.pointerPressed({100, 100}, transformBinding());
    const auto query = takeRpc(model, "query_gizmo_handle");
    model.receiveRpcResult(
        query.request_id,
        queryResult(GizmoMode::Translate, "translate_x", "x", 10.0,
                    {-1.0, 0.0}, 0.25)
            .dump());
    const auto begin = takeEdit(model, GizmoEditActionKind::Begin);
    model.confirmEditStarted(begin.gesture_id, true);

    model.pointerMoved({80, 100});
    const auto preview = takeEdit(model, GizmoEditActionKind::Preview);
    REQUIRE(preview.value.at(0).get<double>() == Catch::Approx(6.0));
    REQUIRE(preview.value.at(1) == 2.0);
    REQUIRE(preview.value.at(2) == 3.0);
}

TEST_CASE("Devstudio gizmo miss falls back to picking and stale query cannot start a drag",
          "[devstudio][gizmo][selection][stale][wp275]") {
    GizmoModel model;
    openModel(model);

    model.pointerPressed({100, 100}, transformBinding());
    const auto miss_query = takeRpc(model, "query_gizmo_handle");
    Json miss = queryResult(GizmoMode::Translate, "translate_x", "x");
    miss["handle"] = nullptr;
    model.pointerMoved({130, 140});
    model.pointerReleased({130, 140});
    model.receiveRpcResult(miss_query.request_id, miss.dump());
    REQUIRE(model.takeFallbackPicks() ==
            std::vector<GizmoPixelPosition>{{100, 100}});
    REQUIRE(model.takeEditActions().empty());

    model.pointerPressed({100, 100}, transformBinding());
    const auto stale_query = takeRpc(model, "query_gizmo_handle");
    model.setMode(GizmoMode::Rotate);
    (void)takeRpc(model, "set_gizmo");
    model.receiveRpcResult(
        stale_query.request_id,
        queryResult(GizmoMode::Translate, "translate_x", "x").dump());
    REQUIRE(model.takeEditActions().empty());
    REQUIRE(model.takeFallbackPicks().empty());

    model.setMode(GizmoMode::Translate);
    (void)takeRpc(model, "set_gizmo");
    model.pointerPressed({100, 100}, transformBinding());
    const auto failed_query = takeRpc(model, "query_gizmo_handle");
    model.receiveRpcFailure(
        failed_query.request_id,
        "engine://features/gizmo.json is not enabled");
    REQUIRE(model.takeFallbackPicks() ==
            std::vector<GizmoPixelPosition>{{100, 100}});
    REQUIRE_FALSE(model.notice().message.empty());
}

TEST_CASE("Devstudio gizmo discards a superseded query failure symmetrically",
          "[devstudio][gizmo][stale][failure][wp276]") {
    GizmoModel model;
    openModel(model);

    model.pointerPressed({100, 100}, transformBinding());
    const auto first = takeRpc(model, "query_gizmo_handle");
    model.pointerPressed({120, 100}, transformBinding());
    const auto second = takeRpc(model, "query_gizmo_handle");

    model.receiveRpcFailure(
        first.request_id,
        "engine://features/gizmo.json is not enabled");
    REQUIRE(model.notice().kind == PelicanStudio::GizmoNoticeKind::None);
    REQUIRE(model.takeFallbackPicks().empty());
    REQUIRE(model.takeEditActions().empty());
    REQUIRE(model.gestureActive());

    model.receiveRpcResult(
        second.request_id,
        queryResult(GizmoMode::Translate, "translate_x", "x").dump());
    REQUIRE(takeEdit(model, GizmoEditActionKind::Begin).field_key ==
            "41:transform:0:/pos");
}

TEST_CASE("Devstudio gizmo aborts its preview when mode changes during a drag",
          "[devstudio][gizmo][preview][abort][wp275]") {
    GizmoModel model;
    openModel(model);

    model.pointerPressed({100, 100}, transformBinding());
    const auto query = takeRpc(model, "query_gizmo_handle");
    model.receiveRpcResult(
        query.request_id,
        queryResult(GizmoMode::Translate, "translate_x", "x").dump());
    const auto begin = takeEdit(model, GizmoEditActionKind::Begin);
    model.confirmEditStarted(begin.gesture_id, true);
    model.pointerMoved({140, 100});
    (void)takeEdit(model, GizmoEditActionKind::Preview);

    model.setMode(GizmoMode::Rotate);
    const auto actions = model.takeEditActions();
    REQUIRE(actions.size() == 1);
    REQUIRE(actions.front().kind == GizmoEditActionKind::Finish);
    REQUIRE(actions.front().gesture_id == begin.gesture_id);
    REQUIRE_FALSE(actions.front().commit);
    REQUIRE_FALSE(model.gestureActive());
    REQUIRE(takeRpc(model, "set_gizmo").params.at("mode") == "rotate");
}
