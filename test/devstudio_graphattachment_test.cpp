#include "graphattachmentmodel.hpp"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
using Json = nlohmann::json;
using PelicanStudio::GraphAttachmentModel;
using PelicanStudio::GraphAttachmentOwner;
using PelicanStudio::InspectorNoticeKind;

Json context() {
    return {{"source_digest", {{"hex", "source-a"}}},
        {"definitions", {{"unit", {{"nodes", Json::array({
            {{"id", "gain"}, {"op", "graph.input"}, {"default", 2.0}},
            {{"id", "speed"}, {"op", "graph.input"}}})}}}}},
        {"instances", Json::array({
            {{"id", "left"}, {"op", "graph.instance"}, {"definition", "unit"},
                {"owner", {{"scene", "main"}, {"object", 41}}}},
            {{"id", "right"}, {"op", "graph.instance"}, {"definition", "unit"},
                {"owner", {{"scene", "main"}, {"object", 73}}}},
            {{"id", "elsewhere"}, {"op", "graph.instance"}, {"definition", "unit"},
                {"owner", {{"scene", "other"}, {"object", 41}}}},
            {{"id", "global"}, {"op", "graph.instance"}, {"definition", "unit"}}})}};
}

struct Harness {
    GraphAttachmentModel model;
    PelicanStudio::InspectorRpcRequest take(std::string_view method) {
        auto requests = model.takeRpcRequests();
        REQUIRE(requests.size() == 1);
        REQUIRE(requests.front().method == std::string{method});
        return requests.front();
    }
    void reply(const PelicanStudio::InspectorRpcRequest &request, const Json &value) {
        model.receiveRpcResult(request.request_id, value.dump());
    }
    void open() {
        model.selectOwner(GraphAttachmentOwner{"main", 41});
        model.setConnected(true);
        reply(take("get_graph_instances"), context());
        REQUIRE(model.canEdit());
    }
};
} // namespace

TEST_CASE("Instance input rows name their supply and report what was drawn", "[studio][graph-attachment]") {
    Harness h;
    auto document = context();
    // A root clock and an edge into one of left's ports, so the rows have all
    // three document modes to show.
    document["nodes"] = Json::array({
        {{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "scene_time"}}},
         {"outputs", Json::array({"value"})}},
        {{"id", "left"}, {"op", "graph.instance"}, {"definition", "unit"},
         {"owner", {{"scene", "main"}, {"object", 41}}},
         {"parameters", {{"speed", 4}}},
         {"inputs", {{"gain", {{"node", "clock"}, {"port", "value"}}}}}}});
    document["instances"][0] = document["nodes"][1];
    h.model.selectOwner(GraphAttachmentOwner{"main", 41});
    h.model.setConnected(true);
    h.reply(h.take("get_graph_instances"), document);

    auto rows = h.model.instanceInputs("left");
    REQUIRE(rows.size() == 2);
    // The definition declares gain first, then speed.
    REQUIRE(rows[0].port == "gain");
    REQUIRE(rows[0].mode == "connected");
    REQUIRE(rows[0].source_node == "clock");
    // The definition's default is still the way back; the edge did not take it.
    REQUIRE(rows[0].has_stored);
    REQUIRE(rows[0].stored == 2.0);
    REQUIRE(rows[1].port == "speed");
    REQUIRE(rows[1].mode == "parameter");
    REQUIRE(rows[1].stored == 4.0);
    REQUIRE_FALSE(rows[0].has_value);

    // Selecting the row asks for the published values on a slot of its own, so
    // it never blocks an edit and never joins the edit's CAS.
    h.model.watchInstance("left");
    const auto values = h.take("get_graph_values");
    REQUIRE(values.params == Json{{"instance", "left"}});
    REQUIRE(h.model.canEdit());
    h.reply(values, {{"instance", "left"}, {"published", true}, {"generation", 3}, {"frame", 9},
        {"inputs", Json::array({{{"port", "gain"}, {"value", 1.25}, {"source", "connected"},
                                 {"connected", {{"node", "clock"}, {"port", "value"}}}},
                                {{"port", "speed"}, {"value", 7.0}, {"source", "override"},
                                 {"parameter", "left/speed"}}})}});
    rows = h.model.instanceInputs("left");
    REQUIRE(rows[0].has_value);
    REQUIRE(rows[0].value == 1.25);
    REQUIRE(rows[0].value_source == "connected");
    // The document still stores 4; the override is reported, not hidden.
    REQUIRE(rows[1].value == 7.0);
    REQUIRE(rows[1].value_source == "override");
    REQUIRE(rows[1].stored == 4.0);
}

TEST_CASE("Instance input edits send one op list per action", "[studio][graph-attachment]") {
    Harness h;
    auto document = context();
    document["nodes"] = Json::array({
        {{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "scene_time"}}},
         {"outputs", Json::array({"value"})}},
        {{"id", "left"}, {"op", "graph.instance"}, {"definition", "unit"},
         {"owner", {{"scene", "main"}, {"object", 41}}},
         {"parameters", {{"gain", 2}, {"speed", 4}}}}});
    document["instances"][0] = document["nodes"][1];
    h.model.selectOwner(GraphAttachmentOwner{"main", 41});
    h.model.setConnected(true);
    h.reply(h.take("get_graph_instances"), document);

    REQUIRE(h.model.setInstanceParameter("left", "gain", 6));
    auto edit = h.take("edit_graph_document");
    REQUIRE(edit.params.at("base_source_digest") == "source-a");
    REQUIRE(edit.params.at("ops") == Json::array({
        {{"op", "set_node_parameters"}, {"node", "left"},
         {"parameters", {{"gain", 6.0}, {"speed", 4}}}}}));
    h.reply(edit, {{"status", "accepted"}, {"ticket", "edit-1"}});
    h.reply(h.take("get_edit_result"), {{"status", "committed"}});
    h.reply(h.take("get_graph_instances"), document);

    // Control with a curve: one call, so one ticket and one CAS. The scene
    // clock the document already has is reused rather than duplicated, and the
    // two keys hold the value the port has now so nothing moves.
    REQUIRE(h.model.controlWithCurve("left", "gain"));
    edit = h.take("edit_graph_document");
    const auto ops = edit.params.at("ops");
    REQUIRE(ops.size() == 3);
    REQUIRE(ops[0].at("node").at("op") == "value.playback");
    REQUIRE(ops[0].at("node").at("inputs").at("elapsed").at("node") == "clock");
    REQUIRE(ops[1].at("node").at("op") == "value.curve");
    REQUIRE(ops[1].at("node").at("parameters").at("keys") ==
            Json::array({{{"time", 0}, {"value", 2.0}}, {{"time", 1}, {"value", 2.0}}}));
    REQUIRE(ops[2] == Json{{"op", "set_input"}, {"node", "left"}, {"port", "gain"},
        {"source", {{"node", ops[1].at("node").at("id")}, {"port", "value"}}}});
    h.reply(edit, {{"status", "accepted"}, {"ticket", "edit-2"}});
    h.reply(h.take("get_edit_result"), {{"status", "committed"}});

    auto connected = document;
    connected["nodes"].push_back(ops[0].at("node"));
    connected["nodes"].push_back(ops[1].at("node"));
    connected["nodes"][1]["inputs"]["gain"] = ops[2].at("source");
    connected["instances"][0] = connected["nodes"][1];
    h.reply(h.take("get_graph_instances"), connected);
    REQUIRE(h.model.instanceInputs("left")[0].mode == "connected");

    // Disconnecting is one op, and the stored value it returns to is the one
    // the attachment kept all along.
    REQUIRE(h.model.disconnectInstanceInput("left", "gain"));
    edit = h.take("edit_graph_document");
    REQUIRE(edit.params.at("ops") == Json::array({
        {{"op", "set_input"}, {"node", "left"}, {"port", "gain"}, {"source", nullptr}}}));

    // Without a document to read, nothing is sent.
    h.model.setConnected(false);
    REQUIRE_FALSE(h.model.controlWithCurve("left", "gain"));
    REQUIRE_FALSE(h.model.setInstanceParameter("left", "gain", 1));
    REQUIRE(h.model.takeRpcRequests().empty());
}

TEST_CASE("Output candidates come from the document and are narrowed by the catalog",
          "[studio][graph-attachment]") {
    Harness h;
    auto document = context();
    document["nodes"] = Json::array({
        {{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "scene_time"}}},
         {"outputs", Json::array({"value"})}},
        {{"id", "surface"}, {"op", "render.buffer"}, {"parameters", {{"element", "float"}}},
         {"outputs", Json::array({"resource"})}},
        {{"id", "left"}, {"op", "graph.instance"}, {"definition", "unit"},
         {"owner", {{"scene", "main"}, {"object", 41}}},
         {"parameters", {{"gain", 2}, {"speed", 4}}}}});
    document["instances"][0] = document["nodes"][2];
    h.model.selectOwner(GraphAttachmentOwner{"main", 41});
    h.model.setConnected(true);
    h.reply(h.take("get_graph_instances"), document);

    // Before the catalog arrives the panel offers what the document declares.
    auto candidates = h.model.outputCandidates("left");
    REQUIRE(candidates.size() == 2);
    h.model.requestCatalog();
    const auto catalog = h.take("get_graph_catalog");
    REQUIRE(h.model.canEdit());
    h.reply(catalog, {{"schema", "pelican.graph_catalog"}, {"version", 1},
        {"modules", Json::array({{{"namespace", "pelican.value"},
            {"operations", Json::array({
                {{"op", "value.input"}, {"ports", "declared"},
                 {"outputs", Json::array({{{"port", "value"},
                    {"type", {{"semantic", "pelican.value.scalar@1"}}}}})}},
                {{"op", "render.buffer"}, {"ports", "parameter_dependent"}}})}}})}});
    // render.buffer resolves its ports from its parameters, so the catalog
    // cannot promise a scalar and the panel does not offer one.
    candidates = h.model.outputCandidates("left");
    REQUIRE(candidates.size() == 1);
    REQUIRE(candidates[0].node == "clock");
    REQUIRE(candidates[0].port == "value");
    REQUIRE(h.model.connectInstanceInput("left", "gain", candidates[0]));
    const auto edit = h.take("edit_graph_document");
    REQUIRE(edit.params.at("ops") == Json::array({
        {{"op", "set_input"}, {"node", "left"}, {"port", "gain"},
         {"source", {{"node", "clock"}, {"port", "value"}}}}}));
}

TEST_CASE("Graph attachments use stable owners and source CAS", "[studio][graph-attachment]") {
    Harness h;
    h.open();
    REQUIRE(h.model.instances().size() == 1);
    REQUIRE(h.model.instances().at(0).at("id") == "left");
    REQUIRE(h.model.parameterDefaults("unit") == Json{{"gain", 2.0}, {"speed", 0.0}});
    REQUIRE(h.model.attach("new", "unit", {{"gain", 3}, {"speed", 4}}));
    REQUIRE_FALSE(h.model.attach("duplicate", "unit", {{"speed", 0}}));
    REQUIRE_FALSE(h.model.detach("left"));
    const auto submit = h.take("edit_graph_instances");
    REQUIRE(submit.params.at("base_source_digest") == "source-a");
    const auto attached = submit.params.at("attach").at(0);
    REQUIRE(attached.at("owner") == Json{{"scene", "main"}, {"object", 41}});
    REQUIRE(attached.at("parameters") == Json{{"gain", 3}, {"speed", 4}});
    h.reply(submit, {{"status", "accepted"}, {"ticket", "edit-1"}});
    h.reply(h.take("get_edit_result"), {{"status", "pending"}});
    REQUIRE(h.model.busy());
    h.model.poll();
    h.reply(h.take("get_edit_result"), {{"status", "committed"}});
    auto saved = context();
    saved["source_digest"]["hex"] = "source-b";
    saved["instances"].push_back(attached);
    h.reply(h.take("get_graph_instances"), saved);
    REQUIRE(h.model.instances().size() == 2);
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::Success);
    REQUIRE_FALSE(h.model.detach("right"));
    REQUIRE(h.model.detach("left"));
    const auto detach = h.take("edit_graph_instances");
    REQUIRE(detach.params.at("detach") == Json::array({"left"}));
    REQUIRE(detach.params.at("attach").empty());
    REQUIRE(detach.params.at("base_source_digest") == "source-b");
}

TEST_CASE("Graph attachment selection changes discard stale reads but keep pending edits", "[studio][graph-attachment]") {
    Harness h;
    h.model.setConnected(true);
    h.model.selectOwner(GraphAttachmentOwner{"main", 41});
    const auto obsolete = h.take("get_graph_instances");
    h.model.selectOwner(GraphAttachmentOwner{"main", 73});
    const auto current = h.take("get_graph_instances");
    h.reply(obsolete, context());
    REQUIRE(h.model.instances().empty());
    REQUIRE(h.model.busy());
    h.reply(current, context());
    REQUIRE(h.model.instances().at(0).at("id") == "right");
    REQUIRE(h.model.detach("right"));
    const auto edit = h.take("edit_graph_instances");
    h.model.selectOwner(GraphAttachmentOwner{"main", 41});
    REQUIRE(h.model.takeRpcRequests().empty());
    REQUIRE_FALSE(h.model.canEdit());
    h.reply(edit, {{"status", "accepted"}, {"ticket", "old-owner-edit"}});
    const auto poll = h.take("get_edit_result");
    h.model.receiveRpcFailure(poll.request_id, "Temporary transport failure");
    REQUIRE(h.model.busy());
    h.model.poll();
    h.reply(h.take("get_edit_result"), {{"status", "committed"}});
    h.reply(h.take("get_graph_instances"), context());
    REQUIRE(h.model.instances().at(0).at("id") == "left");
    REQUIRE(h.model.notice().kind == InspectorNoticeKind::None);
}

TEST_CASE("Graph attachment failures preserve context and expose engine diagnostics", "[studio][graph-attachment]") {
    Harness h;
    h.open();
    REQUIRE(h.model.detach("left"));
    h.reply(h.take("edit_graph_instances"), {{"status", "rejected"},
        {"error", {{"code", "invalid_graph_attachment"}, {"message", "output still referenced"}}}});
    REQUIRE(h.model.canEdit());
    REQUIRE(h.model.instances().size() == 1);
    REQUIRE(h.model.notice().message == "output still referenced (invalid_graph_attachment)");
    REQUIRE(h.model.detach("left"));
    h.reply(h.take("edit_graph_instances"), {{"status", "accepted"}, {"ticket", "stale"}});
    h.reply(h.take("get_edit_result"), {{"status", "rejected"},
        {"error", {{"code", "stale_source_digest"}, {"message", "Source changed"}}}});
    REQUIRE(h.model.canRefresh());
    REQUIRE(h.model.notice().message == "Source changed (stale_source_digest)");
    h.model.refresh();
    h.reply(h.take("get_graph_instances"), context());
    REQUIRE(h.model.canEdit());
}

TEST_CASE("Graph attachment drafts validate finite scalar parameters", "[studio][graph-attachment]") {
    Harness h;
    h.open();
    REQUIRE_FALSE(h.model.attach("x", "unit", {{"gain", 1}}));
    REQUIRE_FALSE(h.model.attach("x", "unit", {{"speed", std::numeric_limits<double>::infinity()}}));
    REQUIRE_FALSE(h.model.attach("x", "unit", {{"speed", 1}, {"unknown", 2}}));
    REQUIRE_FALSE(h.model.attach("x", "missing", Json::object()));
    REQUIRE_FALSE(h.model.attach("left", "unit", {{"speed", 1}}));
    REQUIRE_FALSE(h.model.attach("bad/name", "unit", {{"speed", 1}}));
    REQUIRE(h.model.takeRpcRequests().empty());
    REQUIRE(h.model.attach(h.model.suggestInstanceId("unit"), "unit", {{"speed", 1}}));
    h.take("edit_graph_instances");
    h.model.setConnected(false);
    REQUIRE_FALSE(h.model.busy());
    REQUIRE_FALSE(h.model.canEdit());
    REQUIRE(h.model.instances().empty());
}

TEST_CASE("Unconfirmed graph edit poll replies retain the accepted ticket", "[studio][graph-attachment]") {
    Harness h;
    h.open();
    REQUIRE(h.model.detach("left"));
    h.reply(h.take("edit_graph_instances"), {{"status", "accepted"}, {"ticket", "edit-1"}});
    auto poll = h.take("get_edit_result");
    h.model.receiveRpcResult(poll.request_id, "invalid JSON");
    REQUIRE(h.model.busy());
    REQUIRE_FALSE(h.model.detach("left"));
    h.model.poll();
    h.reply(h.take("get_edit_result"), {{"error", {{"message", "Temporary service error"}}}});
    REQUIRE(h.model.busy());
    h.model.poll();
    h.reply(h.take("get_edit_result"), {{"status", "unexpected"}});
    REQUIRE(h.model.busy());
    h.model.poll();
    h.reply(h.take("get_edit_result"), {{"status", "rejected"}});
    REQUIRE_FALSE(h.model.busy());
    REQUIRE(h.model.canRefresh());
}
