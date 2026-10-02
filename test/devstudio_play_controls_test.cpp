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
#include "../src/devstudio/viewport/studioplayerarguments.hpp"
#include "../src/devstudio/viewport/viewportgeometry.hpp"
#include "graphcanvaswidget.hpp"
#include "outlinertree.hpp"
#include "playcontrols.hpp"
#include "studiotheme.hpp"

#include <QAction>
#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QMessageBox>
#include <QPushButton>
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
    // PM-3b: report view_camera, session and recorded_edits (PM-1b / PM-2).
    bool report_session = false;
    bool session = false;
    std::string view = "game";
    std::vector<Json> recorded;
    // The not_kept a Stop with keep answers with.
    Json not_kept = Json::array();
    int reloads = 0;
    int saves = 0;
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
        if (method == "set_play_state" || method == "advance_frames" ||
            method == "set_view_camera" || method == "reload_game_logic") {
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
            Json answer{{"state", state}, {"previous", previous}};
            if (previous == "edit" && state != "edit") {
                session = true;
                view = "game";
                game_frame = 0;
            }
            if (state == "edit" && previous != "edit") {
                session = false;
                view = "editor";
                recorded.clear();
                if (params.contains("keep")) answer["not_kept"] = not_kept;
            }
            result(id, answer);
            return;
        }
        if (method == "set_view_camera") {
            if (state == "playing") {
                error(id, -32000, "set_view_camera: playing",
                      {{"code", "play_state_mismatch"},
                       {"state", state},
                       {"required", Json::array({"edit", "paused"})}});
                return;
            }
            const auto previous = view;
            view = params.at("camera").get<std::string>();
            result(id, {{"camera", view}, {"previous", previous}});
            return;
        }
        if (method == "reload_game_logic") {
            ++reloads;
            result(id, {{"generation", reloads}, {"systems", 1}});
            return;
        }
        if (method == "save_scene") {
            ++saves;
            result(id, {{"status", "saved"}, {"scene_revision", 1}});
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
                if (report_session) {
                    status["play"]["view_camera"] = view;
                    status["play"]["session"] = session;
                    status["play"]["recorded_edits"] = recorded;
                }
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
        if (method == "scene_outline") {
            result(id, {{"scene_id", "main"},
                        {"scene_revision", 1},
                        {"not_modified", false},
                        {"objects", Json::array()}});
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

    explicit Rig(bool report_play = true, const std::function<void(FakeEngine &)> &setup = {}) {
        engine.report_play = report_play;
        if (setup) setup(engine);
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
    // PM-3b: set_view_camera names the states it works in as a list.
    CHECK(playRefusalDescription(Json{{"code", "play_state_mismatch"},
                                      {"state", "playing"},
                                      {"required", Json::array({"edit", "paused"})}}) ==
          "The game is playing; this works only while it is in edit or paused "
          "(play_state_mismatch).");
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
    // PM-3b: playing has a colour of its own.
    const QString playing_band = currentStudioTheme().play.playing_band.name();
    CHECK(rig.bandStyle().contains(playing_band));

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
    CHECK(rig.bandStyle().contains(playing_band));
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


// ---------------------------------------------------------------------------
// PM-3b: the engine starts in edit; Play / Stop, the Play changes panel and
// its Keep marks, the View choice, Reload game logic, what waits for Stop,
// and what is asked before leaving a run.
// ---------------------------------------------------------------------------

namespace {

// An engine as Studio starts it now: in edit, with PM-1b's view and PM-2's
// session in its get_status.
void editEngine(FakeEngine &engine) {
    engine.state = "edit";
    engine.report_session = true;
    engine.view = "editor";
    engine.game_frame = 0;
}

QCheckBox *changeRow(Rig &rig, int index) {
    return rig.window.findChild<QCheckBox *>(QStringLiteral("pelican.playChanges.row.%1").arg(index));
}

QLabel *namedLabel(Rig &rig, const char *name) {
    return rig.window.findChild<QLabel *>(QString::fromLatin1(name));
}

} // namespace

TEST_CASE("PM-3b Studio starts the engine in edit, unless its configured arguments run the game "
          "from the first frame",
          "[devstudio][play][pm3b][launch][negative-contrast]") {
    const auto has_edit = [](const std::vector<std::string> &arguments) {
        for (std::size_t index = 0; index + 1 < arguments.size(); ++index)
            if (arguments[index] == "--start-state" && arguments[index + 1] == "edit") return true;
        return false;
    };
    const auto plain = studioPlayerLaunch({}, "C:/project");
    CHECK(has_edit(plain.arguments));
    CHECK(plain.start_state_notice.empty());
    // With --embedded, as EW-2 passes it.
    CHECK(std::find(plain.arguments.begin(), plain.arguments.end(), "--embedded") !=
          plain.arguments.end());
    // The probe the CTest lanes use reads the same list.
    CHECK(studioPlayerArgumentStrings({}, "C:/project") == plain.arguments);
    // Headless with Studio's --rpc is an edit launch the engine accepts.
    CHECK(has_edit(studioPlayerLaunch({"--headless"}, "C:/project").arguments));

    for (const std::vector<std::string> &configured :
         {std::vector<std::string>{"--replay", "run.jsonl"},
          std::vector<std::string>{"--record-input=out.jsonl"},
          std::vector<std::string>{"--bake-camera-output", "cam.json"},
          std::vector<std::string>{"--start-time", "3"}}) {
        const auto launch = studioPlayerLaunch(configured, "C:/project");
        CHECK_FALSE(has_edit(launch.arguments));
        const auto flag = configured.front().substr(0, configured.front().find('='));
        CHECK(launch.start_state_notice.find(flag) != std::string::npos);
        CHECK(launch.start_state_notice.find("starts playing") != std::string::npos);
    }
    // A start state of its own wins, and needs no word.
    const auto own = studioPlayerLaunch({"--start-state=playing"}, "C:/project");
    CHECK_FALSE(has_edit(own.arguments));
    CHECK(own.start_state_notice.empty());
    // The Qt side says the same.
    CHECK(studioPlayerStartStateNotice({QStringLiteral("--replay"), QStringLiteral("x")},
                                       QStringLiteral("C:/project"))
              .contains(QStringLiteral("--replay")));
}

TEST_CASE("PM-3b get_status.play is read with edit, the view and the session's edits",
          "[devstudio][play][pm3b][negative-contrast]") {
    const Json status{{"play",
                       {{"state", "edit"},
                        {"game_frame", 12},
                        {"game_time", 0.0},
                        {"pending_steps", 0},
                        {"view_camera", "editor"},
                        {"session", false},
                        {"recorded_edits", Json::array()}}}};
    const auto reading = parsePlayStatus(status);
    REQUIRE(reading);
    CHECK(reading->state == PlayState::edit);
    CHECK(reading->view_camera == ViewCamera::editor);
    CHECK_FALSE(reading->session);

    PlayStateModel model;
    model.connect();
    (void)model.takeRequests();
    REQUIRE(model.observeStatus(status));
    CHECK(model.editing());
    CHECK(model.statusText() == "Edit");
    // In edit: Play and Step (which starts the run paused), not Pause.
    CHECK(model.canPlayOrStop());
    CHECK(model.canStep());
    CHECK_FALSE(model.canTogglePause());
    CHECK(model.canSetView());

    Json playing = status;
    playing["play"]["state"] = "playing";
    playing["play"]["session"] = true;
    playing["play"]["view_camera"] = "game";
    playing["play"]["recorded_edits"] =
        Json::array({{{"transaction_id", "txn-1"}, {"actor", 9}}});
    REQUIRE(model.observeStatus(playing));
    CHECK(model.session());
    CHECK_FALSE(model.canSetView());
    REQUIRE(model.status()->recorded_edits.size() == 1);
    CHECK(model.status()->recorded_edits.front() == RecordedEdit{"txn-1", 9});

    // A recorded edit without its transaction is not the engine's shape.
    Json broken = playing;
    broken["play"]["recorded_edits"] = Json::array({{{"actor", 9}}});
    CHECK_FALSE(parsePlayStatus(broken));
}

TEST_CASE("PM-3b Play from edit starts the run, and the band, the frame and the line follow; "
          "Stop with nothing marked sends no keep",
          "[devstudio][play][pm3b][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    QAction &play = *rig.controls.playAction();
    CHECK(play.isVisible());
    CHECK(play.isEnabled());
    CHECK(play.text() == QStringLiteral("▶ Play"));
    CHECK_FALSE(rig.pause().isEnabled());
    CHECK(rig.step().isEnabled());
    const StudioTheme &theme = currentStudioTheme();
    CHECK(rig.frameColour() != theme.play.playing_frame);
    CHECK(rig.frameColour() != theme.play.paused_frame);
    CHECK(rig.bandStyle().isEmpty());
    // In edit the Timeline can be scrubbed.
    CHECK(rig.graph().scrubBlockedReason().empty());

    // Ctrl+P: Play.
    rig.press(*rig.window.outliner_, Qt::ControlModifier);
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
    REQUIRE(rig.sent() == 1);
    CHECK(rig.last() == Json{{"method", "set_play_state"}, {"params", {{"state", "playing"}}}});
    CHECK(play.text() == QStringLiteral("■ Stop"));
    CHECK(rig.pause().isEnabled());
    CHECK_FALSE(rig.step().isEnabled());
    CHECK(rig.frameColour() == theme.play.playing_frame);
    CHECK(rig.bandStyle().contains(theme.play.playing_band.name()));
    CHECK_FALSE(rig.graph().scrubBlockedReason().empty());

    // Ctrl+P again: Stop, nothing marked, so no keep at all.
    rig.press(*rig.window.outliner_, Qt::ControlModifier);
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    REQUIRE(rig.sent() == 2);
    CHECK(rig.last() == Json{{"method", "set_play_state"}, {"params", {{"state", "edit"}}}});
    CHECK(play.text() == QStringLiteral("▶ Play"));
    CHECK(rig.frameColour() != theme.play.playing_frame);
    CHECK(rig.bandStyle().isEmpty());
    CHECK(rig.window.statusBar()->currentMessage().startsWith(QStringLiteral("Stopped: 0 reverted, 0 kept")));
}

TEST_CASE("PM-3b Step from edit starts the run paused, then steps it once",
          "[devstudio][play][pm3b][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    rig.step().trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line() == shown("Paused", 1); }));
    REQUIRE(rig.sent() == 2);
    CHECK(rig.engine.play_requests[0] ==
          Json{{"method", "set_play_state"}, {"params", {{"state", "paused"}}}});
    CHECK(rig.engine.play_requests[1] ==
          Json{{"method", "advance_frames"}, {"params", {{"count", 1}}}});
    CHECK(rig.pause().isChecked());
    CHECK(rig.controls.playAction()->text() == QStringLiteral("■ Stop"));
    CHECK(rig.frameColour() == currentStudioTheme().play.paused_frame);
}

TEST_CASE("PM-3b Play changes names each edit of the run, and Stop keeps only the marked ones "
          "still recorded",
          "[devstudio][play][pm3b][keep][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.controls.model().session(); }));

    // Studio's own edit, its undo and redo, and another client's edit.
    rig.engine.recorded = Json::array({{{"transaction_id", "txn-1"}, {"actor", 9}},
                                       {{"transaction_id", "txn-3"}, {"actor", 9}},
                                       {{"transaction_id", "txn-9"}, {"actor", 4}}});
    rig.controls.noteCommittedEdit({.transaction_id = "txn-1", .label = "Edit light.intensity"});
    rig.controls.noteCommittedEdit({.transaction_id = "txn-2",
                                    .kind = CommittedEditKind::undo,
                                    .source_transaction_id = "txn-1"});
    rig.controls.noteCommittedEdit({.transaction_id = "txn-3",
                                    .kind = CommittedEditKind::redo,
                                    .source_transaction_id = "txn-2"});
    REQUIRE(rig.pumpUntil([&] { return changeRow(rig, 2) != nullptr; }));
    rig.controls.showChanges();
    CHECK(changeRow(rig, 0)->text() == QStringLiteral("Edit light.intensity"));
    CHECK(changeRow(rig, 1)->text() == QStringLiteral("Redo Edit light.intensity"));
    CHECK(changeRow(rig, 2)->text() == QString::fromUtf8("Actor 4 · txn-9"));
    CHECK(rig.controls.model().labelOf("txn-2") == "Undo Edit light.intensity");
    for (int row = 0; row < 3; ++row) CHECK_FALSE(changeRow(rig, row)->isChecked());
    CHECK(rig.controls.changesAction()->text() == QStringLiteral("Play changes (3)"));

    // Keep all, then Keep none, then one mark by hand.
    rig.window.findChild<QPushButton *>(QStringLiteral("pelican.playChanges.keepAll"))->click();
    for (int row = 0; row < 3; ++row) CHECK(changeRow(rig, row)->isChecked());
    rig.window.findChild<QPushButton *>(QStringLiteral("pelican.playChanges.keepNone"))->click();
    for (int row = 0; row < 3; ++row) CHECK_FALSE(changeRow(rig, row)->isChecked());
    // A press where a person points: the row as laid out, near its right
    // end. A check box answers a press on its box and its words only, so a
    // row wider than its words would let this press fall on nothing.
    for (int row : {1, 2}) {
        QCheckBox *box = changeRow(rig, row);
        QTest::mouseClick(box, Qt::LeftButton, Qt::NoModifier,
                          QPoint{box->width() - 3, box->height() / 2});
    }
    CHECK(rig.controls.model().kept("txn-3"));
    CHECK(rig.controls.model().kept("txn-9"));

    // txn-9 is undone by its client before Stop: the recorded edits are read
    // first, and only txn-3 is named.
    rig.engine.recorded = Json::array({{{"transaction_id", "txn-1"}, {"actor", 9}},
                                       {{"transaction_id", "txn-3"}, {"actor", 9}}});
    const auto reads = rig.engine.status_reads;
    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    CHECK(rig.engine.status_reads > reads);
    CHECK(rig.last() == Json{{"method", "set_play_state"},
                             {"params", {{"state", "edit"}, {"keep", Json::array({"txn-3"})}}}});
    CHECK(rig.window.statusBar()->currentMessage() ==
          QStringLiteral("Stopped: 1 reverted, 1 kept"));
    // The marks went with the run.
    CHECK_FALSE(rig.controls.model().kept("txn-3"));
}

TEST_CASE("PM-3b an edit Stop could not keep is named in Play changes with why",
          "[devstudio][play][pm3b][keep][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.controls.model().session(); }));
    rig.engine.recorded = Json::array({{{"transaction_id", "txn-5"}, {"actor", 9}},
                                       {{"transaction_id", "txn-6"}, {"actor", 9}}});
    rig.controls.noteCommittedEdit({.transaction_id = "txn-6", .label = "Edit transform.pos"});
    REQUIRE(rig.pumpUntil([&] { return changeRow(rig, 1) != nullptr; }));
    rig.controls.keep(QStringLiteral("txn-6"));
    rig.engine.not_kept = Json::array({{{"transaction_id", "txn-6"}, {"code", "closure_unresolvable"}}});
    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    CHECK(rig.window.statusBar()->currentMessage() ==
          QStringLiteral("Stopped: 2 reverted, 0 kept, 1 could not be kept"));
    const QLabel *not_kept = namedLabel(rig, "pelican.playChanges.notKept");
    REQUIRE(not_kept != nullptr);
    CHECK_FALSE(not_kept->isHidden());
    CHECK(not_kept->text().contains(QStringLiteral("Edit transform.pos")));
    CHECK(not_kept->text().contains(QStringLiteral("closure_unresolvable")));
    CHECK(namedLabel(rig, "pelican.playChanges.summary")->text().contains(
        QStringLiteral("1 could not be kept")));
}

TEST_CASE("PM-3b Save waits for Stop, and the render panels say their edits stay",
          "[devstudio][play][pm3b][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    QAction &save = *rig.window.save_scene_action_;
    const QLabel *tools = namedLabel(rig, "pelican.play.renderBanner.renderTools");
    const QLabel *pass = namedLabel(rig, "pelican.play.renderBanner.passEditor");
    REQUIRE(tools != nullptr);
    REQUIRE(pass != nullptr);
    CHECK(save.isEnabled());
    CHECK(tools->isHidden());
    CHECK(pass->isHidden());

    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.controls.model().session(); }));
    CHECK_FALSE(save.isEnabled());
    CHECK(save.toolTip().contains(QStringLiteral("play_session_active")));
    CHECK_FALSE(tools->isHidden());
    CHECK_FALSE(pass->isHidden());
    CHECK(tools->text() == QStringLiteral("Changes here are written to files and stay after Stop."));
    save.trigger();
    rig.pumpFor(50);
    CHECK(rig.engine.saves == 0);

    // Paused is still the run.
    rig.pause().trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.pause().isChecked(); }));
    CHECK_FALSE(save.isEnabled());

    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    CHECK(save.isEnabled());
    CHECK(tools->isHidden());
    CHECK(pass->isHidden());
}

TEST_CASE("PM-3b View: Game | Editor while paused or in edit, not while playing",
          "[devstudio][play][pm3b][view][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    QAction &game = *rig.controls.viewGameAction();
    QAction &editor = *rig.controls.viewEditorAction();
    CHECK(game.isVisible());
    CHECK(editor.isChecked());

    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
    CHECK_FALSE(game.isVisible());
    CHECK_FALSE(editor.isVisible());

    rig.pause().trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Paused")); }));
    CHECK(game.isVisible());
    CHECK(game.isChecked());
    editor.trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.engine.view == "editor"; }));
    CHECK(rig.last() == Json{{"method", "set_view_camera"}, {"params", {{"camera", "editor"}}}});
    REQUIRE(rig.pumpUntil([&] { return editor.isChecked() && editor.isEnabled(); }));
    game.trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.engine.view == "game"; }));

    CHECK(playRefusalDescription(Json{{"code", "view_camera_unavailable"}, {"camera", "editor"}}) ==
          "This engine has no editor camera; it was started without --start-state edit or "
          "--free-camera (view_camera_unavailable).");
}

TEST_CASE("PM-3b play_busy says why in one line and can send the same thing again when done",
          "[devstudio][play][pm3b][busy][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    rig.engine.refuse_next = Json{{"code", "play_busy"},
                                  {"state", "edit"},
                                  {"requested", "playing"},
                                  {"pending_ticket_ids", Json::array({"edit:4"})},
                                  {"open_preview_lease", false}};
    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.controls.retryButton()->isVisible(); }));
    CHECK(rig.window.statusBar()->currentMessage().contains(QStringLiteral("(play_busy)")));
    CHECK(rig.line() == QStringLiteral("Edit"));
    REQUIRE(rig.sent() == 1);

    rig.controls.retryButton()->click();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
    REQUIRE(rig.sent() == 2);
    CHECK(rig.last() == Json{{"method", "set_play_state"}, {"params", {{"state", "playing"}}}});
    CHECK_FALSE(rig.controls.retryButton()->isVisible());
}

TEST_CASE("PM-3b Reload game logic is sent in any state",
          "[devstudio][play][pm3b][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    REQUIRE(rig.controls.reloadAction()->isEnabled());
    rig.controls.reloadAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.engine.reloads == 1; }));
    CHECK(rig.last() == Json{{"method", "reload_game_logic"}, {"params", Json::object()}});
    REQUIRE(rig.pumpUntil([&] {
        return rig.window.statusBar()->currentMessage().startsWith(
            QStringLiteral("Game logic reloaded"));
    }));
    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.line().startsWith(QStringLiteral("Playing")); }));
    REQUIRE(rig.controls.reloadAction()->isEnabled());
    rig.controls.reloadAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.engine.reloads == 2; }));
}

TEST_CASE("PM-3b the refusals of Play, Save and Stop read as sentences",
          "[devstudio][play][pm3b]") {
    CHECK(playRefusalDescription(Json{{"code", "game_logic_reload_failed@1"},
                                      {"state", "edit"},
                                      {"requested", "playing"},
                                      {"error_fields", {{"found", "v4"}, {"host", "v3"}}}}) ==
          "The game logic DLL did not reload, so the game did not start: found v4, host v3 "
          "(game_logic_reload_failed@1).");
    CHECK(playRefusalDescription(Json{{"code", "game_logic_abi_stale@1"},
                                      {"error_fields", Json::object()}}) ==
          "The game logic DLL was built for another engine version (game_logic_abi_stale@1).");
    CHECK(playRefusalDescription(Json{{"code", "play_session_active"},
                                      {"method", "save_scene"},
                                      {"state", "playing"}})
              .find("stop it first (save_scene) (play_session_active).") != std::string::npos);
    CHECK(knownRefusalDescription("play_session_active", "").find("stop it first") !=
          std::string::npos);
    CHECK(notKeptReason("closure_unresolvable").find("(closure_unresolvable)") !=
          std::string::npos);
    CHECK(notKeptReason("something_new") ==
          "the engine could not apply it again (something_new)");
}

TEST_CASE("PM-3b leaving while the game runs is asked first, and the run is stopped before it goes",
          "[devstudio][play][pm3b][confirm][negative-contrast]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    rig.controls.playAction()->trigger();
    REQUIRE(rig.pumpUntil([&] { return rig.controls.model().session(); }));
    const auto prompt = [&] {
        return rig.window.findChild<QMessageBox *>(QStringLiteral("pelican.playSessionPrompt"));
    };

    // Cancel: the run goes on, nothing is sent, Studio stays.
    rig.window.close();
    REQUIRE(rig.pumpUntil([&] { return prompt() != nullptr && prompt()->isVisible(); }));
    const auto before = rig.sent();
    prompt()->findChild<QPushButton *>(QStringLiteral("pelican.playSessionPrompt.cancel"))->click();
    REQUIRE(rig.pumpUntil([&] { return prompt() == nullptr || !prompt()->isVisible(); }));
    rig.pumpFor(100);
    CHECK(rig.sent() == before);
    CHECK(rig.window.isVisible());
    CHECK(rig.controls.model().session());

    // End the run: Stop with nothing kept, then Studio closes.
    rig.controls.keep(QStringLiteral("txn-1"));
    rig.window.close();
    REQUIRE(rig.pumpUntil([&] { return prompt() != nullptr && prompt()->isVisible(); }));
    prompt()->findChild<QPushButton *>(QStringLiteral("pelican.playSessionPrompt.end"))->click();
    REQUIRE(rig.pumpUntil([&] { return !rig.window.isVisible(); }));
    CHECK(rig.last() == Json{{"method", "set_play_state"}, {"params", {{"state", "edit"}}}});
    CHECK(rig.engine.state == "edit");
}

TEST_CASE("PM-3b with no run, leaving asks nothing about the game",
          "[devstudio][play][pm3b][confirm]") {
    Rig rig{true, editEngine};
    REQUIRE(rig.pumpUntil([&] { return rig.line() == QStringLiteral("Edit"); }));
    rig.window.close();
    REQUIRE(rig.pumpUntil([&] { return !rig.window.isVisible(); }));
    CHECK(rig.window.findChild<QMessageBox *>(QStringLiteral("pelican.playSessionPrompt")) == nullptr);
    CHECK(rig.sent() == 0);
}

} // namespace PelicanStudio
