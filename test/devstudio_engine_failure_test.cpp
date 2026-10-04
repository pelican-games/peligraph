#include "embeddedviewport.hpp"
#include "enginefailuremodel.hpp"
#include "frameplanwidget.hpp"
#include "problemsdock.hpp"
#include "renderfallbacknotice.hpp"
#include "../src/devstudio/model/inspectormodel.hpp"

#include "../src/core/asset/model.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/project/renderconfigsection.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <QApplication>
#include <QByteArray>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonValue>
#include <QLabel>
#include <QObject>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace PelicanStudio {
namespace {

namespace fs = std::filesystem;

fs::path filesystemPath(const QString &path) {
#ifdef _WIN32
    return fs::path{path.toStdWString()};
#else
    return fs::path{path.toStdString()};
#endif
}

QString displayPath(const fs::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

std::string readText(const fs::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("could not read " + path.string());
    }
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

void writeText(const fs::path &path, const std::string &text) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error("could not write " + path.string());
    }
    output << text;
    if (!output) {
        throw std::runtime_error("could not finish writing " + path.string());
    }
}

void copyExampleProject(const fs::path &source, const fs::path &destination) {
    fs::create_directories(destination);
    for (const auto &entry : fs::recursive_directory_iterator{source}) {
        const fs::path relative = fs::relative(entry.path(), source);
        const fs::path target = destination / relative;
        if (entry.is_directory()) {
            fs::create_directories(target);
            continue;
        }
        if (!entry.is_regular_file()) {
            continue;
        }

        fs::create_directories(target.parent_path());
        const auto first_component = relative.begin();
        const bool is_asset_payload =
            first_component != relative.end() &&
            *first_component == fs::path{"assets"} &&
            relative != fs::path{"assets"} / "asset_data.json";
        if (is_asset_payload) {
            std::error_code hard_link_error;
            fs::create_hard_link(entry.path(), target, hard_link_error);
            if (!hard_link_error) {
                continue;
            }
        }
        fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
    }
}

void removeForwardTransparentPass(const fs::path &config_path) {
    nlohmann::json config = nlohmann::json::parse(readText(config_path));
    // P-2a-4b: the shipping project is a `pelican.graph` root, so the pass to
    // remove lives in its `rendering` section. The document is written back
    // whole, root keys included.
    auto &passes = Pelican::renderingSectionOf(config)
                       .at("rendering_passes")
                       .at(0)
                       .at("passes");
    const auto original_size = passes.size();
    passes.erase(
        std::remove_if(passes.begin(), passes.end(), [](const auto &pass) {
            return pass.value("name", "") == "forward_transparent";
        }),
        passes.end());
    if (passes.size() + 1 != original_size) {
        throw std::runtime_error(
            "example must contain exactly one forward_transparent pass");
    }
    writeText(config_path, config.dump(2) + "\n");
}

void addAliciaModel(const fs::path &asset_data_path) {
    nlohmann::json asset_data =
        nlohmann::json::parse(readText(asset_data_path));
    auto &models = asset_data.at("models");
    const bool already_present =
        std::any_of(models.begin(), models.end(), [](const auto &model) {
            return model.value("name", "") == "alicia";
        });
    if (already_present) {
        throw std::runtime_error(
            "example asset data already contains the Alicia control model");
    }
    models.push_back({
        {"name", "alicia"},
        {"path", "assets/models/AliciaSolid.vrm"},
    });
    writeText(asset_data_path, asset_data.dump(2) + "\n");
}

QString lastFatalLine(const QString &standard_error) {
    QString result;
    const QStringList lines = standard_error.split(QLatin1Char('\n'));
    for (QString line : lines) {
        if (line.endsWith(QLatin1Char('\r'))) {
            line.chop(1);
        }
        if (line.contains(QStringLiteral("Pelican fatal error"))) {
            result = std::move(line);
        }
    }
    return result;
}

class EnvironmentVariableGuard {
  public:
    EnvironmentVariableGuard(const char *name, const QByteArray &value)
        : name_{name}, was_set_{qEnvironmentVariableIsSet(name)},
          previous_{qgetenv(name)} {
        qputenv(name_.constData(), value);
    }

    ~EnvironmentVariableGuard() {
        if (was_set_) {
            qputenv(name_.constData(), previous_);
        } else {
            qunsetenv(name_.constData());
        }
    }

    EnvironmentVariableGuard(const EnvironmentVariableGuard &) = delete;
    EnvironmentVariableGuard &operator=(const EnvironmentVariableGuard &) = delete;

  private:
    QByteArray name_;
    bool was_set_ = false;
    QByteArray previous_;
};

struct ViewportObservation {
    QString studio_output;
    QString standard_error;
    QString fatal_error_line;
    QJsonValue rpc_result;
    bool stopped = false;
    bool rpc_available = false;
    bool rpc_received = false;
};

void observeViewport(EmbeddedViewport &viewport,
                     ViewportObservation &observation,
                     QEventLoop &ready_loop,
                     QEventLoop &rpc_loop,
                     QEventLoop &stopped_loop,
                     QObject &connection_context) {
    QObject::connect(
        &viewport, &EmbeddedViewport::engineOutputReceived,
        &connection_context,
        [&](const QString &output) { observation.studio_output += output; });
    QObject::connect(
        &viewport, &EmbeddedViewport::engineStandardErrorReceived,
        &connection_context,
        [&](const QString &output) { observation.standard_error += output; });
    QObject::connect(
        &viewport, &EmbeddedViewport::engineProcessExitedWithFailure,
        &connection_context,
        [&](const QString &fatal_error_line) {
            observation.fatal_error_line = fatal_error_line;
        });
    QObject::connect(
        &viewport, &EmbeddedViewport::engineRpcBecameAvailable,
        &connection_context, [&] {
            observation.rpc_available = true;
            ready_loop.quit();
        });
    QObject::connect(
        &viewport, &EmbeddedViewport::inspectorRpcSucceeded,
        &connection_context,
        [&](qint64, const QByteArray &result_json) {
            observation.rpc_received = true;
            observation.rpc_result =
                QJsonDocument::fromJson(result_json).object();
            rpc_loop.quit();
        });
    QObject::connect(
        &viewport, &EmbeddedViewport::engineRpcBecameUnavailable,
        &connection_context,
        [&](const QString &) {
            observation.stopped = true;
            ready_loop.quit();
            rpc_loop.quit();
            stopped_loop.quit();
        });
}

bool waitForLiveFixtureChild(EmbeddedViewport &viewport) {
    bool ready = false;
    QEventLoop loop;
    QObject::connect(
        &viewport, &EmbeddedViewport::engineOutputReceived, &loop,
        [&](const QString &output) {
            if (!output.contains(QStringLiteral("viewport-hang-ready"))) {
                return;
            }
            ready = true;
            loop.quit();
        });
    viewport.openProject(QDir::tempPath());
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    if (!ready) {
        loop.exec();
    }
    return ready;
}

} // namespace

TEST_CASE("Embedded viewport suppresses RPC-unavailable while destroying a live child",
          "[devstudio][process][lifetime][wp351b]") {
    int argument_count = 1;
    char application_name[] = "pelican_wp351b_shutdown_signal_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};
    EnvironmentVariableGuard player_path{
        "PELICAN_STUDIO_PLAYER",
        QByteArray{PELICAN_WP351B_LIVE_CHILD}};
    EnvironmentVariableGuard child_mode{
        "PELICAN_WP173_CHILD_MODE", QByteArray{"viewport-hang"}};
    EnvironmentVariableGuard player_arguments{
        "PELICAN_STUDIO_PLAYER_ARGUMENTS", QByteArray{}};

    int unavailable_count = 0;
    QObject receiver;
    {
        EmbeddedViewport viewport;
        QObject::connect(
            &viewport, &EmbeddedViewport::engineRpcBecameUnavailable,
            &receiver,
            [&](const QString &) { ++unavailable_count; });
        REQUIRE(waitForLiveFixtureChild(viewport));
    }

    REQUIRE(unavailable_count == 0);
}

TEST_CASE("Viewport observation disconnects before a live child outlives its local scope",
          "[devstudio][process][lifetime][wp351b]") {
    int argument_count = 1;
    char application_name[] = "pelican_wp351b_observer_lifetime_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};
    EnvironmentVariableGuard player_path{
        "PELICAN_STUDIO_PLAYER",
        QByteArray{PELICAN_WP351B_LIVE_CHILD}};
    EnvironmentVariableGuard child_mode{
        "PELICAN_WP173_CHILD_MODE", QByteArray{"viewport-hang"}};
    EnvironmentVariableGuard player_arguments{
        "PELICAN_STUDIO_PLAYER_ARGUMENTS", QByteArray{}};

    ViewportObservation observation;
    QEventLoop ready_loop;
    QEventLoop rpc_loop;
    QEventLoop stopped_loop;
    EmbeddedViewport viewport;
    {
        QObject connection_context;
        observeViewport(viewport, observation, ready_loop, rpc_loop,
                        stopped_loop, connection_context);
        REQUIRE(waitForLiveFixtureChild(viewport));
    }

    emit viewport.engineRpcBecameUnavailable(
        QStringLiteral("post-observation lifetime probe"));
    REQUIRE_FALSE(observation.stopped);
}

TEST_CASE("Engine failure model keeps recent stderr and contrasts clean exits",
          "[devstudio][failure][negative-contrast][wp298]") {
    EngineFailureModel model{2};
    model.beginRun();
    model.appendStandardError(QStringLiteral(
        "old line\nPelican fatal error : first\nPelican fatal "));
    model.appendStandardError(QStringLiteral("error : running value\n"));

    const auto failed = model.finishRun(1);
    REQUIRE(failed.has_value());
    REQUIRE(failed->fatal_error_line ==
            QStringLiteral("Pelican fatal error : running value"));
    REQUIRE(model.recentStandardErrorLines() ==
            QStringList{QStringLiteral("Pelican fatal error : first"),
                        QStringLiteral("Pelican fatal error : running value")});

    model.beginRun();
    model.appendStandardError(QStringLiteral("normal startup\n"));
    REQUIRE_FALSE(model.finishRun(0).has_value());
}

TEST_CASE("Frame plan distinguishes not-started guidance from a running engine fatal line",
          "[devstudio][frame-plan][failure][negative-contrast][wp298]") {
    int argument_count = 1;
    char application_name[] = "pelican_engine_failure_view_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};
    EmbeddedViewport viewport;
    FramePlanWidget frame_plan{&viewport};
    auto *status = frame_plan.findChild<QLabel *>(
        QStringLiteral("pelican.framePlanStatus"));
    REQUIRE(status != nullptr);
    const QString not_started = status->text();
    REQUIRE(not_started.contains(QStringLiteral("not running")));

    const QString engine_line = QStringLiteral(
        "Pelican fatal error : route from the running engine");
    emit viewport.engineProcessExitedWithFailure(engine_line);
    QApplication::processEvents();

    REQUIRE(status->text() == engine_line);
    REQUIRE(status->text() != not_started);
}

TEST_CASE("Studio receives the running example fatal text while RPC stdout stays framed",
          "[devstudio][process][failure][rpc][negative-contrast][wp298][WP380c]") {
    int argument_count = 1;
    char application_name[] = "pelican_engine_failure_entry_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

    const fs::path source_project =
        filesystemPath(QString::fromUtf8(PELICAN_WP298_EXAMPLE_PROJECT));
    if (!fs::is_regular_file(
            source_project / "assets" / "models" / "sponza.glb") ||
        !fs::is_regular_file(
            source_project / "assets" / "models" / "AliciaSolid.vrm")) {
        SKIP("projects/example model assets are not available");
    }

    QTemporaryDir sandbox{
        QDir::temp().filePath(QStringLiteral("pelican_wp298_XXXXXX"))};
    REQUIRE(sandbox.isValid());
    const fs::path sandbox_root = filesystemPath(sandbox.path());
    const fs::path test_project = sandbox_root / "projects" / "example";
    copyExampleProject(source_project, test_project);

    const fs::path rendering_config =
        test_project / "passes" / "main_rendering_config.json";
    const fs::path asset_data =
        test_project / "assets" / "asset_data.json";
    const std::string original_config = readText(rendering_config);
    const std::string original_asset_data = readText(asset_data);
    addAliciaModel(asset_data);
    removeForwardTransparentPass(rendering_config);

    const QString player = QString::fromUtf8(PELICAN_WP298_PLAYER);
    EnvironmentVariableGuard player_path{
        "PELICAN_STUDIO_PLAYER", player.toLocal8Bit()};

    // PB-1s (engines BS-1 and PB-1): Studio starts its engine with
    // --render-fallback, and a start whose materials find no pass for their
    // route (material_route_unserved@1) is a start that falls back now. The
    // engine keeps running on its placeholder; the row above the viewport
    // and the Frame Plan's status say why, and the engine's list holds it as
    // one problem met at start -- the Problems dock's row.
    ViewportObservation broken;
    QString not_started_status;
    QString broken_status;
    nlohmann::json broken_reading;
    QString notice_line;
    std::vector<ProblemRow> problem_rows;
    bool running_after = false;
    {
        EnvironmentVariableGuard player_arguments{
            // Exercise Studio's windowed launch path. A headless child can
            // never satisfy native-window discovery, so loading large models
            // may kill it before the intended material-pass refusal occurs.
            "PELICAN_STUDIO_PLAYER_ARGUMENTS", QByteArray{"--size 320x180"}};
        EmbeddedViewport viewport;
        FramePlanWidget frame_plan{&viewport};
        auto *status = frame_plan.findChild<QLabel *>(
            QStringLiteral("pelican.framePlanStatus"));
        REQUIRE(status != nullptr);
        not_started_status = status->text();
        RenderFallbackNotice notice{RenderFallbackNotice::Hooks{}};
        notice.setSeeProblems(true);
        ProblemsDock problems{ProblemsDock::Hooks{
            .send = [&viewport](const QString &method, const QJsonObject &params, QString *error) {
                return viewport.requestRpc(method, params, error);
            },
            .read_status = [&viewport] {
                (void)viewport.requestRpc(QStringLiteral("get_status"), QJsonObject{});
            },
        }};
        QEventLoop ready_loop;
        QEventLoop rpc_loop;
        QEventLoop stopped_loop;
        QObject connection_context;
        observeViewport(viewport, broken, ready_loop, rpc_loop, stopped_loop,
                        connection_context);
        // What the window does with every get_status and every reply.
        QObject::connect(&viewport, &EmbeddedViewport::engineStatusParsed, &connection_context,
                         [&](qint64, const nlohmann::json &parsed) {
                             if (broken_reading.is_null() && parsed.contains("render_pipeline"))
                                 broken_reading = parsed;
                             notice.observeStatus(parsed);
                             problems.observeStatus(parsed);
                         });
        QObject::connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded, &connection_context,
                         [&](qint64 request_id, const QByteArray &result_json) {
                             (void)problems.receiveResult(request_id, result_json);
                         });
        QObject::connect(&viewport, &EmbeddedViewport::engineRpcBecameAvailable, &connection_context,
                         [&] { problems.engineAvailable(); });

        viewport.resize(640, 480);
        viewport.show();
        viewport.openProject(displayPath(test_project));
        QTimer::singleShot(60000, &ready_loop, &QEventLoop::quit);
        if (!broken.rpc_available && !broken.stopped) ready_loop.exec();
        // The Problems row (get_problems, read once the revision is seen)
        // and the Frame Plan's status (its plan of the placeholder).
        QEventLoop listed_loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &listed_loop, [&] {
            if (broken.stopped ||
                (!problems.session().rows().empty() &&
                 status->text().startsWith(QStringLiteral("Showing a placeholder"))))
                listed_loop.quit();
        });
        poll.start(50);
        QTimer::singleShot(30000, &listed_loop, &QEventLoop::quit);
        listed_loop.exec();
        QApplication::processEvents();
        broken_status = status->text();
        notice_line = notice.line()->text();
        problem_rows = problems.session().rows();
        running_after = viewport.engineRunning();
    }

    INFO("engine stderr: " << broken.standard_error.toStdString());
    INFO("Studio process output: " << broken.studio_output.toStdString());
    INFO("first get_status: " << broken_reading.dump().substr(0, 2000));
    INFO("Frame Plan status: " << broken_status.toStdString());
    INFO("placeholder row: " << notice_line.toStdString());
    // The engine did not stop: it answered, and still ran when let go.
    REQUIRE(broken.rpc_available);
    CHECK_FALSE(broken.stopped);
    CHECK(running_after);
    CHECK(broken.fatal_error_line.isEmpty());
    CHECK(lastFatalLine(broken.standard_error).isEmpty());
    // It draws its placeholder for the refusal the load names (G-4: the
    // same code and route hint as an edit ticket's rejection).
    const auto reading = readRenderPipeline(broken_reading);
    REQUIRE(reading.state == RenderPipelineReading::State::Fallback);
    CHECK(reading.refusal.value("code", std::string{}) == "material_route_unserved@1");
    CHECK_THAT(reading.refusal.dump(), Catch::Matchers::ContainsSubstring("forward_transparent"));
    // The row above the viewport and the Frame Plan's status say which
    // route no pass draws and what to add.
    CHECK(notice_line.startsWith(QStringLiteral("Showing a placeholder: the rendering setup was refused")));
    CHECK(notice_line.contains(QStringLiteral("add a forward_transparent_v1 material pass")));
    CHECK(notice_line.contains(QStringLiteral("(material_route_unserved@1)")));
    CHECK(notice_line.endsWith(QStringLiteral("See Problems.")));
    CHECK(broken_status.startsWith(QStringLiteral("Showing a placeholder")));
    CHECK(broken_status.contains(QStringLiteral("material_route_unserved@1")));
    CHECK(broken_status != not_started_status);
    // The engine's list: one problem, met at start, fixed in the rendering
    // section of the root.
    REQUIRE(problem_rows.size() == 1);
    const auto &row = problem_rows.front();
    CHECK(row.code == "material_route_unserved@1");
    CHECK(row.source == "startup");
    CHECK(row.error());
    CHECK(row.document_kind == "root_rendering");
    CHECK(row.document == "passes/main_rendering_config.json");
    CHECK(row.place == ProblemRow::Place::RenderingSettings);
    CHECK_THAT(row.sentence, Catch::Matchers::ContainsSubstring("add a forward_transparent_v1 material pass"));
    // The RPC replies were consumed by EngineProcess: stdout contributed no
    // byte to what Studio forwards.
    REQUIRE(broken.studio_output == broken.standard_error);

    // Studio gives each project's player its own log, truncated per run.
    const fs::path player_log =
        test_project / ".pelican" / "logs" / "player.log";
    REQUIRE(fs::is_regular_file(player_log));
    const std::string broken_log = readText(player_log);

    // Restore the deliberately broken example before running the control.
    writeText(rendering_config, original_config);
    writeText(asset_data, original_asset_data);
    REQUIRE(readText(rendering_config) == original_config);
    REQUIRE(readText(asset_data) == original_asset_data);

    ViewportObservation normal;
    QString running_status;
    QString stopped_status;
    {
        EnvironmentVariableGuard player_arguments{
            "PELICAN_STUDIO_PLAYER_ARGUMENTS",
            QByteArray{"--frames 600 --fps 120 --size 320x180"}};
        EmbeddedViewport viewport;
        FramePlanWidget frame_plan{&viewport};
        auto *status = frame_plan.findChild<QLabel *>(
            QStringLiteral("pelican.framePlanStatus"));
        REQUIRE(status != nullptr);
        QEventLoop ready_loop;
        QEventLoop rpc_loop;
        QEventLoop stopped_loop;
        QObject connection_context;
        observeViewport(viewport, normal, ready_loop, rpc_loop, stopped_loop,
                        connection_context);

        viewport.resize(640, 480);
        viewport.show();
        viewport.openProject(displayPath(test_project));
        QTimer::singleShot(30000, &ready_loop, &QEventLoop::quit);
        if (!normal.rpc_available && !normal.stopped) {
            ready_loop.exec();
        }
        REQUIRE(normal.rpc_available);

        QTimer::singleShot(10000, &rpc_loop, &QEventLoop::quit);
        if (!normal.rpc_received && !normal.stopped) {
            rpc_loop.exec();
        }
        REQUIRE(normal.rpc_received);
        running_status = status->text();

        QTimer::singleShot(30000, &stopped_loop, &QEventLoop::quit);
        if (!normal.stopped) {
            stopped_loop.exec();
        }
        QApplication::processEvents();
        stopped_status = status->text();
    }

    REQUIRE(normal.stopped);
    REQUIRE(normal.fatal_error_line.isEmpty());
    REQUIRE(lastFatalLine(normal.standard_error).isEmpty());
    REQUIRE(normal.rpc_result.isObject());
    // The JSON-RPC response was consumed by EngineProcess. Any extra stdout
    // byte would be forwarded and make these streams differ.
    REQUIRE(normal.studio_output == normal.standard_error);
    REQUIRE_FALSE(running_status.contains(
        QStringLiteral("Pelican fatal error")));
    REQUIRE_FALSE(stopped_status.contains(
        QStringLiteral("Pelican fatal error")));
    // The control run truncated the file: it holds this run, not the failed one.
    const QString control_log = QString::fromStdString(readText(player_log));
    REQUIRE(control_log.contains(QStringLiteral("starting main loop")));
    REQUIRE(QString::fromStdString(broken_log) != control_log);
}

// UX-9, decision 5 (engine CT-1) -> BS-1s, decision 4 (engine BS-1): a
// document the engine cannot read at start -- a buffer port without its
// element -- no longer stops the engine Studio starts (--render-fallback). It
// keeps running on its placeholder, says why in get_status.render_pipeline,
// and the Frame Plan's status says the code's sentence and the file to fix,
// read from what the running engine really answers. The fatal line a player
// started without the flag writes is still read (engineStoppedSentence, and
// the test above it).
TEST_CASE("BS-1s decision 4 (UX-9 decision 5): an engine Studio starts on a buffer port without its element "
          "keeps running on its placeholder, and the Frame Plan's status says the code's sentence and the file",
          "[devstudio][frame-plan][failure][ux9][bs1s]") {
    int argument_count = 1;
    char application_name[] = "pelican_engine_failure_ux9_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

    QTemporaryDir sandbox{QDir::temp().filePath(QStringLiteral("pelican_ux9_fatal_XXXXXX"))};
    REQUIRE(sandbox.isValid());
    const fs::path project = filesystemPath(sandbox.path()) / "project";
    fs::copy(fs::path{PELICAN_UX9_FIXTURE_PROJECT}, project, fs::copy_options::recursive);
    const fs::path document_path = project / "passes" / "main.json";
    auto document = nlohmann::json::parse(readText(document_path));
    // The case CT-1 names: a compute task's buffer port, written by hand
    // without the element it holds.
    document.at("nodes").push_back(
        {{"id", "sim"},
         {"op", "render.compute"},
         {"parameters",
          {{"ports", {{"data", {{"kind", "buffer"}, {"access", "read_write"}}}}},
           {"shader", "shaders/sim.comp"}}},
         {"outputs", nlohmann::json::array()}});
    writeText(document_path, document.dump(2) + "\n");

    EnvironmentVariableGuard player_path{
        "PELICAN_STUDIO_PLAYER", QString::fromUtf8(PELICAN_WP298_PLAYER).toLocal8Bit()};
    EnvironmentVariableGuard player_arguments{"PELICAN_STUDIO_PLAYER_ARGUMENTS",
                                              QByteArray{"--size 320x180"}};
    const QString expected = QStringLiteral(
        "Showing a placeholder: the rendering setup was refused — Port data of sim needs element "
        "(graph.port_field_missing@1) in passes/main.json. Fix it, then Reload from disk.");
    ViewportObservation observed;
    nlohmann::json status;
    QString status_text;
    bool running_after = false;
    {
        EmbeddedViewport viewport;
        FramePlanWidget frame_plan{&viewport};
        auto *frame_status = frame_plan.findChild<QLabel *>(QStringLiteral("pelican.framePlanStatus"));
        REQUIRE(frame_status != nullptr);
        QEventLoop ready_loop;
        QEventLoop rpc_loop;
        QEventLoop stopped_loop;
        QObject connection_context;
        observeViewport(viewport, observed, ready_loop, rpc_loop, stopped_loop, connection_context);
        // The readiness probe's get_status is the first status read.
        QObject::connect(&viewport, &EmbeddedViewport::engineStatusParsed, &connection_context,
                         [&](qint64, const nlohmann::json &parsed) {
                             if (status.is_null()) status = parsed;
                         });
        viewport.resize(640, 480);
        viewport.show();
        viewport.openProject(displayPath(project));
        QTimer::singleShot(60000, &ready_loop, &QEventLoop::quit);
        if (!observed.rpc_available && !observed.stopped) ready_loop.exec();
        // The Frame Plan reads the plan when the engine answers; the plan of
        // the placeholder carries the same render_pipeline.
        QEventLoop plan_loop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &plan_loop, [&] {
            if (frame_status->text() == expected || observed.stopped) plan_loop.quit();
        });
        poll.start(50);
        QTimer::singleShot(30000, &plan_loop, &QEventLoop::quit);
        if (frame_status->text() != expected && !observed.stopped) plan_loop.exec();
        QApplication::processEvents();
        status_text = frame_status->text();
        running_after = viewport.engineRunning();
    }

    INFO("engine stderr: " << observed.standard_error.toStdString());
    INFO("Frame Plan status: " << status_text.toStdString());
    INFO("first get_status: " << status.dump().substr(0, 2000));
    // The engine did not stop: it answered, and was still running when the
    // test let it go.
    REQUIRE(observed.rpc_available);
    CHECK_FALSE(observed.stopped);
    CHECK(running_after);
    CHECK(observed.fatal_error_line.isEmpty());
    CHECK(lastFatalLine(observed.standard_error).isEmpty());
    // It said why, as the edit's refusal names it.
    const auto reading = readRenderPipeline(status);
    REQUIRE(reading.state == RenderPipelineReading::State::Fallback);
    CHECK(reading.refusal.value("code", std::string{}) == "graph.port_field_missing@1");
    CHECK(renderFallbackNode(reading) == "sim");
    // The sentence Studio has for the code, with the port, the node, the
    // field and the document the engine named.
    CHECK(status_text == expected);
}

} // namespace PelicanStudio
