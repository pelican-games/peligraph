// PM-3a: Pause / Step / Resume and the play line, on the real window over the
// real RPC transport. A child process (process_fixture_child rpc-hold) takes
// every request the window sends and writes it back on stderr; this test
// answers in the engine's place through the transport's own signals, so the
// viewport's routing (the get_status reads it hands on, the refusals it
// passes whole) is the production one. The fake engine keeps a play state the
// way PM-1a does: a game frame apart from the evaluation number, steps taken
// one per frame, advance_frames refused unless paused.
//
// What is judged is what a person sees and what the engine is sent.

// Everything the two headers below include, first, so that only their own
// classes are opened up (as in the Outliner edit test).
#include "layoutpreset.hpp"
#include "toollayoutpreset.hpp"
#include "../src/devstudio/model/gizmomodel.hpp"
#include "../src/devstudio/model/inspectormodel.hpp"
#include "../src/devstudio/model/playstatemodel.hpp"
#include "../src/devstudio/model/project.hpp"
#include "../src/devstudio/model/selection.hpp"
#include "../src/devstudio/viewport/engineattach.hpp"
#include "../src/devstudio/viewport/enginefailuremodel.hpp"
#include "../src/devstudio/viewport/enginelogbuffer.hpp"
#include "../src/devstudio/viewport/engineprocess.hpp"
#include "../src/devstudio/viewport/nativewindowhost.hpp"
#include "../src/devstudio/viewport/viewportgeometry.hpp"
#include "graphcanvaswidget.hpp"
#include "outlinertree.hpp"
#include "playcontrols.hpp"
#include "studiotheme.hpp"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QElapsedTimer>
#include <QFrame>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QStatusBar>
#include <QStringList>
#include <QTest>
#include <QToolBar>
#include <QWidget>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define private public
#include "../src/devstudio/viewport/embeddedviewport.hpp"
#include "mainwindow.hpp"
#undef private

#ifndef PELICAN_PROCESS_FIXTURE_PATH
#error "PELICAN_PROCESS_FIXTURE_PATH must name the process fixture child"
#endif

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_play_controls_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

// PM-1a as far as Studio can tell. Every pump is one engine frame: the
// evaluation number always rises, the game frame only while playing or for a
// step that was asked for.
class FakeEngine {
  public:
    explicit FakeEngine(EmbeddedViewport &viewport) : viewport_{viewport} {
        QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                         [this](const QString &text) { receive(text); });
    }

    bool report_play = true;
    std::string state = "playing";
    std::uint64_t evaluation = 5000;
    std::uint64_t game_frame = 100;
    std::uint64_t pending_steps = 0;
    // Hold the answers to set_play_state / advance_frames until released.
    bool hold = false;
    // The next play request is refused with this data.
    std::optional<Json> refuse_next;
    // set_play_state and advance_frames, in the order they were sent.
    std::vector<Json> play_requests;
    int status_reads = 0;
    // What the last get_status said the game frame was.
    std::uint64_t reported_game_frame = 0;

    void pump() {
        QApplication::processEvents();
        while (!pending_.empty()) {
            const Json request = std::move(pending_.front());
            pending_.pop_front();
            answer(request);
            QApplication::processEvents();
        }
        frame();
    }

    void release() {
        hold = false;
        auto held = std::move(held_);
        held_.clear();
        for (const auto &request : held) answer(request, false);
        QApplication::processEvents();
    }

  private:
    EmbeddedViewport &viewport_;
    QString buffer_;
    std::deque<Json> pending_;
    std::vector<Json> held_;

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

    void frame() {
        ++evaluation;
        if (state == "playing") {
            ++game_frame;
        } else if (pending_steps > 0) {
            --pending_steps;
            ++game_frame;
        }
    }

    void result(qint64 id, const Json &value) {
        emit viewport_.process_.rpcResultReceived(id, QByteArray::fromStdString(value.dump()));
    }

    void error(qint64 id, int code, const std::string &message, const Json &data) {
        emit viewport_.process_.rpcErrorReceived(
            id, code, QString::fromStdString(message),
            QJsonValue{QJsonDocument::fromJson(QByteArray::fromStdString(data.dump())).object()});
    }

    void answer(const Json &request, bool first = true) {
        const auto id = request.at("id").get<qint64>();
        const auto method = request.at("method").get<std::string>();
        const Json params = request.value("params", Json::object());
        if (method == "set_play_state" || method == "advance_frames") {
            if (first) play_requests.push_back({{"method", method}, {"params", params}});
            if (hold) {
                held_.push_back(request);
                return;
            }
            if (refuse_next) {
                error(id, -32000, "refused", *std::exchange(refuse_next, std::nullopt));
                return;
            }
        }
        if (method == "set_play_state") {
            const auto previous = state;
            state = params.at("state").get<std::string>();
            if (state == "playing") pending_steps = 0;
            result(id, {{"state", state}, {"previous", previous}});
            return;
        }
        if (method == "advance_frames") {
            if (state != "paused") {
                error(id, -32000, "advance_frames steps a paused game",
                      {{"code", "play_state_mismatch"}, {"state", state}, {"required", "paused"}});
                return;
            }
            const auto count = params.at("count").get<std::uint64_t>();
            pending_steps += count;
            result(id, {{"scheduled", count}, {"until_game_frame", game_frame + pending_steps}});
            return;
        }
        if (method == "get_status") {
            ++status_reads;
            Json status{{"frame", evaluation}, {"time", 1.5}};
            if (report_play) {
                status["play"] = {{"state", state},
                                  {"game_frame", game_frame},
                                  {"game_time", 1.5},
                                  {"pending_steps", pending_steps}};
                reported_game_frame = game_frame;
            }
            result(id, status);
            return;
        }
        if (method == "open_editor_session") {
            result(id, {{"actor_id", 9}, {"display_name", "Pelican Studio Inspector"}});
            return;
        }
        if (method == "scene_tree") {
            result(id, {{"scene_revision", 1}, {"scene_id", "main"}, {"objects", Json::array()}});
            return;
        }
        if (method == "get_scene_revision") {
            result(id, {{"scene_revision", 1}, {"preview_epoch", 0}});
            return;
        }
        error(id, -32601, "not in this fake engine", Json::object());
    }

};

struct ProjectSandbox {
    std::filesystem::path root;

    ProjectSandbox() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        root = std::filesystem::temp_directory_path() / ("pelican_play_controls_" + suffix);
        std::filesystem::create_directories(root / "scenes");
        std::filesystem::create_directories(root / "config");
        write("project.json", R"json({
  "schema":"pelican.project","version":1,"name":"pm3a",
  "basic_config":{"scene_data_json":"scenes/main.scene.json"}
})json");
        write("scenes/main.scene.json", R"json({
  "schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}
})json");
    }

    ~ProjectSandbox() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write(const char *relative, const char *text) {
        std::ofstream stream(root / relative, std::ios::binary);
        stream << text;
    }
};

struct Rig {
    QApplication &app = application();
    ProjectSandbox sandbox;
    MainWindow window{QString::fromStdWString((sandbox.root / "config").wstring())};
    EmbeddedViewport &viewport = *window.viewport_;
    FakeEngine engine{viewport};
    PlayControls &controls = *window.play_controls_;

    explicit Rig(bool report_play = true) {
        engine.report_play = report_play;
        QString error;
        REQUIRE(viewport.process_.start(
            {QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH), {QStringLiteral("rpc-hold")}, {}},
            &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        window.project_model_ = ProjectOutlinerModel::open(sandbox.root);
        window.selection_model_.bindProject(&*window.project_model_);
        window.resize(1200, 800);
        window.show();
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        viewport.rpc_ready_ = true;
        emit viewport.engineRpcBecameAvailable();
        pumpFor(100);
    }

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

    // Ctrl+P with `modifiers`, the keyboard in `widget`.
    void press(QWidget &widget, Qt::KeyboardModifiers modifiers) {
        widget.activateWindow();
        widget.setFocus(Qt::OtherFocusReason);
        REQUIRE(pumpUntil([&] { return QApplication::focusWidget() == &widget; }, 2000));
        QTest::keyClick(&widget, Qt::Key_P, modifiers);
    }

    QAction &pause() const { return *controls.pauseAction(); }
    QAction &step() const { return *controls.stepAction(); }
    QString line() const { return controls.statusLabel()->text(); }
    std::size_t sent() const { return engine.play_requests.size(); }
    const Json &last() const { return engine.play_requests.back(); }

    // The margin between the viewport's frame and the viewport.
    QColor frameColour() const {
        const QImage picture = window.viewport_frame_->grab().toImage();
        return picture.pixelColor(2, picture.height() / 2);
    }
    QString bandStyle() const { return window.gizmo_toolbar_->styleSheet(); }
    GraphCanvasModel &graph() const { return window.graph_canvas_->model(); }
};

const QString &middleDot() {
    static const QString dot = QString::fromUtf8("\xC2\xB7");
    return dot;
}

QString shown(const char *state, std::uint64_t frame) {
    return QStringLiteral("%1 %2 frame %3").arg(QString::fromUtf8(state), middleDot()).arg(frame);
}

} // namespace

TEST_CASE("PM-3a get_status.play is read as the game frame, not the evaluation number",
          "[devstudio][play][pm3a]") {
    // The model alone: the line is built from play.game_frame.
    PlayStateModel model;
    model.connect();
    auto requests = model.takeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests.front().method == "get_status");
    model.receiveResult(requests.front().request_id,
                        Json{{"frame", 9000},
                             {"time", 2.0},
                             {"play",
                              {{"state", "paused"},
                               {"game_frame", 42},
                               {"game_time", 0.7},
                               {"pending_steps", 0}}}});
    CHECK(model.supported());
    CHECK(model.statusText() == "Paused · frame 42");
    CHECK(model.statusText().find("9000") == std::string::npos);

    // Steps still to take read as such.
    CHECK(model.observeStatus(Json{{"frame", 9001},
                                   {"play",
                                    {{"state", "paused"},
                                     {"game_frame", 42},
                                     {"game_time", 0.7},
                                     {"pending_steps", 3}}}}));
    CHECK(model.statusText() == "Stepping (3 left) · frame 42");

    // An engine from before PM-1a: no play, nothing to show.
    PlayStateModel old;
    old.connect();
    const auto read = old.takeRequests();
    REQUIRE(read.size() == 1);
    old.receiveResult(read.front().request_id, Json{{"frame", 12}, {"time", 0.2}});
    CHECK_FALSE(old.supported());
    CHECK(old.statusText().empty());
    CHECK_FALSE(old.togglePause());
    CHECK(old.takeRequests().empty());
}

TEST_CASE("PM-3a the play refusals read as one line from Studio's table",
          "[devstudio][play][pm3a]") {
    CHECK(playRefusalDescription(Json{{"code", "play_state_mismatch"},
                                      {"state", "playing"},
                                      {"required", "paused"}}) ==
          "The game is playing; this works only while it is paused (play_state_mismatch).");
    CHECK(playRefusalDescription(Json{{"code", "play_state_mismatch"},
                                      {"state", "playing"},
                                      {"requested", "edit"}}) ==
          "This engine has no edit state yet; the game stays playing (play_state_mismatch).");
    CHECK(playRefusalDescription(Json{{"code", "step_frame_windowed"}}) ==
          "step_frame steps a headless engine only; pause the game and use Step "
          "(step_frame_windowed).");
    // Studio's one lookup answers them too.
    CHECK(knownRefusalDescription("step_frame_windowed", "") ==
          "step_frame steps a headless engine only; pause the game and use Step "
          "(step_frame_windowed).");
    CHECK(playRefusalDescription(Json{{"code", "graph.definition_missing@1"}}).empty());
}

TEST_CASE("PM-3a Pause sends paused, pressing it again sends playing, and the line, the "
          "frame and the band follow",
          "[devstudio][play][pm3a][negative-contrast]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.pause().isVisible() && !rig.line().isEmpty(); }));
    CHECK(rig.line() == shown("Playing", rig.engine.reported_game_frame));
    CHECK_FALSE(rig.pause().isChecked());
    CHECK(rig.pause().isEnabled());
    CHECK_FALSE(rig.step().isEnabled());
    const QColor paused_frame = currentStudioTheme().play.paused_frame;
    CHECK(rig.frameColour() != paused_frame);
    CHECK(rig.bandStyle().isEmpty());

    rig.pause().trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Paused")); }));
    REQUIRE(rig.sent() == 1);
    CHECK(rig.last() == Json{{"method", "set_play_state"}, {"params", {{"state", "paused"}}}});
    // The frame the game stopped on, read after the pause -- not the
    // evaluation number, which went on rising.
    CHECK(rig.line() == shown("Paused", rig.engine.game_frame));
    CHECK_FALSE(rig.line().contains(QString::number(rig.engine.evaluation)));
    CHECK(rig.pause().isChecked());
    CHECK(rig.step().isEnabled());
    CHECK(rig.frameColour() == paused_frame);
    CHECK(rig.bandStyle().contains(currentStudioTheme().play.paused_band.name()));

    // Released: Resume.
    rig.pause().trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
    REQUIRE(rig.sent() == 2);
    CHECK(rig.last() == Json{{"method", "set_play_state"}, {"params", {{"state", "playing"}}}});
    CHECK_FALSE(rig.pause().isChecked());
    CHECK_FALSE(rig.step().isEnabled());
    CHECK(rig.frameColour() != paused_frame);
    CHECK(rig.bandStyle().isEmpty());
}

TEST_CASE("PM-3a Step sends advance_frames count 1, and only while paused",
          "[devstudio][play][pm3a][negative-contrast]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.pause().isVisible(); }));

    // Playing: neither the button nor its key sends anything.
    CHECK_FALSE(rig.step().isEnabled());
    rig.step().trigger();
    rig.press(*rig.window.outliner_, Qt::ControlModifier | Qt::AltModifier);
    rig.pumpFor(100);
    CHECK(rig.sent() == 0);

    rig.pause().trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Paused")); }));
    const auto stopped = rig.engine.game_frame;

    // One step, held so the line can be seen waiting for it.
    rig.engine.hold = true;
    rig.step().trigger();
    rig.pumpFor(30);
    REQUIRE(rig.sent() == 2);
    CHECK(rig.last() == Json{{"method", "advance_frames"}, {"params", {{"count", 1}}}});
    CHECK_FALSE(rig.step().isEnabled());
    rig.engine.release();
    REQUIRE(rig.pumpUntil([&] { return rig.line() == shown("Paused", stopped + 1); }));
    CHECK(rig.engine.game_frame == stopped + 1);
    CHECK(rig.step().isEnabled());

    // The key does the same, now that the game is paused.
    rig.press(*rig.window.outliner_, Qt::ControlModifier | Qt::AltModifier);
    REQUIRE(rig.pumpUntil([&] { return rig.line() == shown("Paused", stopped + 2); }));
    CHECK(rig.sent() == 3);
}

TEST_CASE("PM-3a nothing can be pressed while an answer is awaited, and a refusal is one line",
          "[devstudio][play][pm3a][negative-contrast]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.pause().isVisible(); }));

    rig.engine.hold = true;
    rig.pause().trigger();
    rig.pumpFor(30);
    REQUIRE(rig.sent() == 1);
    CHECK_FALSE(rig.pause().isEnabled());
    CHECK_FALSE(rig.step().isEnabled());
    rig.pause().trigger();
    rig.press(*rig.window.outliner_, Qt::ControlModifier | Qt::ShiftModifier);
    rig.pumpFor(50);
    CHECK(rig.sent() == 1);
    rig.engine.release();
    REQUIRE(rig.pumpUntil([&] { return rig.pause().isEnabled() && rig.pause().isChecked(); }));

    // The engine refuses a step: the status bar says why in words, and the
    // state is read again.
    rig.engine.refuse_next = Json{{"code", "play_state_mismatch"},
                                  {"state", "playing"},
                                  {"required", "paused"}};
    const auto reads = rig.engine.status_reads;
    rig.step().trigger();
    REQUIRE(rig.pumpUntil([&] { return !rig.window.statusBar()->currentMessage().isEmpty(); }));
    CHECK(rig.window.statusBar()->currentMessage() ==
          QStringLiteral("The game is playing; this works only while it is paused "
                         "(play_state_mismatch)."));
    REQUIRE(rig.pumpUntil([&] { return rig.engine.status_reads > reads; }));
}

TEST_CASE("PM-3a the play keys do nothing while a person is typing",
          "[devstudio][play][pm3a][negative-contrast]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.pause().isVisible(); }));
    auto *field = new QLineEdit(&rig.window);
    field->setObjectName(QStringLiteral("pm3a.field"));
    field->show();
    rig.press(*field, Qt::ControlModifier | Qt::ShiftModifier);
    rig.pumpFor(80);
    CHECK(rig.sent() == 0);

    // With the keyboard elsewhere, the same key pauses.
    rig.press(*rig.window.outliner_, Qt::ControlModifier | Qt::ShiftModifier);
    REQUIRE(rig.pumpUntil([&] { return rig.sent() == 1; }));
    CHECK(rig.last() == Json{{"method", "set_play_state"}, {"params", {{"state", "paused"}}}});
}

TEST_CASE("PM-3a the Timeline cannot be scrubbed while the game plays",
          "[devstudio][play][pm3a][timeline][negative-contrast]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
    REQUIRE(rig.graph().connected());
    auto *strip = rig.window.findChild<QWidget *>(QStringLiteral("pelican.graphTimeline"));
    REQUIRE(strip != nullptr);

    (void)rig.graph().takeRpcRequests();
    CHECK_FALSE(rig.graph().scrubBlockedReason().empty());
    CHECK_FALSE(rig.graph().scrub(0.5));
    for (const auto &request : rig.graph().takeRpcRequests()) CHECK(request.method != "set_time");
    CHECK(strip->toolTip().contains(QStringLiteral("Pause")));

    rig.pause().trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Paused")); }));
    CHECK(rig.graph().scrubBlockedReason().empty());
    (void)rig.graph().takeRpcRequests();
    CHECK(rig.graph().scrub(0.5));
    const auto scrub = rig.graph().takeRpcRequests();
    REQUIRE_FALSE(scrub.empty());
    CHECK(scrub.front().method == "set_time");
    CHECK_FALSE(strip->toolTip().contains(QStringLiteral("Pause")));
}

TEST_CASE("PM-3a an engine without play gets no buttons, and a restart shows playing again",
          "[devstudio][play][pm3a][negative-contrast]") {
    SECTION("an engine from before PM-1a") {
        Rig rig{false};
        REQUIRE(rig.pumpUntil([&] { return rig.engine.status_reads > 0; }));
        rig.pumpFor(50);
        CHECK_FALSE(rig.pause().isVisible());
        CHECK_FALSE(rig.step().isVisible());
        CHECK(rig.controls.statusLabel()->isHidden());
        // Its Timeline keeps the scrub it had.
        CHECK(rig.graph().scrubBlockedReason().empty());
    }
    SECTION("a restart after a pause") {
        Rig rig;
        REQUIRE(rig.pumpUntil([&] { return rig.pause().isVisible(); }));
        rig.pause().trigger();
        REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Paused")); }));
        REQUIRE(rig.frameColour() == currentStudioTheme().play.paused_frame);

        // The engine goes away: nothing paused stays on screen.
        rig.viewport.rpc_ready_ = false;
        emit rig.viewport.engineRpcBecameUnavailable(QStringLiteral("restart"));
        rig.pumpFor(30);
        CHECK_FALSE(rig.pause().isVisible());
        CHECK_FALSE(rig.pause().isChecked());
        CHECK(rig.frameColour() != currentStudioTheme().play.paused_frame);
        CHECK(rig.bandStyle().isEmpty());

        // The next engine starts playing, and says so.
        rig.engine.state = "playing";
        rig.viewport.rpc_ready_ = true;
        emit rig.viewport.engineRpcBecameAvailable();
        REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
        CHECK(rig.pause().isVisible());
        CHECK_FALSE(rig.pause().isChecked());
    }
}

TEST_CASE("PM-3a another panel's get_status moves the line; no clock of its own reads",
          "[devstudio][play][pm3a]") {
    Rig rig;
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
    // Idle: no reads while nothing changes.
    const auto reads = rig.engine.status_reads;
    rig.pumpFor(300);
    CHECK(rig.engine.status_reads == reads);

    // Something else pauses the game (another client); the next get_status
    // anyone in Studio reads -- here the Graph tab's kind -- shows it.
    rig.engine.state = "paused";
    QString error;
    REQUIRE(rig.viewport.requestRpc(QStringLiteral("get_status"), QJsonObject{}, &error) != 0);
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Paused")); }));
    CHECK(rig.pause().isChecked());
}

} // namespace PelicanStudio
