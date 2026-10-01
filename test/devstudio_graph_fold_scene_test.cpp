// GL-3a on the Graph tab itself: Fold into group, the group's frame and the
// words on its heading, its Decisions, folding it as it is looked at, and
// Unfold -- each through the capability the tab is given, as the engine
// answers.
#include "graphcanvasfoldgroups.hpp"
#include "graphcanvaswidget.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGraphicsEllipseItem>
#include <QGraphicsItem>
#include <QGraphicsSimpleTextItem>
#include <QThread>
#include <QGraphicsSceneMouseEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QString>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;
using namespace PelicanStudio;

QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_graph_fold_scene_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

struct Transport {
    struct Call {
        QString method;
        Json params;
    };
    std::vector<Call> calls;
    std::set<qint64> answered;
    std::function<void(qint64, QByteArray)> deliver;

    qint64 note(const QString &method, Json params = Json::object()) {
        calls.push_back({method, std::move(params)});
        return static_cast<qint64>(calls.size());
    }
    GraphCanvasReadCapability capability() {
        GraphCanvasReadCapability seam;
        seam.ready = [](QObject *, auto handler) { handler(true, {}); };
        seam.requestGraphInstances = [this](QString *) {
            return note(QStringLiteral("get_graph_instances"));
        };
        seam.requestGraphCatalog = [this](QString *) {
            return note(QStringLiteral("get_graph_catalog"));
        };
        seam.requestFramePlan = [this](QString *) { return note(QStringLiteral("get_frame_plan")); };
        seam.requestEditResult = [this](const QString &, QString *) {
            return note(QStringLiteral("get_edit_result"));
        };
        seam.requestFoldGroups = [this](QString *) { return note(QStringLiteral("get_fold_groups")); };
        seam.requestRootDocument = [this](const QString &reference, QString *) {
            return note(QStringLiteral("get_render_documents"),
                        Json{{"references", Json::array({reference.toStdString()})}});
        };
        seam.requestFoldNodes = [this](const QJsonObject &params, QString *) {
            return note(QStringLiteral("fold_nodes"),
                        Json::parse(QJsonDocument(params).toJson().toStdString()));
        };
        seam.result = [this](QObject *, auto handler) { deliver = handler; };
        seam.failure = [](QObject *, auto) {};
        return seam;
    }
    // The newest call of `method` nobody has answered yet; 0 for none.
    qint64 pending(const QString &method) const {
        for (auto index = static_cast<qint64>(calls.size()); index >= 1; --index)
            if (calls[static_cast<std::size_t>(index - 1)].method == method && !answered.contains(index))
                return index;
        return 0;
    }
    const Json &last(const QString &method) const {
        for (auto it = calls.rbegin(); it != calls.rend(); ++it)
            if (it->method == method) return it->params;
        static const Json none;
        return none;
    }
    int count(const QString &method) const {
        return static_cast<int>(std::ranges::count(calls, method, &Call::method));
    }
    bool answer(const QString &method, const Json &reply) {
        const auto id = pending(method);
        if (id == 0) return false;
        answered.insert(id);
        deliver(id, QByteArray::fromStdString(reply.dump()));
        QCoreApplication::processEvents();
        return true;
    }
};

// Five value nodes: two inputs, a product and two sums that read it.
Json document(const std::string &digest, const Json &fold_groups = Json::object()) {
    const auto input = [](const char *id, const char *name) {
        return Json{{"id", id}, {"op", "value.input"}, {"parameters", {{"name", name}}},
                    {"outputs", Json::array({"value"})}};
    };
    const auto pair = [](const char *id, const char *op, const char *a, const char *b) {
        return Json{{"id", id}, {"op", op},
                    {"inputs", {{"a", {{"node", a}, {"port", "value"}}},
                                {"b", {{"node", b}, {"port", "value"}}}}},
                    {"outputs", Json::array({"value"})}};
    };
    Json root{{"schema", "pelican.graph"}, {"version", 1}, {"definitions", Json::object()},
              {"outputs", Json::object()},
              {"nodes", Json::array({input("src_a", "scene_time"), input("src_b", "delta_time"),
                                     pair("mid", "value.multiply", "src_a", "src_b"),
                                     pair("gate_a", "value.add", "src_a", "mid"),
                                     pair("gate_b", "value.add", "src_b", "mid")})}};
    if (!fold_groups.empty()) root["fold_groups"] = fold_groups;
    return root;
}

Json context(const Json &root, const std::string &digest) {
    return {{"source_reference", "passes/main.json"},
            {"root_schema", "pelican.graph"},
            {"source_digest", {{"hex", digest}}},
            {"instances", Json::array()},
            {"nodes", root.at("nodes")},
            {"definitions", root.at("definitions")},
            {"layout", Json::object()}};
}

QGraphicsItem *itemOf(QGraphicsScene &scene, const char *kind, const QString &name) {
    for (QGraphicsItem *item : scene.items())
        if (item->data(NodeGraphItemKindRole).toString() == QLatin1String{kind} &&
            item->data(NodeGraphNameRole).toString() == name)
            return item;
    return nullptr;
}

void mouse(QGraphicsScene &scene, const QPointF &at, QEvent::Type type) {
    QGraphicsSceneMouseEvent event{type};
    event.setScenePos(at);
    event.setButton(Qt::LeftButton);
    event.setButtons(Qt::LeftButton);
    QApplication::sendEvent(&scene, &event);
    if (type == QEvent::GraphicsSceneMousePress) {
        QGraphicsSceneMouseEvent release{QEvent::GraphicsSceneMouseRelease};
        release.setScenePos(at);
        release.setButton(Qt::LeftButton);
        QApplication::sendEvent(&scene, &release);
    }
    QCoreApplication::processEvents();
}

} // namespace

TEST_CASE("GL-3a the Graph tab folds picked root nodes into a group, says it is not "
          "verified, writes its decisions, folds it as it is looked at, and unfolds it",
          "[devstudio][graph-canvas][graph-fold][gl3a]") {
    (void)application();
    Transport transport;
    GraphCanvasWidget widget{transport.capability()};
    widget.resize(1200, 700);
    widget.show();
    QCoreApplication::processEvents();
    auto *values = widget.findChild<QCheckBox *>(QStringLiteral("pelican.graphCanvasShow.value"));
    REQUIRE(values != nullptr);
    values->setChecked(true);
    REQUIRE(transport.answer(QStringLiteral("get_graph_instances"), context(document("d1"), "d1")));
    // The groups are read with the document; there are none yet.
    REQUIRE(transport.answer(QStringLiteral("get_fold_groups"),
                             Json{{"source_digest", {{"hex", "d1"}}}, {"fold_groups", Json::array()}}));
    auto &scene = widget.scene();
    REQUIRE(itemOf(scene, GraphCanvasNodeItem, QStringLiteral("mid")) != nullptr);

    // Decision 1: two root nodes picked, Fold into group..., no question.
    itemOf(scene, GraphCanvasNodeItem, QStringLiteral("gate_a"))->setSelected(true);
    itemOf(scene, GraphCanvasNodeItem, QStringLiteral("mid"))->setSelected(true);
    auto *fold = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.foldIntoGroup"));
    REQUIRE(fold != nullptr);
    fold->trigger();
    QCoreApplication::processEvents();
    REQUIRE(transport.count(QStringLiteral("fold_nodes")) == 1);
    CHECK(transport.last(QStringLiteral("fold_nodes")) ==
          Json{{"base_source_digest", "d1"},
               {"group", "fold_1"},
               {"nodes", {"mid", "gate_a"}},
               {"folded", {{"kind", "decisions"}, {"decisions", Json::object()}}}});
    REQUIRE(transport.answer(QStringLiteral("fold_nodes"), Json{{"status", "accepted"}, {"ticket", "t1"}}));
    REQUIRE(transport.answer(QStringLiteral("get_edit_result"),
                             Json{{"status", "committed"}, {"committed", true},
                                  {"source_digest", {{"hex", "d2"}}}}));
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    REQUIRE(notice != nullptr);
    CHECK(notice->text().startsWith(QStringLiteral("Folded mid, gate_a into fold_1")));

    // The engine's answers: the document, the group (unverified -- FG-2 is
    // not there yet), the root's decisions (none).
    const Json group_json{{"mid_gate",
                           {{"nodes", {"mid", "gate_a"}},
                            {"folded", {{"kind", "decisions"}, {"decisions", Json::object()}}}}}};
    const Json groups{{"fold_1", group_json.at("mid_gate")}};
    const auto report = Json{{"source_digest", {{"hex", "d2"}}},
                             {"fold_groups",
                              Json::array({{{"group", "fold_1"}, {"nodes", {"mid", "gate_a"}},
                                            {"kind", "decisions"}, {"digest", std::string(64, 'a')},
                                            {"state", "unverified"}}})}};
    const auto root_document = Json{
        {"documents", Json::array({{{"reference", "passes/main.json"}, {"kind", "root"},
                                    {"digest", "d2"},
                                    {"content", document("d2", groups).dump()}}})}};
    transport.answer(QStringLiteral("get_graph_instances"), context(document("d2", groups), "d2"));
    for (int round = 0; round < 6; ++round) {
        transport.answer(QStringLiteral("get_fold_groups"), report);
        transport.answer(QStringLiteral("get_render_documents"), root_document);
    }
    QCoreApplication::processEvents();

    // Decisions 2 and 3: a frame round the two nodes, and the heading says
    // the decisions are not applied.
    auto *frame = itemOf(scene, GraphCanvasFoldGroupItem, QStringLiteral("fold_1"));
    REQUIRE(frame != nullptr);
    for (const char *id : {"mid", "gate_a"})
        CHECK(frame->sceneBoundingRect().contains(
            itemOf(scene, GraphCanvasNodeItem, QString::fromLatin1(id))->sceneBoundingRect()));
    CHECK_FALSE(frame->sceneBoundingRect().contains(
        itemOf(scene, GraphCanvasNodeItem, QStringLiteral("gate_b"))->sceneBoundingRect()));
    auto *state = dynamic_cast<QGraphicsSimpleTextItem *>(
        itemOf(scene, GraphCanvasFoldStateItem, QStringLiteral("fold_1")));
    REQUIRE(state != nullptr);
    CHECK(state->text().toStdString() == std::string{graphFoldUnverifiedLine});

    // Decision 4: the group just made is picked; its Decisions say they are
    // requests, and Isolate is added by choosing.
    auto *note = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasFold.note"));
    REQUIRE(note != nullptr);
    CHECK(note->text().toStdString() == std::string{graphFoldDecisionsNote});
    auto *node = widget.findChild<QComboBox *>(QStringLiteral("pelican.graphCanvasFold.isolate.node"));
    auto *add = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasFold.isolate.add"));
    REQUIRE(node != nullptr);
    REQUIRE(add != nullptr);
    CHECK(node->count() == 2);
    node->setCurrentIndex(node->findText(QStringLiteral("gate_a")));
    add->click();
    QCoreApplication::processEvents();
    REQUIRE(transport.count(QStringLiteral("fold_nodes")) == 2);
    CHECK(transport.last(QStringLiteral("fold_nodes")).at("folded") ==
          Json{{"kind", "decisions"},
               {"decisions", {{"isolate", Json::array({{{"node", "gate_a"}}})}}}});
    REQUIRE(transport.answer(QStringLiteral("fold_nodes"), Json{{"status", "accepted"}, {"ticket", "t2"}}));
    REQUIRE(transport.answer(QStringLiteral("get_edit_result"),
                             Json{{"status", "committed"}, {"committed", true}}));

    // Decision 2: Fold draws one folded node with the lines that cross the
    // group's edge as its rows; the nodes are hidden, the document untouched.
    const int writes_before = transport.count(QStringLiteral("fold_nodes"));
    auto *toggle = itemOf(scene, GraphCanvasFoldToggleItem, QStringLiteral("fold_1"));
    REQUIRE(toggle != nullptr);
    mouse(scene, toggle->sceneBoundingRect().center(), QEvent::GraphicsSceneMousePress);
    auto *folded = itemOf(scene, GraphCanvasNodeItem, QStringLiteral("fold:fold_1"));
    REQUIRE(folded != nullptr);
    CHECK_FALSE(itemOf(scene, GraphCanvasNodeItem, QStringLiteral("mid"))->isVisible());
    std::set<std::string> rows;
    for (QGraphicsItem *child : folded->childItems())
        if (child->data(NodeGraphItemKindRole).toString() == QLatin1String{GraphCanvasDerivedSocketItem})
            rows.insert(child->data(GraphCanvasPortRole).toString().toStdString());
    CHECK(rows == std::set<std::string>{"mid.a", "mid.b", "gate_a.a", "mid.value"});
    CHECK(transport.count(QStringLiteral("fold_nodes")) == writes_before);
    // Rule 6: a double-click on it goes in -- the nodes are back.
    mouse(scene, folded->sceneBoundingRect().center(), QEvent::GraphicsSceneMouseDoubleClick);
    CHECK(itemOf(scene, GraphCanvasNodeItem, QStringLiteral("fold:fold_1")) == nullptr);
    CHECK(itemOf(scene, GraphCanvasNodeItem, QStringLiteral("mid"))->isVisible());

    // Decision 5: Unfold asks (Cancel is the default), then sends folded null.
    auto *unfold = widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.unfoldGroup"));
    REQUIRE(unfold != nullptr);
    unfold->trigger();
    QCoreApplication::processEvents();
    auto *ask = widget.findChild<QMessageBox *>(QStringLiteral("pelican.graphCanvasFold.unfoldConfirm"));
    REQUIRE(ask != nullptr);
    CHECK(ask->defaultButton()->objectName() ==
          QStringLiteral("pelican.graphCanvasFold.unfoldConfirm.cancel"));
    auto *ok = ask->findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasFold.unfoldConfirm.ok"));
    REQUIRE(ok != nullptr);
    ok->click();
    QCoreApplication::processEvents();
    CHECK(transport.last(QStringLiteral("fold_nodes")) ==
          Json{{"base_source_digest", "d2"}, {"group", "fold_1"}, {"folded", nullptr}});
}

TEST_CASE("GL-4a Verify from a group's heading runs verify_fold, holds the group's edits while "
          "it runs, and marks what it found; a stale group offers Verify again",
          "[devstudio][graph-canvas][graph-fold][gl4a]") {
    (void)application();
    Transport transport;
    auto capability = transport.capability();
    capability.requestVerifyFold = [&transport](const QJsonObject &params, QString *) {
        return transport.note(QStringLiteral("verify_fold"),
                              Json::parse(QJsonDocument(params).toJson().toStdString()));
    };
    GraphCanvasWidget widget{capability};
    widget.resize(1200, 700);
    widget.show();
    QCoreApplication::processEvents();
    widget.findChild<QCheckBox *>(QStringLiteral("pelican.graphCanvasShow.value"))->setChecked(true);
    const Json groups{
        {"fold_1", {{"nodes", {"mid", "gate_a"}}, {"folded", {{"kind", "decisions"}}}}},
        {"fold_2", {{"nodes", {"gate_b"}}, {"folded", {{"kind", "decisions"}}}}}};
    const auto digest = std::string(64, 'd');
    REQUIRE(transport.answer(QStringLiteral("get_graph_instances"),
                             context(document(digest, groups), digest)));
    const auto report = Json{
        {"fold_groups",
         Json::array({{{"group", "fold_1"}, {"nodes", {"mid", "gate_a"}}, {"kind", "decisions"},
                       {"digest", std::string(64, 'a')}, {"state", "unverified"}},
                      {{"group", "fold_2"}, {"nodes", {"gate_b"}}, {"kind", "decisions"},
                       {"digest", std::string(64, 'b')},
                       {"verified", {{"digest", std::string(64, 'c')}, {"at", "t"}}},
                       {"state", "stale"}}})}};
    REQUIRE(transport.answer(QStringLiteral("get_fold_groups"), report));
    transport.answer(QStringLiteral("get_render_documents"),
                     Json{{"documents", Json::array({{{"reference", "passes/main.json"},
                                                      {"content", document(digest, groups).dump()}}})}});
    auto &scene = widget.scene();
    const auto wait_for = [&](const QString &method) {
        for (int round = 0; round < 200 && transport.pending(method) == 0; ++round) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
        return transport.pending(method) != 0;
    };

    // Decision 4: the stale group offers Verify again, and says why it is
    // stale (a loop's Count counts).
    auto *again = itemOf(scene, GraphCanvasFoldVerifyItem, QStringLiteral("fold_2"));
    REQUIRE(again != nullptr);
    CHECK(again->data(GraphCanvasLabelRole).toString() == QStringLiteral("Verify again"));
    CHECK(again->toolTip().contains(QStringLiteral("Count")));

    // Decision 1: Verify on the heading, with Quick check from the fields.
    auto *heading = itemOf(scene, GraphCanvasFoldHeadingItem, QStringLiteral("fold_1"));
    REQUIRE(heading != nullptr);
    mouse(scene, heading->sceneBoundingRect().topLeft() + QPointF{6.0, 6.0},
          QEvent::GraphicsSceneMousePress);
    auto *quick = widget.findChild<QCheckBox *>(QStringLiteral("pelican.graphCanvasFold.quickCheck"));
    REQUIRE(quick != nullptr);
    quick->setChecked(true);
    auto *verify = itemOf(scene, GraphCanvasFoldVerifyItem, QStringLiteral("fold_1"));
    REQUIRE(verify != nullptr);
    CHECK(verify->data(GraphCanvasLabelRole).toString() == QStringLiteral("Verify"));
    mouse(scene, verify->sceneBoundingRect().center(), QEvent::GraphicsSceneMousePress);
    REQUIRE(transport.count(QStringLiteral("verify_fold")) == 1);
    CHECK(transport.last(QStringLiteral("verify_fold")) ==
          Json{{"base_source_digest", digest}, {"group", "fold_1"}, {"frames", 2},
               {"warmup_seconds", 1}});

    // Decision 2: while it runs, the heading counts seconds, the group's
    // fields are grey and its edits wait.
    auto *state = dynamic_cast<QGraphicsSimpleTextItem *>(
        itemOf(scene, GraphCanvasFoldStateItem, QStringLiteral("fold_1")));
    REQUIRE(state != nullptr);
    CHECK(state->text().startsWith(QStringLiteral("verifying\xE2\x80\xA6 (")));
    CHECK(itemOf(scene, GraphCanvasFoldVerifyItem, QStringLiteral("fold_1")) == nullptr);
    auto *add = widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasFold.isolate.add"));
    REQUIRE(add != nullptr);
    CHECK_FALSE(add->isEnabled());
    itemOf(scene, GraphCanvasNodeItem, QStringLiteral("src_a"))->setSelected(true);
    widget.findChild<QAction *>(QStringLiteral("pelican.graphCanvas.foldAddSelected"))->trigger();
    QCoreApplication::processEvents();
    auto *notice = widget.findChild<QLabel *>(QStringLiteral("pelican.graphCanvasNotice"));
    CHECK(notice->text() == QStringLiteral("fold_1 is being verified; edits wait until it finishes"));
    CHECK(transport.count(QStringLiteral("fold_nodes")) == 0);

    REQUIRE(transport.answer(QStringLiteral("verify_fold"),
                             Json{{"ticket", "v1"}, {"status", "accepted"}}));
    REQUIRE(wait_for(QStringLiteral("get_edit_result")));
    REQUIRE(transport.answer(QStringLiteral("get_edit_result"),
                             Json{{"status", "running"}, {"operation", "verify_fold"}, {"group", "fold_1"}}));
    REQUIRE(wait_for(QStringLiteral("get_edit_result")));
    // Decision 3: the decided form drew differently -- a red mark and one
    // sentence on the heading; the decisions still do not apply.
    REQUIRE(transport.answer(
        QStringLiteral("get_edit_result"),
        Json{{"status", "rejected"},
             {"committed", false},
             {"error", {{"code", "graph.fold_mismatch@1"}, {"message", "differs"}, {"group", "fold_1"},
                        {"what", "pixels"}, {"detail", {{"frame", 2}, {"differing_pixels", 14}}}}}}));
    state = dynamic_cast<QGraphicsSimpleTextItem *>(
        itemOf(scene, GraphCanvasFoldStateItem, QStringLiteral("fold_1")));
    REQUIRE(state != nullptr);
    CHECK(state->text() == QStringLiteral("frame 2 differs in 14 pixels"));
    auto *mark = dynamic_cast<QGraphicsEllipseItem *>(
        itemOf(scene, GraphCanvasFoldMarkItem, QStringLiteral("fold_1")));
    REQUIRE(mark != nullptr);
    CHECK(mark->brush().color() == QColor(0xd9, 0x4c, 0x3d));
    CHECK(notice->text().startsWith(QStringLiteral("fold_1 is not verified: frame 2 differs")));
    // Verify is offered again, and the fields take edits again.
    CHECK(itemOf(scene, GraphCanvasFoldVerifyItem, QStringLiteral("fold_1")) != nullptr);
    CHECK(widget.findChild<QPushButton *>(QStringLiteral("pelican.graphCanvasFold.isolate.add"))
              ->isEnabled());
}
