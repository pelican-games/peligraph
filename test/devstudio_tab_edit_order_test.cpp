#include "tabeditorder.hpp"
#include "renderdocumentreload.hpp"
#include "graphcomposition.hpp"
#include "renderconfigdocument.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
using Json = nlohmann::json;
using Order = PelicanStudio::TabEditOrder;
using History = PelicanStudio::RenderEditHistory;
using Request = PelicanStudio::InspectorRpcRequest;
const std::string Root = "project://passes/main.json";

struct Rig {
    PelicanStudio::GraphCanvasModel graph;
    Order order{graph};
    std::vector<Request> requests;
    std::set<std::uint64_t> answered;
    std::uint64_t next = 100;
    int generation = 0;
    bool content_digests = false;
    Json document{{"schema", "pelican.graph"}, {"version", 1},
                  {"nodes", Json::array({{{"id", "value"}, {"op", "value.constant"},
                                         {"parameters", {{"value", 1}}}}})},
                  {"definitions", Json::object()}, {"outputs", Json::object()},
                  {"fold_groups", {{"group", {{"folded", Json::object()}}}}}};
    bool fail_poll = false;
    std::vector<std::function<void()>> scheduled;
    History history{History::Transport{
        .request = [this](const std::string &method, const Json &params, std::string *) {
            if (fail_poll && method == "get_edit_result") return std::int64_t{0};
            const auto id = next++;
            requests.push_back({id, method, params});
            return static_cast<std::int64_t>(id);
        },
        .schedule = [this](int, std::function<void()> task) { scheduled.push_back(std::move(task)); },
    }};

    Rig() {
        order.setRenderHistory(&history);
        graph.setConnected(true);
        readGraph();
    }
    std::string digest() const {
        return content_digests ? Pelican::renderConfigSourceDigest(document.dump())
                               : std::string(63, 'a') + "0123456789abcdef"[generation % 16];
    }
    Json commit() const {
        return {{"status", "committed"}, {"committed", true},
                {"source_digest", {{"hex", digest()}}}, {"documents", {{Root, digest()}}}};
    }
    Json snapshot() const {
        return {{"documents", Json::array({{{"reference", Root}, {"kind", "root"},
                                            {"digest", digest()}, {"content", document.dump()}}})}};
    }
    void readGraph() {
        for (const auto &request : graph.takeRpcRequests()) {
            if (request.method == "get_graph_instances") {
                auto context = document;
                context["source_reference"] = Root;
                context["source_digest"] = {{"hex", digest()}};
                graph.receiveRpcResult(request.request_id, context.dump());
            }
        }
    }
    Request graphRequest(const std::string &method) {
        const auto outbox = graph.takeRpcRequests();
        const auto found = std::ranges::find(outbox, method, &Request::method);
        REQUIRE(found != outbox.end());
        return *found;
    }
    Request last(const std::string &method) const {
        const auto found = std::find_if(requests.rbegin(), requests.rend(), [&](const auto &r) {
            return r.method == method;
        });
        REQUIRE(found != requests.rend());
        return *found;
    }
    void readHistory() {
        for (std::size_t i = 0; i < requests.size(); ++i) {
            const auto request = requests[i];
            if (request.method == "get_render_documents" && answered.insert(request.request_id).second)
                REQUIRE(history.receiveResult(request.request_id, snapshot()));
        }
    }
    Request submitGraph(std::string label = "changed") {
        REQUIRE(graph.setNodeLabel("value", std::move(label)));
        return graphRequest("edit_graph_document");
    }
    void commitGraph(const Request &request) {
        graph.receiveRpcResult(request.request_id, R"({"status":"accepted","ticket":"graph"})");
        const auto poll = graphRequest("get_edit_result");
        document = Pelican::editGraphDocument(document, request.params.at("ops"));
        ++generation;
        graph.receiveRpcResult(poll.request_id, commit().dump());
        readGraph();
        REQUIRE(graph.canEdit());
    }
    std::uint64_t submitRender(const std::string &method = "edit_preset_pass", bool own = true,
                               bool snapshot_ok = true) {
        history.beforeRequest(method, {{"group", "group"}, {"base_source_digest", digest()}});
        const auto read = last("get_render_documents").request_id;
        const auto id = next++;
        history.requestSent(id, method, {});
        if (own) order.renderRequestSent(id);
        if (snapshot_ok) readHistory();
        else {
            answered.insert(read);
            REQUIRE(history.receiveFailure(read, "snapshot lost"));
        }
        return id;
    }
    std::uint64_t accept(std::uint64_t id, const std::string &ticket = "render") {
        REQUIRE(history.receiveResult(id, {{"status", "accepted"}, {"ticket", ticket}}));
        return fail_poll ? 0 : last("get_edit_result").request_id;
    }
    void commitRender(std::uint64_t poll) {
        ++generation;
        REQUIRE(history.receiveResult(poll, commit()));
        readHistory();
    }
    void restore(bool reject = false) {
        readHistory();
        const auto request = last("restore_render_documents");
        if (reject) {
            REQUIRE(history.receiveResult(request.request_id, {{"status", "rejected"},
                    {"error", {{"code", "external_modification"}, {"reference", Root}}}}));
            return;
        }
        const auto &documents = request.params.at("documents");
        REQUIRE(documents.size() == 1);
        CHECK(documents[0].at("reference") == Root);
        CHECK(documents[0].at("base_digest") == digest());
        document = Json::parse(documents[0].at("content").get<std::string>());
        ++generation;
        REQUIRE(history.receiveResult(request.request_id, {{"status", "accepted"}, {"ticket", "restore"}}));
        REQUIRE(history.receiveResult(last("get_edit_result").request_id, commit()));
    }
};
} // namespace

TEST_CASE("RT-D ruling 2 verify placeholder survives a later root refusal and retry is last",
          "[devstudio][tab-edit-order][rtd]") {
    const auto code = GENERATE(std::string{"external_modification"}, std::string{"stale_source_digest"});
    Rig rig;
    const auto verify = rig.submitRender("verify_fold");
    const auto verify_poll = rig.accept(verify, "verify");
    REQUIRE(rig.order.undoRecords().size() == 1);
    const auto first = rig.order.undoRecords()[0];
    CHECK(first.serial == 0);
    REQUIRE(rig.history.receiveResult(verify_poll, {{"status", "running"}}));
    const auto later = rig.submitGraph();
    const auto discarded_key = rig.order.undoRecords().back().sent;
    REQUIRE_FALSE(rig.order.canUndo());
    rig.graph.receiveRpcResult(later.request_id, R"({"status":"accepted","ticket":"later"})");
    const auto later_poll = rig.graphRequest("get_edit_result");
    // The fake responses model harvestFoldVerifies moving verify ahead of
    // the already accepted root edit. No engine asynchronous worker runs.
    rig.document["fold_groups"]["group"]["folded"]["verified"] = {{"digest", "verified"}};
    REQUIRE(rig.scheduled.size() == 1);
    auto poll_again = std::move(rig.scheduled.front());
    rig.scheduled.clear();
    poll_again();
    rig.commitRender(rig.last("get_edit_result").request_id);
    REQUIRE(rig.order.undoRecords().size() == 2);
    CHECK(rig.order.undoRecords()[0].sent == first.sent);
    CHECK(rig.order.undoRecords()[0].serial == rig.history.undoStack().back().serial);
    CHECK(rig.order.undoRecords()[1].serial == 0);
    rig.graph.receiveRpcResult(later_poll.request_id,
        Json{{"status", "rejected"}, {"error", {{"code", code}}}}.dump());
    rig.readGraph();
    REQUIRE(rig.order.undoRecords().size() == 1);
    CHECK(rig.order.undoRecords()[0].sent == first.sent);
    const auto retry = rig.submitGraph();
    REQUIRE(rig.order.undoRecords().size() == 2);
    CHECK(rig.order.undoRecords().back().sent > discarded_key);
    CHECK(rig.order.undoRecords().back().serial == 0);
    rig.commitGraph(retry);
    CHECK(rig.document["fold_groups"]["group"]["folded"].contains("verified"));
    CHECK(rig.order.undoRecords().back().history == Order::History::Graph);
    CHECK(rig.order.undoRecords().back().serial == rig.graph.undoSerial());
    CHECK(rig.order.redoRecords().empty());
}

TEST_CASE("RT-D submission order is independent of Graph and Render commit notification order",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    const auto render = rig.submitRender();
    const auto poll = rig.accept(render);
    const auto edit = rig.submitGraph();
    rig.commitGraph(edit);
    REQUIRE(rig.order.undoRecords().size() == 2);
    CHECK(rig.order.undoRecords()[0].serial == 0);
    CHECK(rig.order.undoRecords()[1].serial == rig.graph.undoSerial());
    rig.commitRender(poll);
    const auto records = rig.order.undoRecords();
    CHECK(records[0].history == Order::History::Render);
    CHECK(records[1].history == Order::History::Graph);
    CHECK(records[0].sent < records[1].sent);
    CHECK(records[0].serial == rig.history.undoStack().back().serial);
    REQUIRE(rig.order.undo());
    const auto inverse = rig.graphRequest("edit_graph_document");
    CHECK(inverse.params.at("ops")[0].at("op") == "replace_node");
    CHECK(inverse.params.at("ops")[0].at("node").at("id") == "value");
    CHECK_FALSE(inverse.params.at("ops")[0].at("node").contains("label"));
    CHECK(rig.order.undoRecords() == records); // Merely sent.
    rig.commitGraph(inverse);
    CHECK(rig.order.undoRecords().size() == 1);
    CHECK(rig.order.redoRecords().back() == records[1]);
}

TEST_CASE("RT-D render submission terminals remove only their own placeholders",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    rig.commitGraph(rig.submitGraph());
    const auto kept = rig.order.undoRecords();
    const auto id = rig.submitRender();
    REQUIRE(rig.order.undoRecords().size() == 2);
    CHECK_FALSE(rig.order.canUndo());
    SECTION("refused on submission") {
        REQUIRE(rig.history.receiveResult(id, {{"status", "rejected"}, {"error", {{"code", "gate_closed"}}}}));
    }
    SECTION("lost submission reply") { REQUIRE(rig.history.receiveFailure(id, "lost")); }
    SECTION("lost ticket reply") { REQUIRE(rig.history.receiveFailure(rig.accept(id), "lost")); }
    SECTION("refused accepted ticket") {
        REQUIRE(rig.history.receiveResult(rig.accept(id), {{"status", "rejected"},
            {"error", {{"code", "external_modification"}}}}));
    }
    SECTION("ticket poll could not be sent") { rig.fail_poll = true; rig.accept(id); }
    SECTION("abandon") {
        rig.history.abandonInFlight();
        CHECK(rig.order.undoRecords() == kept);
    }
    SECTION("clear") {
        rig.history.clear();
        CHECK(rig.order.undoRecords() == kept);
    }
    SECTION("no change commit") {
        auto commit = rig.commit();
        commit["no_change"] = true;
        commit.erase("documents");
        REQUIRE(rig.history.receiveResult(rig.accept(id), commit));
    }
    CHECK(rig.order.undoRecords() == kept);
    CHECK(rig.order.canUndo());
    rig.order.renderRequestSent(0);
    CHECK(rig.order.undoRecords() == kept);
}

TEST_CASE("RT-D malformed render commits retain a named barrier before older Graph Undo",
          "[devstudio][tab-edit-order][rtd]") {
    const auto malformed = GENERATE(Json{}, Json::array(), Json("wrong"), Json(7), Json::object());
    const auto accepted = GENERATE(false, true);
    Rig rig;
    rig.commitGraph(rig.submitGraph());
    const auto graph_record = rig.order.undoRecords().back();
    rig.graph.takeRpcRequests(); // Discard read-only refreshes from the prior Graph commit.
    const auto id = rig.submitRender();
    const auto placeholder = rig.order.undoRecords().back();
    auto commit = rig.commit();
    if (malformed.is_null()) commit.erase("documents");
    else commit["documents"] = malformed;
    REQUIRE(rig.history.receiveResult(accepted ? rig.accept(id) : id, commit));
    REQUIRE(rig.history.undoStack().size() == 1);
    const auto &barrier = rig.history.undoStack().back();
    REQUIRE(barrier.barrier);
    CHECK(barrier.barrier_reason.find("render_history.invalid_commit_response@1") != std::string::npos);
    CHECK(barrier.barrier_reason.find("documents") != std::string::npos);
    REQUIRE(rig.order.undoRecords().size() == 2);
    CHECK(rig.order.undoRecords()[0] == graph_record);
    CHECK(rig.order.undoRecords().back().sent == placeholder.sent);
    CHECK(rig.order.undoRecords().back().serial == barrier.serial);
    const auto records = rig.order.undoRecords();
    const auto requests = rig.requests.size();
    const auto line = rig.history.lineSerial();
    REQUIRE(rig.order.canUndo()); // Actionable only to explain the barrier.
    CHECK_FALSE(rig.order.undo());
    CHECK_FALSE(rig.order.undo());
    CHECK(rig.history.lineSerial() == line + 2);
    CHECK(rig.history.line().find(barrier.barrier_reason) != std::string::npos);
    CHECK(rig.graph.notice().message == rig.history.line());
    CHECK(rig.requests.size() == requests);
    CHECK(rig.graph.takeRpcRequests().empty());
    CHECK(rig.order.undoRecords() == records);
    CHECK(rig.order.redoRecords().empty());
    CHECK(rig.graph.canUndo()); // The older inverse exists but was not sent.
}

TEST_CASE("RT-D Graph rejection and abandon preserve committed serials and remove pending positions",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    rig.commitGraph(rig.submitGraph());
    const auto kept = rig.order.undoRecords();
    SECTION("ordinary refusal") {
        const auto request = rig.submitGraph("later");
        rig.graph.receiveRpcResult(request.request_id, R"({"status":"rejected","error":{"code":"external_modification"}})");
        rig.readGraph();
    }
    SECTION("engine restart") {
        rig.submitGraph("later");
        rig.graph.setConnected(false);
        rig.graph.setConnected(true);
        rig.readGraph();
    }
    SECTION("Undo refusal followed by retry") {
        REQUIRE(rig.order.undo());
        auto request = rig.graphRequest("edit_graph_document");
        rig.graph.receiveRpcResult(request.request_id, R"({"status":"rejected","error":{"code":"external_modification"}})");
        rig.readGraph();
        CHECK(rig.order.undoRecords() == kept);
        REQUIRE(rig.order.undo());
        request = rig.graphRequest("edit_graph_document");
        CHECK(rig.order.undoRecords() == kept);
        rig.commitGraph(request);
        CHECK(rig.order.redoRecords() == kept);
        REQUIRE(rig.order.redo());
        rig.commitGraph(rig.graphRequest("edit_graph_document"));
    }
    CHECK(rig.order.undoRecords() == kept);
    CHECK(rig.order.canUndo());
}

TEST_CASE("RT-D render restoration moves serials only at success including Redo refusal",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    const auto before = rig.document;
    const auto poll = rig.accept(rig.submitRender());
    rig.document["rendering"] = {{"present", {{"enabled", true}}}};
    rig.commitRender(poll);
    const auto record = rig.order.undoRecords().back();
    REQUIRE(rig.order.undo());
    CHECK(rig.order.undoRecords().back() == record);
    CHECK(rig.order.redoRecords().empty());
    rig.restore();
    CHECK(rig.document == before);
    REQUIRE(rig.order.redoRecords().back() == record);
    REQUIRE(rig.order.redo());
    rig.restore(true);
    CHECK(rig.order.redoRecords().back() == record);
    CHECK(rig.order.undoRecords().empty());
    REQUIRE(rig.order.redo());
    rig.restore();
    CHECK(rig.order.undoRecords().back() == record);
    REQUIRE(rig.order.undo());
    rig.restore();
    CHECK(rig.order.redoRecords().back() == record);
}

TEST_CASE("RT-D render barriers and synchronous refusals keep the tab on its record",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    rig.commitGraph(rig.submitGraph());
    rig.commitRender(rig.accept(rig.submitRender("edit_preset_pass", true, false)));
    const auto records = rig.order.undoRecords();
    REQUIRE(rig.history.undoStack().back().barrier);
    CHECK_FALSE(rig.order.undo());
    CHECK(rig.order.undoRecords() == records);
    CHECK(rig.order.redoRecords().empty());
    CHECK(rig.graph.canUndo());
}

TEST_CASE("RT-D files bind synchronously and historyMoved carries file Undo and Redo to the tab",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    const auto path = std::filesystem::temp_directory_path() /
        ("pelican-rtd-order-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Remove { std::filesystem::path path; ~Remove() { std::filesystem::remove(path); } } remove{path};
    { std::ofstream file{path}; file << "after"; }
    int restored = 0;
    rig.history.restored = [&](const auto &, bool) { ++restored; };
    rig.order.recordFiles("switch", {{path.string(), "before", "after"}});
    const auto record = rig.order.undoRecords().back();
    REQUIRE(record.serial != 0);
    REQUIRE(rig.order.undo());
    CHECK(rig.order.redoRecords().back() == record);
    { std::ifstream file{path}; std::string bytes; file >> bytes; CHECK(bytes == "before"); }
    REQUIRE(rig.order.redo());
    CHECK(rig.order.undoRecords().back() == record);
    { std::ifstream file{path}; std::string bytes; file >> bytes; CHECK(bytes == "after"); }
    CHECK(restored == 0);
    CHECK(rig.requests.empty());
    REQUIRE(rig.order.undo());
    // A new Graph commit consumes the tab's file Redo even though the
    // shared Render history still contains the old entry.
    rig.commitGraph(rig.submitGraph());
    CHECK(rig.order.redoRecords().empty());
    CHECK_FALSE(rig.order.redo());
    CHECK_FALSE(rig.history.redoStack().empty());
}

TEST_CASE("RT-D tab clear and disconnection ignore late render commits",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    const auto poll = rig.accept(rig.submitRender());
    SECTION("external reload or project boundary") {
        rig.order.clear();
        CHECK(rig.order.undoRecords().empty());
    }
    SECTION("history detached") {
        rig.order.setRenderHistory(nullptr);
        CHECK(rig.order.undoRecords().empty());
    }
    rig.commitRender(poll);
    CHECK(rig.order.undoRecords().empty());
    CHECK_FALSE(rig.order.undo());
    CHECK_FALSE(rig.history.undoStack().empty());
}

TEST_CASE("RT-D a new render commit consumes Graph Redo while a refused edit preserves it",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    rig.commitGraph(rig.submitGraph());
    REQUIRE(rig.order.undo());
    rig.commitGraph(rig.graphRequest("edit_graph_document"));
    const auto undone = rig.order.redoRecords();
    REQUIRE(undone.size() == 1);
    const auto refused = rig.submitRender();
    REQUIRE(rig.history.receiveResult(refused, {{"status", "rejected"},
        {"error", {{"code", "external_modification"}}}}));
    CHECK(rig.order.redoRecords() == undone);
    CHECK(rig.order.canRedo());
    rig.commitRender(rig.accept(rig.submitRender()));
    CHECK(rig.order.redoRecords().empty());
    CHECK_FALSE(rig.order.redo());
    CHECK(rig.graph.canRedo());
}

TEST_CASE("RT-D a placement retry reserves a fresh position and commits a single entry",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    REQUIRE(rig.graph.setLayout("", {{"nodes", {{"value", {{"x", 50}, {"y", 20}}}}}}));
    const auto sent = rig.order.undoRecords().back().sent;
    const auto request = rig.graphRequest("edit_graph_document");
    rig.graph.receiveRpcResult(request.request_id,
        R"({"status":"rejected","error":{"code":"external_modification"}})");
    CHECK(rig.order.undoRecords().empty());
    rig.readGraph();
    REQUIRE(rig.order.undoRecords().size() == 1);
    CHECK(rig.order.undoRecords().back().sent > sent);
    CHECK(rig.order.undoRecords().back().serial == 0);
    rig.commitGraph(rig.graphRequest("edit_graph_document"));
    REQUIRE(rig.order.undoRecords().size() == 1);
    CHECK(rig.order.undoRecords().back().serial == rig.graph.undoSerial());
}

TEST_CASE("RT-D shared history safely outlives its tab observer and pending completion",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    rig.order.setRenderHistory(nullptr);
    auto tab = std::make_unique<Order>(rig.graph);
    tab->setRenderHistory(&rig.history);
    const auto id = rig.submitRender("edit_preset_pass", false);
    tab->renderRequestSent(id);
    REQUIRE(tab->undoRecords().size() == 1);
    tab.reset();
    rig.commitRender(rig.accept(id));
    REQUIRE(rig.history.undo());
    rig.restore();
    CHECK(rig.history.redoStack().size() == 1);
}

TEST_CASE("RT-D tab order keeps at most 256 submissions and never renumbers surviving keys",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    for (int i = 0; i < 300; ++i)
        rig.order.recordFiles("switch", {{"unused-file", "before", "after"}});
    REQUIRE(rig.order.undoRecords().size() == 256);
    CHECK(rig.order.undoRecords().front().sent == 45);
    CHECK(rig.order.undoRecords().back().sent == 300);
    rig.order.clear();
    rig.order.recordFiles("switch", {{"unused-file", "before", "after"}});
    CHECK(rig.order.undoRecords().back().sent == 301);
}

TEST_CASE("RT-D reversed render notifications undo R2 then R1 using the digest chain",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    rig.content_digests = true;
    const auto original = rig.document;
    const auto first = rig.accept(rig.submitRender("fold_nodes"), "R1");
    rig.document["fold_groups"]["group"]["folded"]["decisions"] = {{"isolate", {"value"}}};
    const auto folded = rig.document;
    const auto first_commit = rig.commit();
    const auto second = rig.accept(rig.submitRender("verify_fold"), "R2");
    rig.document["fold_groups"]["group"]["folded"]["verified"] = {{"digest", "verified"}};
    const auto verified = rig.document;
    std::vector<std::uint64_t> pushes;
    rig.history.historyMoved = [&](std::uint64_t serial, History::Move move, bool ok) {
        if (move != History::Move::Push) return;
        REQUIRE(ok);
        // Push identifies the inserted entry, which need not be stack.back().
        const auto &stack = rig.history.undoStack();
        REQUIRE(std::ranges::find(stack, serial, &History::Entry::serial) != stack.end());
        pushes.push_back(serial);
    };
    REQUIRE(rig.history.receiveResult(second, rig.commit()));
    rig.readHistory();
    REQUIRE(rig.order.undoRecords()[0].serial == 0);
    const auto second_serial = rig.order.undoRecords()[1].serial;
    REQUIRE(second_serial != 0);
    CHECK_FALSE(rig.order.undo()); // Both histories still have R1 pending.
    REQUIRE(rig.history.receiveResult(first, first_commit));
    rig.readHistory();
    const auto records = rig.order.undoRecords();
    REQUIRE(records.size() == 2);
    CHECK(pushes == std::vector<std::uint64_t>{second_serial, records[0].serial});
    REQUIRE(rig.history.undoStack().back().serial == second_serial);
    REQUIRE(rig.order.undo());
    rig.restore();
    CHECK(rig.document == folded);
    REQUIRE(rig.order.undo());
    rig.restore();
    CHECK(rig.document == original);
    REQUIRE(rig.order.redo());
    rig.restore();
    CHECK(rig.document == folded);
    REQUIRE(rig.order.redo());
    rig.restore();
    CHECK(rig.document == verified);
    CHECK(rig.order.undoRecords() == records);
}

TEST_CASE("RT-D a synchronous file entry stays above an earlier RPC observed later",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    rig.content_digests = true;
    const auto original = rig.document;
    const auto poll = rig.accept(rig.submitRender());
    rig.document["rendering"] = {{"changed", true}};
    const auto commit = rig.commit();
    const auto path = std::filesystem::temp_directory_path() /
        ("pelican-rtd-mixed-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Remove { std::filesystem::path path; ~Remove() {
        std::error_code error; std::filesystem::remove(path, error);
    } } remove{path};
    { std::ofstream file{path}; file << "after"; }
    rig.order.recordFiles("switch", {{path.string(), "before", "after"}});
    const auto file_record = rig.order.undoRecords().back();
    REQUIRE(rig.history.receiveResult(poll, commit));
    rig.readHistory();
    REQUIRE(rig.history.undoStack().back().serial == file_record.serial);
    const auto requests = rig.requests.size();
    REQUIRE(rig.order.undo());
    CHECK(rig.requests.size() == requests);
    { std::ifstream file{path}; std::string bytes; file >> bytes; CHECK(bytes == "before"); }
    REQUIRE(rig.order.undo());
    rig.restore();
    CHECK(rig.document == original);
    REQUIRE(rig.order.redo());
    rig.restore();
    REQUIRE(rig.order.redo());
    { std::ifstream file{path}; std::string bytes; file >> bytes; CHECK(bytes == "after"); }
    CHECK(rig.order.undoRecords().back() == file_record);
}

TEST_CASE("RT-D digest chains join across notification gaps and repeated document bytes",
          "[devstudio][tab-edit-order][rtd]") {
    const auto arrival = GENERATE("012", "021", "102", "120", "201", "210");
    const auto revisit = GENERATE(false, true);
    INFO("arrival " << arrival << ", repeated bytes " << revisit);
    Rig rig;
    rig.content_digests = true;
    std::vector<Json> states{rig.document};
    std::vector<Json> commits;
    std::vector<std::uint64_t> polls;
    for (int i = 0; i < 3; ++i) {
        polls.push_back(rig.accept(rig.submitRender(), "R" + std::to_string(i)));
        if (revisit && i == 1) rig.document = states[0];
        else rig.document["rendering"] = {{"version", i + 1}};
        states.push_back(rig.document);
        commits.push_back(rig.commit());
    }
    for (int i = 0; i < 3; ++i) {
        const auto index = static_cast<unsigned>(arrival[i] - '0');
        REQUIRE(rig.history.receiveResult(polls[index], commits[index]));
        rig.readHistory();
    }
    for (int i = 2; i >= 0; --i) {
        REQUIRE(rig.order.undo());
        rig.restore();
        CHECK(rig.document == states[i]);
    }
    for (int i = 1; i <= 3; ++i) {
        REQUIRE(rig.order.redo());
        rig.restore();
        CHECK(rig.document == states[i]);
    }
}

TEST_CASE("RT-D independent render documents retain observation order",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    const auto root_digest = rig.digest();
    const std::string a = "project://passes/a.json", b = "project://passes/b.json";
    const std::map<std::string, std::string> before{{a, R"({"pass":"a","value":1})"},
                                                {b, R"({"pass":"b","value":2})"}};
    const std::map<std::string, std::string> after{{a, R"({"pass":"a","value":11})"},
                                               {b, R"({"pass":"b","value":22})"}};
    auto current = before;
    const auto digest = [&](const std::string &reference) {
        return Pelican::renderConfigSourceDigest(current.at(reference));
    };
    const auto snapshot = [&] {
        auto documents = Json::array();
        for (const auto &[reference, content] : current)
            documents.push_back({{"reference", reference}, {"kind", "preset"},
                                 {"digest", digest(reference)}, {"content", content}});
        return Json{{"documents", std::move(documents)}};
    };
    const auto commit = [&](const std::string &reference) {
        return Json{{"status", "committed"}, {"committed", true},
                    {"source_digest", {{"hex", root_digest}}},
                    {"documents", {{reference, digest(reference)}}}};
    };
    const auto readDocuments = [&] {
        for (std::size_t i = 0; i < rig.requests.size(); ++i) {
            const auto request = rig.requests[i];
            if (request.method == "get_render_documents" && rig.answered.insert(request.request_id).second)
                REQUIRE(rig.history.receiveResult(request.request_id, snapshot()));
        }
    };
    std::vector<std::uint64_t> polls;
    for (const auto &reference : {a, b}) {
        rig.history.beforeRequest("edit_preset_pass", {{"base_source_digest", root_digest}});
        const auto id = rig.next++;
        rig.history.requestSent(id, "edit_preset_pass", {});
        rig.order.renderRequestSent(id);
        readDocuments();
        polls.push_back(rig.accept(id, reference));
        current[reference] = after.at(reference);
    }
    std::vector<std::uint64_t> observed;
    for (const auto i : {1, 0}) {
        const auto &reference = i == 0 ? a : b;
        REQUIRE(rig.history.receiveResult(polls[i], commit(reference)));
        observed.push_back(rig.history.undoStack().back().serial);
        readDocuments();
    }
    REQUIRE(rig.history.undoStack().size() == 2);
    CHECK(rig.history.undoStack()[0].serial == observed[0]);
    CHECK(rig.history.undoStack()[0].before.contains(b));
    CHECK(rig.history.undoStack()[1].serial == observed[1]);
    CHECK(rig.history.undoStack()[1].before.contains(a));
    const auto records = rig.order.undoRecords();
    REQUIRE(records.size() == 2);
    CHECK(records[0].serial == observed[1]); // R1, sent first, observed last.
    CHECK(records[1].serial == observed[0]); // R2, sent last, observed first.
    CHECK(records[0].sent < records[1].sent);
    REQUIRE(records.back().serial != rig.history.undoStack().back().serial);

    const auto restore = [&](const std::string &reference, bool redo) {
        const auto undo_records = rig.order.undoRecords();
        const auto redo_records = rig.order.redoRecords();
        REQUIRE((redo ? rig.order.redo() : rig.order.undo()));
        const auto read = rig.last("get_render_documents");
        CHECK(read.params.at("references") == Json::array({reference}));
        readDocuments();
        const auto request = rig.last("restore_render_documents");
        const auto &documents = request.params.at("documents");
        REQUIRE(documents.size() == 1);
        CHECK(documents[0].at("reference") == reference);
        CHECK(documents[0].at("base_digest") == digest(reference));
        CHECK(documents[0].at("content") == (redo ? after : before).at(reference));
        // Sending and even accepting a restore must not move tab records.
        CHECK(rig.order.undoRecords() == undo_records);
        CHECK(rig.order.redoRecords() == redo_records);
        REQUIRE(rig.history.receiveResult(request.request_id,
            {{"status", "accepted"}, {"ticket", "restore"}}));
        CHECK(rig.order.undoRecords() == undo_records);
        CHECK(rig.order.redoRecords() == redo_records);
        current[reference] = documents[0].at("content").get<std::string>();
        REQUIRE(rig.history.receiveResult(rig.last("get_edit_result").request_id, commit(reference)));
    };
    // Shared top is R1: it belongs to this tab even though R2 is last in
    // submission order. Its independent document can be restored first.
    restore(a, false);
    CHECK(current.at(a) == before.at(a));
    CHECK(current.at(b) == after.at(b));
    CHECK(rig.order.undoRecords() == std::vector<Order::Record>{records[1]});
    CHECK(rig.order.redoRecords() == std::vector<Order::Record>{records[0]});
    restore(b, false);
    CHECK(current == before);
    CHECK(rig.order.undoRecords().empty());
    CHECK_FALSE(rig.order.canUndo());
    CHECK(rig.order.redoRecords() == std::vector<Order::Record>{records[1], records[0]});
    // Redo is symmetric: shared top R2 is not the last tab Redo record.
    REQUIRE(rig.order.redoRecords().back().serial != rig.history.redoStack().back().serial);
    restore(b, true);
    CHECK(current.at(a) == before.at(a));
    CHECK(current.at(b) == after.at(b));
    CHECK(rig.order.undoRecords() == std::vector<Order::Record>{records[1]});
    CHECK(rig.order.redoRecords() == std::vector<Order::Record>{records[0]});
    restore(a, true);
    CHECK(current == after);
    CHECK(rig.order.undoRecords() == records);
    CHECK(rig.order.redoRecords().empty());
    CHECK_FALSE(rig.order.canRedo());
    restore(a, false); // Undo after Redo still follows the owned shared top.
    restore(b, false);
    CHECK(current == before);
}

TEST_CASE("RT-D stale source layout refusal rereads without automatic resubmission",
          "[devstudio][tab-edit-order][rtd]") {
    Rig rig;
    REQUIRE(rig.graph.setLayout("", {{"nodes", {{"value", {{"x", 50}, {"y", 20}}}}}}));
    const auto request = rig.graphRequest("edit_graph_document");
    REQUIRE(rig.order.undoRecords().size() == 1);
    rig.graph.receiveRpcResult(request.request_id,
        R"({"status":"rejected","error":{"code":"stale_source_digest"}})");
    CHECK(rig.order.undoRecords().empty());
    rig.readGraph();
    const auto requests = rig.graph.takeRpcRequests();
    CHECK(std::ranges::none_of(requests, [](const auto &r) { return r.method == "edit_graph_document"; }));
    CHECK(rig.order.undoRecords().empty());
    CHECK(rig.graph.canEdit());
}


TEST_CASE("UD-6 asset-only reload preserves Graph and Render Undo and Redo",
          "[devstudio][tab-edit-order][ud6][render_reload]") {
    Rig rig;
    rig.content_digests = true;
    rig.graph.refresh();
    rig.readGraph();
    const auto initial = rig.document;
    rig.commitGraph(rig.submitGraph("authored label"));
    const auto after_graph = rig.document;
    const auto render = rig.submitRender();
    const auto render_poll = rig.accept(render);
    rig.document["rendering"] = {{"draw_sort", "state"}};
    rig.graph.keepHistoryThroughNextDigest();
    rig.commitRender(render_poll);
    rig.graph.refresh();
    rig.readGraph();
    const auto after_render = rig.document;
    REQUIRE(rig.history.canUndo());
    REQUIRE(rig.graph.canUndo());

    // The live digest read by each panel is the one before the outside image
    // write. Use the real reload policy, histories, and tab order together.
    PelicanStudio::RenderDocumentReload reload{{
        .request = [](const std::string &, const Json &, std::string *) { return std::int64_t{1}; },
        .schedule = [](int, std::function<void()>) {},
    }};
    reload.open("project_root");
    reload.engineReady();
    REQUIRE(reload.receiveResult(1, rig.snapshot()));
    int boundaries = 0;
    int refreshed = 0;
    reload.historyBoundary = [&] {
        ++boundaries;
        rig.history.cut("reloaded from disk");
        rig.graph.forgetHistory();
        rig.order.clear();
    };
    reload.reloaded = [&](const Json &) {
        ++refreshed;
        rig.graph.refresh();
        rig.readGraph();
    };
    const auto image_reload = [&] {
        reload.reloadNow();
        auto commit = rig.commit();
        commit["changed"] = Json::array();
        commit["changed_assets"] = Json::array({"assets/images/ltc_2.ktx2"});
        REQUIRE(reload.receiveResult(1, commit));
    };
    image_reload();
    CHECK(boundaries == 0);
    CHECK(refreshed == 1);
    REQUIRE(rig.history.canUndo());
    REQUIRE(rig.graph.canUndo());
    REQUIRE(rig.order.canUndo());
    REQUIRE(rig.order.undo());
    rig.restore();
    rig.graph.refresh();
    rig.readGraph();
    CHECK(rig.document == after_graph);
    REQUIRE(rig.order.undo());
    rig.commitGraph(rig.graphRequest("edit_graph_document"));
    CHECK(rig.document == initial);
    REQUIRE(rig.graph.canRedo());
    REQUIRE(rig.history.canRedo());

    // A second image update must preserve Redo as well as Undo.
    reload.observeReply(rig.commit());
    image_reload();
    CHECK(boundaries == 0);
    CHECK(refreshed == 2);
    REQUIRE(rig.order.redo());
    rig.commitGraph(rig.graphRequest("edit_graph_document"));
    CHECK(rig.document == after_graph);
    REQUIRE(rig.order.redo());
    rig.restore();
    rig.graph.refresh();
    rig.readGraph();
    CHECK(rig.document == after_render);
}

TEST_CASE("UD-6 external document reload still cuts Graph and Render histories",
          "[devstudio][tab-edit-order][ud6][render_reload]") {
    Rig rig;
    rig.commitGraph(rig.submitGraph());
    const auto render = rig.submitRender();
    rig.commitRender(rig.accept(render));
    REQUIRE(rig.graph.canUndo());
    REQUIRE(rig.history.canUndo());
    SECTION("Undo entries") { REQUIRE(rig.order.canUndo()); }
    SECTION("Redo entries") {
        REQUIRE(rig.order.undo());
        rig.restore();
        rig.graph.refresh();
        rig.readGraph();
        REQUIRE(rig.order.undo());
        rig.commitGraph(rig.graphRequest("edit_graph_document"));
        REQUIRE(rig.graph.canRedo());
        REQUIRE(rig.history.canRedo());
    }
    PelicanStudio::RenderDocumentReload reload{{
        .request = [](const std::string &, const Json &, std::string *) { return std::int64_t{1}; },
        .schedule = [](int, std::function<void()>) {},
    }};
    reload.open("project_root");
    reload.engineReady();
    REQUIRE(reload.receiveResult(1, rig.snapshot()));
    int boundaries = 0;
    reload.historyBoundary = [&] {
        ++boundaries;
        rig.history.cut("reloaded from disk");
        rig.graph.forgetHistory();
        rig.order.clear();
    };
    reload.reloadNow();
    auto commit = rig.commit();
    // Do not rely on changed or changed_assets to establish a boundary.
    commit["documents"][Root] = std::string(64, 'f');
    commit["changed"] = Json::array();
    commit["changed_assets"] = Json::array({"assets/images/ltc_2.ktx2"});
    REQUIRE(reload.receiveResult(1, commit));
    CHECK(boundaries == 1);
    CHECK_FALSE(rig.graph.canUndo());
    CHECK_FALSE(rig.graph.canRedo());
    CHECK_FALSE(rig.history.canUndo());
    CHECK_FALSE(rig.history.canRedo());
    CHECK(rig.order.undoRecords().empty());
    CHECK(rig.order.redoRecords().empty());
}
