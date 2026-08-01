#include "engineprocess.hpp"
#include "viewportgeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QProcess>

#include <vector>

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

TEST_CASE("Continuous embedded viewport resizes are bounded and retain the trailing extent",
          "[devstudio][viewport][resize]") {
    struct AppliedExtent {
        qint64 time_ms;
        QSize extent;
    };

    ViewportResizeCoalescer coalescer;
    std::vector<AppliedExtent> applied;
    std::optional<int> trailing_delay_ms;
    constexpr int InputEventCount = 200;

    for (int event_index = 0; event_index < InputEventCount; ++event_index) {
        const qint64 now_ms = event_index;
        const QSize extent{640 + event_index, 360};
        const ViewportResizeDecision decision =
            coalescer.request(extent, now_ms, ViewportExtentChangeKind::resize);
        if (decision.extent_to_apply) {
            applied.push_back({now_ms, *decision.extent_to_apply});
        }
        trailing_delay_ms = decision.next_wakeup_ms;
    }

    REQUIRE(trailing_delay_ms);
    const qint64 trailing_time_ms = InputEventCount - 1 + *trailing_delay_ms;
    const ViewportResizeDecision trailing = coalescer.timerExpired(trailing_time_ms);
    REQUIRE(trailing.extent_to_apply);
    applied.push_back({trailing_time_ms, *trailing.extent_to_apply});

    REQUIRE(applied.size() == 5);
    REQUIRE(applied.size() * 10 < InputEventCount);
    for (std::size_t index = 1; index < applied.size(); ++index) {
        REQUIRE(applied[index].time_ms - applied[index - 1].time_ms >=
                EmbeddedViewportResizeIntervalMs);
    }
    REQUIRE(applied.back().extent == QSize{640 + InputEventCount - 1, 360});
    REQUIRE_FALSE(coalescer.timerExpired(trailing_time_ms + EmbeddedViewportResizeIntervalMs)
                      .extent_to_apply);
}

TEST_CASE("Device-pixel-ratio changes bypass viewport resize coalescing",
          "[devstudio][viewport][resize][dpi]") {
    ViewportResizeCoalescer coalescer;
    REQUIRE(coalescer.request(QSize{640, 360}, 0, ViewportExtentChangeKind::resize)
                .extent_to_apply == QSize{640, 360});

    const ViewportResizeDecision deferred =
        coalescer.request(QSize{650, 360}, 10, ViewportExtentChangeKind::resize);
    REQUIRE_FALSE(deferred.extent_to_apply);
    REQUIRE(deferred.next_wakeup_ms == 40);

    const ViewportResizeDecision dpr_change = coalescer.request(
        QSize{1300, 720}, 11, ViewportExtentChangeKind::device_pixel_ratio);
    REQUIRE(dpr_change.extent_to_apply == QSize{1300, 720});
    REQUIRE_FALSE(dpr_change.next_wakeup_ms);
    REQUIRE_FALSE(coalescer.timerExpired(50).extent_to_apply);
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
