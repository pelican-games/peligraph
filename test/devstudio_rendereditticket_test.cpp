#include "rendereditticket.hpp"
#include "rendereditticketclient.hpp"
#include "raylayoutwidget.hpp"
#include "projectmigrationwidget.hpp"
#include "engineattach.hpp"
#include "enginefailuremodel.hpp"
#include "engineprocess.hpp"
#include "nativewindowhost.hpp"
#include "viewportgeometry.hpp"
#define private public
#include "embeddedviewport.hpp"
#undef private
#include "raylayoutmodel.hpp"
#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include "../src/devstudio/guitrace.hpp"
#include <QScopeGuard>
#include "graphattachmentmodel.hpp"
#include "graphattachmentwidget.hpp"
#include "graphcanvaswidget.hpp"
#include "renderconfigdigest.hpp"
#include <catch2/catch_test_macros.hpp>
#include <QApplication>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPushButton>
#include <QThread>
#include <algorithm>
#include <deque>
#include <memory>

namespace {
using PelicanStudio::RenderEditTicket;
using Json = RenderEditTicket::Json;
using Kind = RenderEditTicket::Kind;

void ensureApplication() {
    static int argc = 1;
    static char name[] = "render_edit_ticket_test";
    static char *argv[] = {name, nullptr};
    static QApplication application{argc, argv};
}

struct Harness {
    struct Call { std::int64_t id; std::string method; Json params; };
    struct Timer { int ms; std::function<void()> task; };
    std::deque<Call> calls;
    std::deque<Timer> timers;
    std::vector<std::string> digests;
    std::vector<RenderEditTicket::Outcome> outcomes;
    std::int64_t next = 1;
    bool broken = false;
    RenderEditTicket edit{
        {[this](const auto &method, const auto &params, std::string *error) {
             if (broken) { *error = "transport unavailable"; return std::int64_t{0}; }
             calls.push_back({next, method, params});
             return next++;
         },
         [this](int ms, auto task) { timers.push_back({ms, std::move(task)}); }},
        [this](const auto &digest) { digests.push_back(digest); }};
    Harness() { edit.changed = [this](const auto &outcome) { outcomes.push_back(outcome); }; }
    std::int64_t take(const char *method) {
        REQUIRE_FALSE(calls.empty());
        const auto call = calls.front(); calls.pop_front();
        REQUIRE(call.method == method);
        if (call.method == "get_edit_result") REQUIRE(call.params == Json{{"ticket", "edit-1"}});
        return call.id;
    }
    void tick(int ms) {
        REQUIRE_FALSE(timers.empty());
        auto timer = std::move(timers.front()); timers.pop_front();
        REQUIRE(timer.ms == ms);
        timer.task();
    }
    void accept() {
        REQUIRE(edit.submit("edit_rendering_keys", Json::object()));
        REQUIRE(edit.receive(take("edit_rendering_keys"), {{"status", "accepted"}, {"ticket", "edit-1"}}).kind == Kind::Accepted);
        REQUIRE(edit.busy());
        REQUIRE(edit.ticket() == "edit-1");
        tick(0);
    }
};
Json commit(bool digest = true) {
    Json result{{"status", "committed"}, {"committed", true}, {"published_generation", std::uint64_t{9007199254740993ULL}}};
    if (digest) result["source_digest"] = {{"hex", std::string(64, 'b')}};
    return result;
}
}

TEST_CASE("RenderEditTicket follows pending and running to one committed digest", "[studio][render-edit-ticket]") {
    Harness h;
    h.accept();
    REQUIRE_FALSE(h.edit.submit("second_edit", Json::object()));
    REQUIRE(h.calls.size() == 1);
    const auto first = h.take("get_edit_result");
    REQUIRE(h.edit.receive(first + 100, commit()).kind == Kind::NotMine);
    REQUIRE(h.edit.busy());
    REQUIRE(h.edit.receive(first, {{"status", "pending"}}).kind == Kind::Pending);
    REQUIRE(h.edit.busy());
    REQUIRE(h.edit.ticket() == "edit-1");
    REQUIRE(h.calls.empty());
    h.tick(25);
    REQUIRE(h.edit.receive(h.take("get_edit_result"), {{"status", "running"}}).kind == Kind::Pending);
    REQUIRE(h.edit.busy());
    h.tick(500);
    const auto id = h.take("get_edit_result");
    const auto result = h.edit.receive(id, commit());
    REQUIRE(result.kind == Kind::Committed);
    REQUIRE(result.result.at("published_generation").get<std::uint64_t>() == 9007199254740993ULL);
    REQUIRE_FALSE(h.edit.busy());
    REQUIRE(h.edit.ticket().empty());
    REQUIRE(h.digests == std::vector<std::string>{std::string(64, 'b')});
    REQUIRE(h.edit.receive(id, commit()).kind == Kind::NotMine);
    REQUIRE(h.digests.size() == 1);
    REQUIRE(h.timers.empty());
}

TEST_CASE("RenderEditTicket accepts immediate commits and preserves refusal results", "[studio][render-edit-ticket]") {
    Harness h;
    SECTION("immediate committed with digest") {
        REQUIRE(h.edit.submit("edit", Json::object()));
        REQUIRE(h.edit.receive(h.take("edit"), commit()).kind == Kind::Committed);
        REQUIRE(h.digests.size() == 1);
    }
    SECTION("committed without digest") {
        h.accept();
        REQUIRE(h.edit.receive(h.take("get_edit_result"), commit(false)).kind == Kind::Committed);
        REQUIRE(h.digests.empty());
    }
    SECTION("rejected at submission") {
        REQUIRE(h.edit.submit("edit", Json::object()));
        Json refusal{{"status", "rejected"}, {"error", {{"code", "external_modification"}}}};
        const auto result = h.edit.receive(h.take("edit"), refusal);
        REQUIRE(result.kind == Kind::Refused);
        REQUIRE(result.result == refusal);
    }
    SECTION("rejected after accepted") {
        h.accept();
        Json refusal{{"status", "rejected"}, {"error", "do not retry"}};
        const auto result = h.edit.receive(h.take("get_edit_result"), refusal);
        REQUIRE(result.kind == Kind::Refused);
        REQUIRE(result.result == refusal);
        REQUIRE(h.digests.empty());
    }
    SECTION("accepted without ticket") {
        for (const auto &accepted : {Json{{"status", "accepted"}},
                                    Json{{"status", "accepted"}, {"ticket", ""}},
                                    Json{{"status", "accepted"}, {"ticket", nullptr}}}) {
            REQUIRE(h.edit.submit("edit", Json::object()));
            const auto result = h.edit.receive(h.take("edit"), accepted);
            REQUIRE(result.kind == Kind::Refused);
            REQUIRE(result.error == "The engine returned no edit ticket.");
            REQUIRE(result.cause == RenderEditTicket::Cause::MissingTicket);
        }
    }
    REQUIRE_FALSE(h.edit.busy());
    REQUIRE(h.edit.ticket().empty());
    REQUIRE(h.timers.empty());
}

TEST_CASE("RenderEditTicket separates failed submission from unconfirmed polls", "[studio][render-edit-ticket]") {
    Harness h;
    SECTION("transport cannot submit") {
        h.broken = true;
        REQUIRE_FALSE(h.edit.submit("edit", Json::object()));
        REQUIRE(h.outcomes.back().kind == Kind::Failed);
        REQUIRE(h.outcomes.back().error == "transport unavailable");
    }
    SECTION("submission failure signal") {
        REQUIRE(h.edit.submit("edit", Json::object()));
        REQUIRE(h.edit.fail(h.take("edit"), "service error").kind == Kind::Failed);
    }
    SECTION("submission service error") {
        REQUIRE(h.edit.submit("edit", Json::object()));
        REQUIRE(h.edit.receive(h.take("edit"), {{"error", "service error"}}).kind == Kind::Failed);
    }
    REQUIRE_FALSE(h.edit.busy());
    REQUIRE(h.edit.ticket().empty());
}

TEST_CASE("RenderEditTicket unconfirmed malformed service and unknown replies resume the same ticket", "[studio][render-edit-ticket]") {
    Harness h;
    h.accept();
    for (const auto &response : {Json::parse("invalid JSON", nullptr, false), Json{{"error", {{"message", "Temporary service error"}}}}, Json{{"status", "unexpected"}}}) {
        const auto result = h.edit.receive(h.take("get_edit_result"), response);
        REQUIRE(result.kind == Kind::Unconfirmed);
        REQUIRE_FALSE(result.error.empty());
        REQUIRE(h.edit.busy());
        REQUIRE(h.edit.ticket() == "edit-1");
        REQUIRE_FALSE(h.edit.submit("second_edit", Json::object()));
        h.tick(500);
    }
    SECTION("rejected after recovering") {
        REQUIRE(h.edit.receive(h.take("get_edit_result"), {{"status", "rejected"}}).kind == Kind::Refused);
    }
    SECTION("committed after recovering") {
        REQUIRE(h.edit.receive(h.take("get_edit_result"), commit()).kind == Kind::Committed);
        REQUIRE(h.digests.size() == 1);
    }
    REQUIRE_FALSE(h.edit.busy());
    REQUIRE(h.edit.ticket().empty());
    CHECK(std::count_if(h.outcomes.begin(), h.outcomes.end(), [](const auto &outcome) {
        return outcome.kind == Kind::Unconfirmed;
    }) == 1);
}

TEST_CASE("RenderEditTicket failed poll transport stays busy and can resume", "[studio][render-edit-ticket]") {
    Harness h;
    h.accept();
    const auto poll = h.take("get_edit_result");
    REQUIRE(h.edit.fail(poll + 100, "someone else's error").kind == Kind::NotMine);
    REQUIRE(h.edit.fail(poll, "offline").kind == Kind::Unconfirmed);
    REQUIRE(h.edit.busy());
    h.broken = true;
    h.edit.resume();
    REQUIRE(h.outcomes.back().kind == Kind::Unconfirmed);
    REQUIRE(h.edit.ticket() == "edit-1");
    h.broken = false;
    h.edit.resume();
    REQUIRE(h.edit.receive(h.take("get_edit_result"), commit()).kind == Kind::Committed);
}

TEST_CASE("RenderEditTicket abandon ignores delayed replies and scheduled polls", "[studio][render-edit-ticket]") {
    Harness h;
    h.accept();
    const auto poll = h.take("get_edit_result");
    h.edit.receive(poll, {{"status", "pending"}});
    h.edit.abandon();
    REQUIRE_FALSE(h.edit.busy());
    REQUIRE(h.edit.receive(poll, commit()).kind == Kind::NotMine);
    REQUIRE(h.edit.fail(poll, "old failure").kind == Kind::NotMine);
    REQUIRE(h.edit.submit("new_edit", Json::object()));
    h.tick(25);
    REQUIRE(h.take("new_edit") > poll);
    REQUIRE(h.calls.empty());
    REQUIRE(h.digests.empty());
}

TEST_CASE("RenderEditTicket delayed callback is harmless after destruction", "[studio][render-edit-ticket]") {
    std::function<void()> delayed;
    int sent = 0;
    {
        RenderEditTicket edit{{[&](const auto &, const auto &, auto *) { return ++sent; },
                               [&](int, auto task) { delayed = std::move(task); }}};
        REQUIRE(edit.submit("edit", Json::object()));
        edit.receive(1, {{"status", "accepted"}, {"ticket", "edit-1"}});
    }
    REQUIRE(delayed);
    delayed();
    REQUIRE(sent == 1);
}

TEST_CASE("RenderEditTicket Attachment commit makes the production Graph digest subscriber reread", "[studio][render-edit-ticket]") {
    ensureApplication();
    std::int64_t next = 1;
    std::vector<std::int64_t> graph_reads;
    std::int64_t catalog_read = 0;
    PelicanStudio::GraphCanvasReadCapability capability;
    capability.ready = [](QObject *, auto handler) { handler(true, {}); };
    capability.requestGraphInstances = [&](QString *) {
        graph_reads.push_back(next);
        return next++;
    };
    capability.requestGraphCatalog = [&](QString *) { catalog_read = next; return next++; };
    capability.result = [](QObject *, auto) {};
    capability.failure = [](QObject *, auto) {};
    PelicanStudio::GraphCanvasWidget graph{std::move(capability)};
    auto hub = std::make_shared<PelicanStudio::RenderConfigDigest>();
    graph.setRenderConfigDigest(hub);

    Json context{{"source_digest", {{"hex", "source-a"}}},
                 {"nodes", Json::array()}, {"instances", Json::array()},
                 {"definitions", Json::object()}};
    REQUIRE(graph_reads.size() == 1);
    graph.receiveResult(graph_reads.back(), QByteArray::fromStdString(context.dump()));
    if (catalog_read) graph.receiveResult(catalog_read,
        QByteArray::fromStdString(Json{{"schema", "pelican.graph_catalog"}, {"version", 1},
                                      {"modules", Json::array()}}.dump()));
    REQUIRE(graph.model().contextReceived());
    REQUIRE(graph.model().digest() == "source-a");

    PelicanStudio::GraphAttachmentModel attachment;
    QObject source;
    const auto exercise = [&](bool connected) {
    if (connected) attachment.publishDigest = [hub, &source](const std::string &digest) {
        hub->publish(digest, &source);
    };
    attachment.setConnected(true);
    attachment.selectOwner(PelicanStudio::GraphAttachmentOwner{"main", 41});
    auto requests = attachment.takeRpcRequests();
    REQUIRE(requests.size() == 1);
    auto attached = context;
    attached["definitions"] = {{"unit", {{"nodes", Json::array()}}}};
    attached["instances"] = Json::array({{{"id", "left"}, {"op", "graph.instance"},
        {"definition", "unit"}, {"owner", {{"scene", "main"}, {"object", 41}}}}});
    attachment.receiveRpcResult(requests.front().request_id, attached.dump());
    REQUIRE(attachment.detach("left"));
    requests = attachment.takeRpcRequests();
    REQUIRE(requests.size() == 1);
    attachment.receiveRpcResult(requests.front().request_id,
                                Json{{"status", "accepted"}, {"ticket", "edit-1"}}.dump());
    requests = attachment.takeRpcRequests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().method == "get_edit_result");
    attachment.receiveRpcResult(requests.front().request_id, commit().dump());
    requests = attachment.takeRpcRequests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests.front().method == "get_graph_instances");
    attached["source_digest"]["hex"] = std::string(64, 'b');
    attachment.receiveRpcResult(requests.front().request_id, attached.dump());
    REQUIRE_FALSE(attachment.busy());
    REQUIRE(attachment.canEdit());
    if (connected) {
        REQUIRE(hub->current() == std::string(64, 'b'));
        REQUIRE(graph_reads.size() == 2);
        REQUIRE(graph.model().digest() == "source-a"); // Still the context the draft was built from.
        context["source_digest"]["hex"] = std::string(64, 'b');
        graph.receiveResult(graph_reads.back(), QByteArray::fromStdString(context.dump()));
        REQUIRE(graph.model().digest() == std::string(64, 'b'));
        REQUIRE(graph.model().canRefresh());
    } else {
        REQUIRE(hub->current() == "source-a");
        REQUIRE(graph_reads.size() == 1);
        REQUIRE(graph.model().digest() == "source-a");
    }
    };
    SECTION("publication connects Attachment to the real Graph subscriber") { exercise(true); }
    SECTION("missing publication leaves the Graph on its old context") { exercise(false); }
}

TEST_CASE("RenderEditTicket Attachment widget schedules polls resumes unconfirmed results and publishes to its hub", "[studio][render-edit-ticket]") {
    ensureApplication();
    struct Call { quint64 id; QString method; QJsonObject params; };
    std::deque<Call> calls;
    PelicanStudio::GraphAttachmentWidget attachment;
    auto hub = std::make_shared<PelicanStudio::RenderConfigDigest>();
    attachment.setRenderConfigDigest(hub);
    QObject observer;
    int publications = 0;
    int committed_publications = 0;
    hub->subscribeCommitted(&observer, [&](const auto &) { ++committed_publications; });
    hub->subscribe(&observer, [&](const auto &) {
        CHECK(committed_publications == 1); // History permission precedes the reread.
        ++publications;
    });
    QObject::connect(&attachment, &PelicanStudio::GraphAttachmentWidget::rpcRequested,
                     &attachment, [&](quint64 id, const QString &method, const QJsonObject &params) {
                         calls.push_back({id, method, params});
                     });
    const auto hasPoll = [&] {
        return std::any_of(calls.begin(), calls.end(), [](const auto &call) {
            return call.method == QStringLiteral("get_edit_result");
        });
    };
    const auto take = [&](const char *method) {
        QElapsedTimer timeout;
        timeout.start();
        for (;;) {
            const auto found = std::find_if(calls.begin(), calls.end(), [&](const auto &call) {
                return call.method == QString::fromLatin1(method);
            });
            if (found != calls.end()) {
                auto call = *found;
                calls.erase(found);
                return call;
            }
            REQUIRE(timeout.elapsed() < 1500);
            QApplication::processEvents();
            QThread::msleep(1);
        }
    };
    const auto reply = [&](const Call &call, const Json &result) {
        attachment.receiveRpcResult(call.id, QByteArray::fromStdString(result.dump()));
    };
    attachment.setOwner(PelicanStudio::GraphAttachmentOwner{"main", 41});
    attachment.setConnected(true);
    Json context{{"source_digest", {{"hex", "source-a"}}},
                 {"definitions", {{"unit", {{"nodes", Json::array()}}}}},
                 {"nodes", Json::array()},
                 {"instances", Json::array({{{"id", "left"}, {"op", "graph.instance"},
                     {"definition", "unit"}, {"owner", {{"scene", "main"}, {"object", 41}}}}})}};
    reply(take("get_graph_instances"), context);
    auto *instances = attachment.findChild<QListWidget *>("pelican.graphAttachments");
    auto *remove = attachment.findChild<QPushButton *>("pelican.graphDetach");
    REQUIRE(instances);
    REQUIRE(remove);
    instances->setCurrentRow(0);
    REQUIRE(remove->isEnabled());
    remove->click();
    reply(take("edit_graph_instances"), {{"status", "accepted"}, {"ticket", "edit-1"}});
    REQUIRE_FALSE(remove->isEnabled());
    auto poll = take("get_edit_result");
    REQUIRE(poll.params.value("ticket").toString() == "edit-1");
    attachment.receiveRpcResult(poll.id, QByteArray{"invalid JSON"});
    for (const auto &result : {Json{{"error", "temporary service error"}}, Json{{"status", "unexpected"}}}) {
        REQUIRE_FALSE(remove->isEnabled());
        poll = take("get_edit_result");
        REQUIRE(poll.params.value("ticket").toString() == "edit-1");
        reply(poll, result);
    }
    poll = take("get_edit_result");
    QElapsedTimer interval;
    interval.start();
    reply(poll, {{"status", "pending"}});
    REQUIRE_FALSE(hasPoll());
    poll = take("get_edit_result");
    REQUIRE(interval.elapsed() >= 15);
    REQUIRE(poll.params.value("ticket").toString() == "edit-1");
    interval.restart();
    reply(poll, {{"status", "running"}});
    while (interval.elapsed() < 150) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    REQUIRE_FALSE(hasPoll()); // The dispatch clock must not poll at 100 ms.
    poll = take("get_edit_result");
    REQUIRE(interval.elapsed() >= 400);
    REQUIRE(poll.params.value("ticket").toString() == "edit-1");
    reply(poll, commit());
    REQUIRE(publications == 1);
    REQUIRE(hub->current() == std::string(64, 'b'));
    context["source_digest"]["hex"] = std::string(64, 'b');
    reply(take("get_graph_instances"), context);
    REQUIRE(remove->isEnabled());
    reply(poll, commit()); // A delayed duplicate cannot publish again.
    REQUIRE(publications == 1);
    REQUIRE_FALSE(hasPoll());
}

TEST_CASE("RenderEditTicket unconfirmed retry stops after abandon and engine loss", "[studio][render-edit-ticket]") {
    Harness h;
    h.accept();
    h.edit.fail(h.take("get_edit_result"), "offline");
    REQUIRE(h.timers.size() == 1);
    h.edit.abandon(); // The Qt adapter and Attachment do this on engine loss.
    h.tick(500);
    REQUIRE(h.calls.empty());
    REQUIRE_FALSE(h.edit.busy());
}

TEST_CASE("DL-2 unrelated RPC bytes bypass parsing and owned replies update state", "[devstudio][automation][dl1]") {
    ensureApplication();
    using namespace PelicanStudio;
    EmbeddedViewport viewport;
    QString error;
    REQUIRE(viewport.process_.start(
        {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}}, &error));
    REQUIRE(viewport.process_.waitForStarted(5000));
    viewport.rpc_ready_ = true;
    struct Call { qint64 id; QString method; QJsonObject params; };
    std::deque<Call> calls;
    viewport.setRpcObserver({{}, [&](qint64 id, const QString &method, const QJsonObject &params) {
        REQUIRE(id != 0);
        calls.push_back({id, method, params});
    }});
    const auto take = [&](const char *method) {
        const auto found = std::find_if(calls.begin(), calls.end(), [&](const Call &call) {
            return call.method == QString::fromLatin1(method);
        });
        REQUIRE(found != calls.end());
        auto call = *found;
        calls.erase(found);
        return call;
    };
    QObject context;
    RenderEditTicketClient client{viewport, &context};
    auto hub = std::make_shared<RenderConfigDigest>();
    client.setRenderConfigDigest(hub);
    std::vector<std::string> committed_digests;
    hub->subscribeCommitted(&context, [&](const auto &digest) { committed_digests.push_back(digest); });
    int completions = 0;
    client.completed = [&](const auto &outcome) {
        CHECK(outcome.kind == Kind::Committed);
        CHECK(outcome.result.at("published_generation").get<std::uint64_t>() == 9007199254740993ULL);
        ++completions;
    };
    RayLayoutWidget ray{&viewport};
    ProjectMigrationWidget migration{[&](const QString &method, const QJsonObject &params, QString *message) {
        return viewport.requestRpc(method, params, message);
    }};
    int forwarded = 0, parsed = 0;
    const QByteArray bytes = "{\"unowned\": [1,2,3]}";
    client.unhandledResult = [&](qint64 id, const QByteArray &raw) {
        if (id == 71) {
            CHECK(raw == bytes);
            ++forwarded;
        }
    };
    const auto previous_sink = GuiTrace::sink;
    void *previous_owner = GuiTrace::owner;
    const auto restore = qScopeGuard([&] { GuiTrace::sink = previous_sink; GuiTrace::owner = previous_owner; });
    GuiTrace::owner = &parsed;
    GuiTrace::sink = [](void *owner, const char *stage, int64_t, int64_t, double, double) {
        if (QString::fromLatin1(stage) == "ticket_client_json_parse" || QString::fromLatin1(stage) == "ray_layout_json_parse" || QString::fromLatin1(stage) == "migration_json_parse") ++*static_cast<int *>(owner);
    };
    // Unknown replies must preserve pending ownership as well as skip parsing.
    REQUIRE(client.submit("edit_rendering_keys", Json::object()));
    const auto edit = take("edit_rendering_keys");
    migration.begin("fixture-project", "project.migration_required@1: {}");
    migration.engineReady();
    CHECK(migration.stage() == ProjectMigrationWidget::Stage::reading_plan);
    const auto plan = take("plan_project_migration");
    const auto layout = take("get_ray_layout");
    const auto documents = take("get_render_documents");
    viewport.inspectorRpcSucceeded(71, bytes);
    CHECK_FALSE(migration.acceptRpcResult(71, bytes));
    CHECK(forwarded == 1);
    CHECK(parsed == 0);
    CHECK(client.busy());
    CHECK(migration.stage() == ProjectMigrationWidget::Stage::reading_plan);

    const auto reply = [&](qint64 id, const Json &value) {
        viewport.inspectorRpcSucceeded(id, QByteArray::fromStdString(value.dump()));
    };
    CHECK(committed_digests.empty());
    reply(edit.id, commit());
    CHECK(committed_digests == std::vector<std::string>{std::string(64, 'b')});
    CHECK(completions == 1);
    CHECK(hub->current() == std::string(64, 'b'));
    CHECK_FALSE(client.busy());
    CHECK(parsed == 1);
    Json read_value;
    REQUIRE(client.read("get_graph_instances", Json::object(), [&](const Json &value) { read_value = value; }));
    CHECK(client.busy());
    reply(take("get_graph_instances").id, {{"nodes", Json::array({"owned-node"})}});
    CHECK(read_value == Json{{"nodes", Json::array({"owned-node"})}});
    CHECK_FALSE(client.busy());
    CHECK(parsed == 2);

    reply(layout.id, {{"source_digest", {{"hex", std::string(64, 'a')}}}, {"mode", "layout"},
        {"layout", {{"reference", "project://passes/ray_layout.json"}, {"digest", std::string(64, 'a')},
                    {"document", defaultRayLayout()}}},
        {"bindings", nullptr}, {"selectable_materials", Json::array()}, {"selectable_procedural_assets", Json::array()}});
    reply(documents.id, {{"documents", Json::array()}});
    auto *name = ray.findChild<QLineEdit *>("pelican.rayLayout.group.default.name");
    REQUIRE(name != nullptr);
    CHECK(name->text() == "default");
    CHECK(name->isEnabled());
    CHECK(parsed == 4);

    const QByteArray plan_bytes = R"({"required":true,"engine_version":"owned-engine","saved_by_engine":"old-engine","documents":[{"path":"passes/owned.json","schema":"pelican.pass","from":1,"to":2}],"expected":{"passes/owned.json":"owned-digest"}})";
    REQUIRE(migration.acceptRpcResult(plan.id, plan_bytes));
    CHECK(migration.stage() == ProjectMigrationWidget::Stage::planned);
    auto *tree = migration.findChild<QTreeWidget *>();
    REQUIRE(tree != nullptr);
    REQUIRE(tree->topLevelItemCount() == 1);
    CHECK(tree->topLevelItem(0)->text(0) == "passes/owned.json");
    auto *apply = migration.findChild<QPushButton *>("pelican.projectMigration.migrate");
    REQUIRE(apply != nullptr);
    REQUIRE(apply->isEnabled());
    apply->click();
    const auto apply_call = take("apply_project_migration");
    CHECK(apply_call.params.value("expected").toObject() == QJsonObject{{"passes/owned.json", "owned-digest"}});
    CHECK(migration.stage() == ProjectMigrationWidget::Stage::applying);
    CHECK(parsed == 5);
    CHECK_FALSE(migration.acceptRpcResult(plan.id, plan_bytes));
    CHECK(parsed == 5);
}
