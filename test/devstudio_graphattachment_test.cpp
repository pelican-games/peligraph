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
