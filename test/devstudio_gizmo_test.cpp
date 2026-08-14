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
                 std::string_view axis, double content_scale = 1.0,
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
        {"content_scale", content_scale},
        {"grab_radius_pixels", 10.0},
        {"handle",
         {{"id", handle},
          {"axis", axis},
          {"drag_direction",
           {{"x", drag_direction[0]}, {"y", drag_direction[1]}}},
          {"value_per_logical_pixel", value_per_logical_pixel}}},
    };
}

Json modalResult(std::string_view phase, std::uint64_t revision,
                 std::optional<std::uint64_t> operation_id,
                 bool enabled, std::array<double, 3> translation,
                 std::optional<GizmoMode> mode = GizmoMode::Translate,
                 std::optional<std::string_view> axis = "x",
                 bool expose_bindings = true) {
    Json result{
        {"contract", 1},
        {"enabled", enabled},
        {"revision", revision},
        {"phase", phase},
        {"operation_id", nullptr},
        {"selection", nullptr},
        {"mode", nullptr},
        {"axis", nullptr},
        {"delta",
         {{"translation",
           {translation[0], translation[1], translation[2]}},
          {"rotation", {0.0, 0.0, 0.0, 1.0}},
          {"scale_exponent", {0.0, 0.0, 0.0}}}},
        {"reason", nullptr},
        {"bindings",
         {{"translate", nullptr},
          {"rotate", nullptr},
          {"scale", nullptr}}},
    };
    if (operation_id) {
        result["operation_id"] = *operation_id;
        result["selection"] =
            Json{{"kind", "declaration"},
                 {"scene_id", Selection.scene_id},
                 {"declaration_index", Selection.declaration_index}};
        result["reason"] = "test";
    }
    if (mode) {
        result["mode"] = std::string{PelicanStudio::gizmoModeName(*mode)};
    }
    if (axis) result["axis"] = std::string{*axis};
    if (expose_bindings) {
        result["bindings"]["translate"] = "G";
        result["bindings"]["rotate"] = "R";
        result["bindings"]["scale"] = "S";
    }
    return result;
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
          "[devstudio][gizmo][rotation][scale][dpi][wp275][wp279]") {
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
    // The fixture keeps grab_radius_pixels at 10.0, so scale cannot be
    // recovered from the hit radius.
    model.receiveRpcResult(
        scale_query.request_id,
        queryResult(GizmoMode::Scale, "scale_x", "x", 2.0).dump());
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
        queryResult(GizmoMode::Translate, "translate_x", "x", 1.0,
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

TEST_CASE("Devstudio adapts engine modal state to one preview lease and RPC toolbar labels",
          "[devstudio][gizmo][modal][rpc][negative-contrast][wp286]") {
    GizmoModel enabled;
    openModel(enabled);
    enabled.pollModalTransform(transformBinding());
    const auto active_poll = takeRpc(enabled, "get_modal_transform");
    enabled.receiveRpcResult(
        active_poll.request_id,
        modalResult("active", 3, 42, true, {0.25, 0.0, 0.0})
            .dump());
    const auto begin = takeEdit(enabled, GizmoEditActionKind::Begin);
    REQUIRE(begin.gesture_id == 42);
    REQUIRE(begin.field_key == "41:transform:0:/pos");
    REQUIRE(std::string{enabled.bindingDisplay(GizmoMode::Translate)} == "G");
    REQUIRE(std::string{enabled.bindingDisplay(GizmoMode::Rotate)} == "R");
    REQUIRE(std::string{enabled.bindingDisplay(GizmoMode::Scale)} == "S");
    enabled.confirmEditStarted(begin.gesture_id, true);
    const auto first_preview =
        takeEdit(enabled, GizmoEditActionKind::Preview);
    REQUIRE(first_preview.value == Json::array({1.25, 2.0, 3.0}));

    enabled.pollModalTransform(transformBinding());
    const auto confirmed_poll = takeRpc(enabled, "get_modal_transform");
    enabled.receiveRpcResult(
        confirmed_poll.request_id,
        modalResult("confirmed", 4, 42, true, {0.5, 0.0, 0.0})
            .dump());
    const auto committed = enabled.takeEditActions();
    REQUIRE(committed.size() == 2);
    REQUIRE(committed[0].kind == GizmoEditActionKind::Preview);
    REQUIRE(committed[0].value == Json::array({1.5, 2.0, 3.0}));
    REQUIRE(committed[1].kind == GizmoEditActionKind::Finish);
    REQUIRE(committed[1].commit);
    REQUIRE_FALSE(enabled.gestureActive());
    const auto acknowledgement = takeRpc(enabled, "ack_modal_transform");
    REQUIRE(acknowledgement.params.at("operation_id") == 42);
    REQUIRE(acknowledgement.params.at("revision") == 4);

    GizmoModel disabled;
    openModel(disabled);
    disabled.pollModalTransform(transformBinding());
    const auto disabled_poll = takeRpc(disabled, "get_modal_transform");
    disabled.receiveRpcResult(
        disabled_poll.request_id,
        modalResult("idle", 1, std::nullopt, false,
                    {0.0, 0.0, 0.0}, std::nullopt, std::nullopt, false)
            .dump());
    const auto disabled_edits = disabled.takeEditActions();
    REQUIRE(disabled_edits.empty());
    REQUIRE_FALSE(disabled.gestureActive());
    REQUIRE(disabled.bindingDisplay(GizmoMode::Translate).empty());
    // Rule 10: the same Studio binding snapshot produces authoring actions
    // only when the engine reports the modal action overlay as effective.
    REQUIRE(committed.size() != disabled_edits.size());
}

TEST_CASE("Devstudio modal cancellation and confirmation finish the same lease differently",
          "[devstudio][gizmo][modal][cancel][negative-contrast][wp286]") {
    const auto terminalCommit = [](std::string_view terminal_phase) {
        GizmoModel model;
        openModel(model);
        model.pollModalTransform(transformBinding());
        auto poll = takeRpc(model, "get_modal_transform");
        model.receiveRpcResult(
            poll.request_id,
            modalResult("active", 10, 7, true, {0.4, 0.0, 0.0})
                .dump());
        const auto begin = takeEdit(model, GizmoEditActionKind::Begin);
        model.confirmEditStarted(begin.gesture_id, true);
        REQUIRE(takeEdit(model, GizmoEditActionKind::Preview).value ==
                Json::array({1.4, 2.0, 3.0}));

        model.pollModalTransform(transformBinding());
        poll = takeRpc(model, "get_modal_transform");
        model.receiveRpcResult(
            poll.request_id,
            modalResult(terminal_phase, 11, 7, true,
                        {0.4, 0.0, 0.0})
                .dump());
        const auto finish = takeEdit(model, GizmoEditActionKind::Finish);
        REQUIRE(finish.gesture_id == begin.gesture_id);
        (void)takeRpc(model, "ack_modal_transform");
        return finish.commit;
    };

    const bool confirmed = terminalCommit("confirmed");
    const bool cancelled = terminalCommit("cancelled");
    REQUIRE(confirmed);
    REQUIRE_FALSE(cancelled);
    REQUIRE(confirmed != cancelled);
}

TEST_CASE("Devstudio refuses modal begin while the Inspector edit lease is busy",
          "[devstudio][gizmo][modal][inspector][negative-contrast][wp286]") {
    const auto beginWithInspector = [](bool inspector_accepts) {
        GizmoModel model;
        openModel(model);
        model.pollModalTransform(transformBinding());
        const auto poll = takeRpc(model, "get_modal_transform");
        model.receiveRpcResult(
            poll.request_id,
            modalResult("active", 20, 8, true, {0.2, 0.0, 0.0})
                .dump());
        const auto begin = takeEdit(model, GizmoEditActionKind::Begin);
        model.confirmEditStarted(begin.gesture_id, inspector_accepts);
        const auto edits = model.takeEditActions();
        const auto requests = model.takeRpcRequests();
        return std::pair{edits, requests};
    };

    const auto [accepted_edits, accepted_requests] =
        beginWithInspector(true);
    const auto [busy_edits, busy_requests] = beginWithInspector(false);
    REQUIRE(accepted_edits.size() == 1);
    REQUIRE(accepted_edits.front().kind == GizmoEditActionKind::Preview);
    REQUIRE(accepted_requests.empty());
    REQUIRE(busy_edits.empty());
    REQUIRE(busy_requests.size() == 1);
    REQUIRE(busy_requests.front().method == "cancel_modal_transform");
    REQUIRE(accepted_edits.size() != busy_edits.size());
}
