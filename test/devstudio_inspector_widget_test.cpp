// Q-7: the Inspector on real Qt widgets, over the real RPC transport. A child
// process (process_fixture_child rpc-hold) takes every request the panel sends
// and writes it back on stderr; this test answers in the engine's place,
// through the viewport's own reply signal. What the panel does with focus,
// Return and a commit is then Qt's and the panel's own doing.
//
// The window is activated: focus events are delivered only to the active
// window, and the lock Q-7 removes needed one. With the window in the
// background the same run passed, which is why the scenario that found it
// failed only some of the time.

#include "inspectorwidget.hpp"

// EmbeddedViewport is final and has no test seam. As in the frame-plan test,
// this translation unit reaches its process and readiness members so the
// widget runs over the viewport's real transport.
#define private public
#include "embeddedviewport.hpp"
#undef private

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTest>

#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef PELICAN_PROCESS_FIXTURE_PATH
#error "PELICAN_PROCESS_FIXTURE_PATH must name the process fixture child"
#endif

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_inspector_widget_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

// The engine, as far as the Inspector can tell: one object with a transform
// and a flag, previews that open, update and commit, and edits that commit
// on the next poll. Every commit moves the scene revision.
class FakeEngine {
  public:
    explicit FakeEngine(EmbeddedViewport &viewport) : viewport_{viewport} {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
    }

    Json transform{{"pos", Json::array({1.0, 2.0, 3.0})},
                   {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
                   {"scale", Json::array({1.0, 1.0, 1.0})}};
    Json flags{{"enabled", true}};
    std::uint64_t revision = 1;
    int spawns = 0;
    // PM-3b: the Transform the game has given the object (get_components'
    // runtime_json.local_trs); none, and the runtime value is the authored.
    std::optional<Json> game_transform;
    // The operations of the last `edit`.
    Json last_operations = Json::array();
    int transactions = 0;

    // Answer everything the panel has asked so far.
    void pump() {
        QApplication::processEvents();
        while (!pending_.empty()) {
            const Json request = std::move(pending_.front());
            pending_.pop_front();
            const auto id = request.at("id").get<qint64>();
            // The attached-graphs row beside the fields asks for graphs; the
            // fixture project has none to give.
            if (request.at("method").get<std::string>().starts_with("get_graph_")) {
                emit viewport_.inspectorRpcFailed(id, QStringLiteral("no graphs here"));
                QApplication::processEvents();
                continue;
            }
            const Json result = answer(request.at("method").get<std::string>(),
                                       request.value("params", Json::object()));
            emit viewport_.inspectorRpcSucceeded(id, QByteArray::fromStdString(result.dump()));
            QApplication::processEvents();
        }
    }

  private:
    EmbeddedViewport &viewport_;
    QString buffer_;
    std::deque<Json> pending_;
    std::map<std::string, std::string> preview_kind_;
    Json preview_operation_;
    std::map<std::string, Json> edits_;
    int next_ = 1;

    void receive(const QString &text) {
        buffer_ += text;
        for (qsizetype end = buffer_.indexOf(QLatin1Char('\n')); end >= 0;
             end = buffer_.indexOf(QLatin1Char('\n'))) {
            const QString line = buffer_.left(end).trimmed();
            buffer_.remove(0, end + 1);
            const QString prefix = QStringLiteral("rpc-hold-request:");
            if (line.startsWith(prefix))
                pending_.push_back(Json::parse(line.mid(prefix.size()).toStdString()));
        }
    }

    void apply(const Json &operation) {
        const auto op = operation.value("op", std::string{});
        if (op == "spawn") {
            ++spawns;
            return;
        }
        if (op != "set_component_value") return;
        Json &component = operation.at("component_slot") == "transform" ? transform : flags;
        component[Json::json_pointer{operation.at("field_path").get<std::string>()}] =
            operation.at("value");
    }

    Json components() const {
        const auto schema = [](Json fields) {
            return Json{{"state", "available"}, {"fields", std::move(fields)}};
        };
        return {{"scene_revision", revision},
                {"authoring_object_id", 41},
                {"declaration_index", 2},
                {"components",
                 Json::array(
                     {{{"name", "transform"},
                       {"component_index", 0},
                       {"authored_json", transform},
                       {"runtime_json", game_transform
                                            ? Json{{"local_trs", *game_transform},
                                                   {"world_trs", *game_transform}}
                                            : transform},
                       {"editable", true},
                       {"pending", false},
                       {"codec", {{"state", "registered"}}},
                       {"schema", schema(Json::array({{{"name", "pos"}, {"type", "vec3"}},
                                                      {{"name", "rotation"}, {"type", "quat"}},
                                                      {{"name", "scale"}, {"type", "vec3"}}}))}},
                      {{"name", "fixture_flags"},
                       {"component_index", 1},
                       {"authored_json", flags},
                       {"editable", true},
                       {"pending", false},
                       {"codec", {{"state", "registered"}}},
                       {"schema", schema(Json::array({{{"name", "enabled"}, {"type", "bool"}}}))}}})}};
    }

    Json answer(const std::string &method, const Json &params) {
        if (method == "open_editor_session")
            return {{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}};
        if (method == "scene_tree")
            return {{"scene_revision", revision},
                    {"scene_id", "main"},
                    {"objects", Json::array({{{"authoring_object_id", 41}, {"declaration_index", 2}}})}};
        if (method == "get_components") return components();
        if (method == "get_scene_revision")
            return {{"scene_revision", revision}, {"preview_epoch", 0}};
        if (method == "list_assets") return {{"assets", Json::array()}};
        if (method == "open_preview" || method == "update_preview" ||
            method == "commit_preview" || method == "abort_preview") {
            const std::string request_id = "preview-request:" + std::to_string(next_++);
            preview_kind_[request_id] = method;
            if (params.contains("operations")) preview_operation_ = params.at("operations").at(0);
            return {{"status", "accepted"}, {"ticket", "preview:1"}, {"request_id", request_id}};
        }
        if (method == "get_preview_result") {
            const auto kind = preview_kind_.at(params.at("request_id").get<std::string>());
            if (kind == "open_preview") return {{"status", "open"}};
            if (kind == "update_preview") return {{"status", "updated"}};
            if (kind == "abort_preview") return {{"status", "succeeded"}};
            apply(preview_operation_);
            return {{"status", "committed"},
                    {"committed_revision", ++revision},
                    {"transaction_id", "txn-" + std::to_string(++transactions)}};
        }
        if (method == "edit") {
            const std::string ticket = "edit:" + std::to_string(next_++);
            edits_[ticket] = params.at("operations");
            last_operations = params.at("operations");
            return {{"status", "accepted"}, {"ticket", ticket}};
        }
        if (method == "get_edit_result") {
            for (const auto &operation : edits_.at(params.at("ticket").get<std::string>()))
                apply(operation);
            return {{"status", "committed"},
                    {"committed_revision", ++revision},
                    {"operation", "edit"},
                    {"transaction_id", "txn-" + std::to_string(++transactions)}};
        }
        FAIL("the Inspector asked the engine for " << method);
        return {};
    }
};

struct Rig {
    // First: every widget below needs the application.
    QApplication &app = application();
    EmbeddedViewport viewport;
    FakeEngine engine{viewport};
    std::unique_ptr<InspectorWidget> inspector;

    Rig() {
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}},
            &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        viewport.rpc_ready_ = true;
        inspector = std::make_unique<InspectorWidget>(&viewport);
        inspector->resize(480, 720);
        inspector->show();
        inspector->activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(inspector.get()));
        inspector->setSelection(OutlinerObjectKey{.scene_id = "main", .declaration_index = 2},
                                QStringLiteral("Plane"));
        REQUIRE(pumpUntil([&] { return field("pelican.inspectorVector.transform.pos.1") != nullptr; }));
        pumpFor(300);
    }

    ~Rig() { inspector.reset(); }

    void pumpFor(int milliseconds) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            engine.pump();
            QTest::qWait(5);
        }
    }

    bool pumpUntil(const std::function<bool()> &done, int milliseconds = 5000) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < milliseconds) {
            engine.pump();
            if (done()) return true;
            QTest::qWait(5);
        }
        return done();
    }

    QDoubleSpinBox *field(const char *name) const {
        return inspector->findChild<QDoubleSpinBox *>(QString::fromLatin1(name));
    }
    QPushButton *refresh() const {
        return inspector->findChild<QPushButton *>(QStringLiteral("pelican.inspectorRefresh"));
    }
};

} // namespace

TEST_CASE("Q-7 a committed field gives the Inspector back",
          "[devstudio][inspector][q7][busy][negative-contrast]") {
    Rig rig;
    QPointer<QDoubleSpinBox> position_y = rig.field("pelican.inspectorVector.transform.pos.1");
    QPointer<QDoubleSpinBox> scale_x = rig.field("pelican.inspectorVector.transform.scale.0");
    REQUIRE(position_y);
    REQUIRE(scale_x);
    REQUIRE(rig.refresh()->isEnabled());

    // What the scenario's `type` and `key Return` do: focus, select, type,
    // Return. Return commits the live preview; while it commits the other
    // fields are locked.
    position_y->setFocus(Qt::OtherFocusReason);
    position_y->selectAll();
    QTest::keyClicks(position_y, QStringLiteral("-0.5"));
    QTest::keyClick(position_y, Qt::Key_Return);
    REQUIRE(rig.pumpUntil([&] { return rig.engine.transform.at("pos") == Json::array({1.0, -0.5, 3.0}); }));
    rig.pumpFor(500);

    // The commit is in and nothing is left holding the panel. Before Q-7 the
    // lock moved focus to pos.2, that focus began an edit, and the Inspector
    // stayed held: Refresh off, every other field off, Studio unable to close
    // without asking about an edit in progress.
    CHECK_FALSE(rig.inspector->sceneEditPending());
    CHECK(rig.refresh()->isEnabled());
    REQUIRE(scale_x);
    CHECK(scale_x->isEnabled());

    // Decision 3: the fields were not built again, and focus is where it was.
    REQUIRE(position_y);
    CHECK(position_y == rig.field("pelican.inspectorVector.transform.pos.1"));
    CHECK(position_y->hasFocus());
    CHECK(position_y->value() == -0.5);
}

TEST_CASE("Q-7 focus holds nothing, and half-typed text survives a round trip",
          "[devstudio][inspector][q7][in-place][negative-contrast]") {
    Rig rig;
    QPointer<QDoubleSpinBox> position_x = rig.field("pelican.inspectorVector.transform.pos.0");
    QPointer<QDoubleSpinBox> scale_x = rig.field("pelican.inspectorVector.transform.scale.0");
    QPointer<QCheckBox> enabled =
        rig.inspector->findChild<QCheckBox *>(QStringLiteral("pelican.inspectorBoolean"));
    REQUIRE(position_x);
    REQUIRE(scale_x);
    REQUIRE(enabled);

    // A field with focus is not an edit: Refresh stays pressable.
    scale_x->setFocus(Qt::OtherFocusReason);
    rig.pumpFor(200);
    REQUIRE(scale_x->hasFocus());
    CHECK_FALSE(rig.inspector->sceneEditPending());
    CHECK(rig.refresh()->isEnabled());

    // Half typed: "1." is not yet a new value.
    scale_x->selectAll();
    QTest::keyClicks(scale_x, QStringLiteral("1."));
    REQUIRE(scale_x->cleanText() == QStringLiteral("1."));

    // Another field commits (a click that does not take focus) and the
    // engine answers with a new revision: the values change in place.
    enabled->click();
    REQUIRE(rig.pumpUntil([&] { return rig.engine.flags.at("enabled") == false; }));
    REQUIRE(rig.pumpUntil([&] { return !rig.inspector->sceneEditPending(); }));
    rig.pumpFor(300);
    REQUIRE(scale_x);
    CHECK(scale_x == rig.field("pelican.inspectorVector.transform.scale.0"));
    CHECK(scale_x->hasFocus());
    CHECK(scale_x->cleanText() == QStringLiteral("1."));

    // Another client moves the object; Refresh, pressed with the focus still
    // in the half-typed field, brings the new position in beside it.
    rig.engine.transform["pos"] = Json::array({5.0, 2.0, 3.0});
    ++rig.engine.revision;
    REQUIRE(rig.refresh()->isEnabled());
    rig.refresh()->click();
    REQUIRE(rig.pumpUntil([&] { return position_x && position_x->value() == 5.0; }));
    CHECK(position_x == rig.field("pelican.inspectorVector.transform.pos.0"));
    REQUIRE(scale_x);
    CHECK(scale_x->hasFocus());
    CHECK(scale_x->cleanText() == QStringLiteral("1."));
}

TEST_CASE("T-1 a rotation is typed as angles and stored as the quaternion",
          "[devstudio][inspector][t1][euler]") {
    Rig rig;
    QPointer<QDoubleSpinBox> about_y = rig.field("pelican.inspectorEuler.transform.rotation.1");
    QPointer<QDoubleSpinBox> quaternion_y =
        rig.field("pelican.inspectorVector.transform.rotation.1");
    auto *details = rig.inspector->findChild<QPushButton *>(
        QStringLiteral("pelican.inspectorRotationDetails.transform.rotation"));
    REQUIRE(about_y);
    REQUIRE(quaternion_y);
    REQUIRE(details != nullptr);
    CHECK(rig.field("pelican.inspectorEuler.transform.rotation.3") == nullptr);
    // The quaternion's own fields are folded away until asked for.
    CHECK_FALSE(quaternion_y->isVisible());
    CHECK(about_y->value() == 0.0);

    // Return over angles that still show the stored rotation sends nothing.
    const auto untouched = rig.engine.revision;
    about_y->setFocus(Qt::OtherFocusReason);
    QTest::keyClick(about_y, Qt::Key_Return);
    rig.pumpFor(300);
    CHECK(rig.engine.revision == untouched);

    // 45 about Y, as the scenario types it.
    about_y->selectAll();
    QTest::keyClicks(about_y, QStringLiteral("45"));
    QTest::keyClick(about_y, Qt::Key_Return);
    REQUIRE(rig.pumpUntil([&] {
        const auto &rotation = rig.engine.transform.at("rotation");
        return std::abs(rotation.at(1).get<double>() - 0.38268343236508984) < 1.0e-9;
    }));
    rig.pumpFor(500);
    const auto &rotation = rig.engine.transform.at("rotation");
    CHECK(rotation.at(0).get<double>() == 0.0);
    CHECK(rotation.at(2).get<double>() == 0.0);
    CHECK(std::abs(rotation.at(3).get<double>() - 0.92387953251128674) < 1.0e-9);
    CHECK_FALSE(rig.inspector->sceneEditPending());
    REQUIRE(about_y);
    CHECK(about_y->value() == 45.0);

    // Unfolded, the quaternion shows what was stored; moving focus through
    // the fields sends nothing more.
    details->click();
    rig.pumpFor(100);
    REQUIRE(quaternion_y);
    CHECK(quaternion_y->isVisible());
    CHECK(quaternion_y->text() == QStringLiteral("0.3827"));
    const auto committed = rig.engine.revision;
    quaternion_y->setFocus(Qt::OtherFocusReason);
    rig.pumpFor(100);
    about_y->setFocus(Qt::OtherFocusReason);
    rig.pumpFor(300);
    CHECK(rig.engine.revision == committed);
}


namespace {

// PM-3b: the play session as the window's play controls would report it.
struct PlaySession {
    bool running = true;
    std::set<std::string> kept;
    std::vector<CommittedEdit> committed;

    InspectorWidget::PlaySessionHooks hooks() {
        return {
            .session = [this] { return running; },
            .kept = [this](const std::string &id) { return kept.contains(id); },
            .keep = [this](const std::string &id) { kept.insert(id); },
            .committed = [this](const CommittedEdit &edit) { committed.push_back(edit); },
        };
    }
};

} // namespace

TEST_CASE("PM-3b an edit made while the game runs says it reverts on Stop, and Keep marks it",
          "[devstudio][inspector][pm3b][keep][negative-contrast]") {
    Rig rig;
    PlaySession play;
    rig.inspector->setPlaySessionHooks(play.hooks());
    auto *notice = rig.inspector->findChild<QLabel *>(QStringLiteral("pelican.inspectorNotice"));
    auto *keep = rig.inspector->findChild<QPushButton *>(QStringLiteral("pelican.inspectorKeep"));
    QPointer<QCheckBox> enabled =
        rig.inspector->findChild<QCheckBox *>(QStringLiteral("pelican.inspectorBoolean"));
    REQUIRE(notice != nullptr);
    REQUIRE(keep != nullptr);
    REQUIRE(enabled);

    enabled->click();
    REQUIRE(rig.pumpUntil([&] { return rig.engine.flags.at("enabled") == false; }));
    REQUIRE(rig.pumpUntil([&] { return notice->toolTip().contains(QStringLiteral("committed")); }));
    // What the panel committed was told, by its transaction and its name.
    REQUIRE(play.committed.size() == 1);
    CHECK(play.committed.front().transaction_id == "txn-1");
    CHECK(play.committed.front().label == "Edit fixture_flags.enabled");
    CHECK(play.committed.front().kind == CommittedEditKind::edit);
    CHECK_FALSE(play.committed.front().keep);
    CHECK(notice->toolTip().endsWith(QStringLiteral(" — reverts on Stop")));
    CHECK(notice->text().endsWith(QStringLiteral(" — reverts on Stop")));
    CHECK(keep->isVisible());

    keep->click();
    CHECK(play.kept.contains("txn-1"));
    CHECK(notice->toolTip().endsWith(QStringLiteral(" — kept after Stop")));
    CHECK(notice->text().endsWith(QStringLiteral(" — kept after Stop")));
    CHECK_FALSE(keep->isVisible());

    // Outside a run an edit says nothing of Stop.
    play.running = false;
    rig.inspector->playSessionChanged();
    CHECK_FALSE(notice->toolTip().contains(QStringLiteral("Stop")));
    CHECK_FALSE(notice->text().contains(QStringLiteral("Stop")));
    CHECK_FALSE(keep->isVisible());
}

TEST_CASE("PM-3b Take from game writes the game's Transform back as one edit marked Keep",
          "[devstudio][inspector][pm3b][take][negative-contrast]") {
    Rig rig;
    PlaySession play;
    play.running = false;
    rig.inspector->setPlaySessionHooks(play.hooks());
    rig.pumpFor(100);
    const auto take = [&] {
        return rig.inspector->findChild<QPushButton *>(
            QStringLiteral("pelican.inspectorTakeFromGame"));
    };
    REQUIRE(take() != nullptr);
    // Only while the game runs.
    CHECK_FALSE(take()->isVisible());
    play.running = true;
    rig.inspector->playSessionChanged();
    CHECK(take()->isVisible());
    REQUIRE(take()->isEnabled());

    rig.engine.game_transform = Json{{"pos", Json::array({4.25, 2.0, 3.0})},
                                     {"rotation", Json::array({0.0, 0.0, 0.0, 1.0})},
                                     {"scale", Json::array({1.0, 1.0, 2.0})}};
    take()->click();
    REQUIRE(rig.pumpUntil(
        [&] { return rig.engine.transform.at("pos") == Json::array({4.25, 2.0, 3.0}); }));
    CHECK(rig.engine.transform.at("scale") == Json::array({1.0, 1.0, 2.0}));
    // One edit, the three fields of the authored Transform.
    REQUIRE(rig.engine.last_operations.size() == 3);
    for (const auto &operation : rig.engine.last_operations) {
        CHECK(operation.at("op") == "set_component_value");
        CHECK(operation.at("component_slot") == "transform");
        CHECK(operation.at("object_id") == 41);
    }
    CHECK(rig.engine.last_operations.at(0).at("field_path") == "/pos");
    CHECK(rig.engine.last_operations.at(0).at("value") == Json::array({4.25, 2.0, 3.0}));
    REQUIRE(rig.pumpUntil([&] { return !play.committed.empty(); }));
    CHECK(play.committed.back().label == "Take Transform from game");
    CHECK(play.committed.back().keep);
}

} // namespace PelicanStudio
