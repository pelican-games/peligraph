#include "engineprocess.hpp"
#include "viewportgeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QProcess>

namespace PelicanStudio {
namespace {

QString fixtureExecutable() {
    return QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH);
}

} // namespace

TEST_CASE("Embedded viewport extent follows Qt device-pixel scaling", "[devstudio][viewport]") {
    REQUIRE(embeddedViewportPixelExtent(QSize{640, 360}, 1.0) == QSize{640, 360});
    REQUIRE(embeddedViewportPixelExtent(QSize{640, 360}, 1.5) == QSize{960, 540});
    REQUIRE(embeddedViewportPixelExtent(QSize{801, 451}, 1.25) == QSize{1001, 564});
    REQUIRE(embeddedViewportPixelExtent(QSize{0, 451}, 1.25) == QSize{0, 564});
    REQUIRE(embeddedViewportPixelExtent(QSize{-1, 451}, 1.25) == QSize{0, 564});
    REQUIRE(embeddedViewportPixelExtent(QSize{640, 360}, 0.0).isEmpty());
}

TEST_CASE("Forced engine child exit leaves the viewport process owner reusable",
          "[devstudio][viewport][process]") {
    EngineProcess process;
    int stopped_count = 0;
    qint64 stopped_process_id = 0;
    QString output;
    QObject::connect(&process, &EngineProcess::processStopped,
                     [&](qint64 process_id, int, QProcess::ExitStatus) {
                         ++stopped_count;
                         stopped_process_id = process_id;
                     });
    QObject::connect(&process, &EngineProcess::outputReceived,
                     [&](const QString &text) { output += text; });

    QString error;
    REQUIRE(process.start({fixtureExecutable(), {QStringLiteral("hang")}, {}}, &error));
    REQUIRE(error.isEmpty());
    REQUIRE(process.waitForStarted(5000));
    const qint64 first_process_id = process.processId();
    REQUIRE(first_process_id > 0);
    REQUIRE(process.isRunning());

    process.kill();
    REQUIRE(process.waitForFinished(5000));
    REQUIRE_FALSE(process.isRunning());
    REQUIRE(process.state() == EngineProcess::State::stopped);
    REQUIRE(stopped_count == 1);
    REQUIRE(stopped_process_id == first_process_id);

    output.clear();
    REQUIRE(process.start(
        {fixtureExecutable(), {QStringLiteral("emit"), QStringLiteral("restarted")}, {}}, &error));
    REQUIRE(process.waitForStarted(5000));
    REQUIRE(process.waitForFinished(5000));
    REQUIRE_FALSE(process.isRunning());
    REQUIRE(stopped_count == 2);
    REQUIRE(output.contains(QStringLiteral("stdout-capture:restarted")));
}

TEST_CASE("Engine process launch rejects a missing executable without starting",
          "[devstudio][viewport][process]") {
    EngineProcess process;
    QString error;
    REQUIRE_FALSE(process.start(
        {QStringLiteral("Z:/pelican/missing/pelican_player.exe"), {}, {}}, &error));
    REQUIRE_FALSE(error.isEmpty());
    REQUIRE(process.state() == EngineProcess::State::failed);
    REQUIRE_FALSE(process.isRunning());
}

} // namespace PelicanStudio
