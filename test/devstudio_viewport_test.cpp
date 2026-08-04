#include "embeddedviewport.hpp"
#include "enginelogbuffer.hpp"
#include "engineprocess.hpp"
#include "viewportgeometry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QProcess>
#include <QWidget>

#include <limits>
#include <memory>

#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace PelicanStudio {
namespace {

QString fixtureExecutable() {
    return QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH);
}

QString ownerFixtureExecutable() {
    return QString::fromUtf8(PELICAN_ENGINE_OWNER_FIXTURE_PATH);
}

#ifdef Q_OS_WIN

class RunningProcessGuard {
  public:
    explicit RunningProcessGuard(QProcess &process) : process_{process} {}
    ~RunningProcessGuard() {
        if (process_.state() != QProcess::NotRunning) {
            process_.kill();
            process_.waitForFinished(5000);
        }
    }

  private:
    QProcess &process_;
};

class NativeProcessGuard {
  public:
    explicit NativeProcessGuard(qint64 process_id) {
        if (process_id > 0 &&
            static_cast<quint64>(process_id) <= std::numeric_limits<DWORD>::max()) {
            handle_ = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE,
                                  static_cast<DWORD>(process_id));
        }
    }

    ~NativeProcessGuard() {
        if (handle_ == nullptr) {
            return;
        }
        if (WaitForSingleObject(handle_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(handle_, 255);
            WaitForSingleObject(handle_, 5000);
        }
        CloseHandle(handle_);
    }

    bool isValid() const noexcept { return handle_ != nullptr; }
    bool waitForFinished(DWORD timeout_ms) const noexcept {
        return handle_ != nullptr && WaitForSingleObject(handle_, timeout_ms) == WAIT_OBJECT_0;
    }

  private:
    HANDLE handle_ = nullptr;
};

#endif

} // namespace

TEST_CASE("Embedded viewport extent follows Qt device-pixel scaling", "[devstudio][viewport]") {
    REQUIRE(embeddedViewportPixelExtent(QSize{640, 360}, 1.0) == QSize{640, 360});
    REQUIRE(embeddedViewportPixelExtent(QSize{640, 360}, 1.5) == QSize{960, 540});
    REQUIRE(embeddedViewportPixelExtent(QSize{801, 451}, 1.25) == QSize{1001, 564});
    REQUIRE(embeddedViewportPixelExtent(QSize{0, 451}, 1.25) == QSize{0, 564});
    REQUIRE(embeddedViewportPixelExtent(QSize{-1, 451}, 1.25) == QSize{0, 564});
    REQUIRE(embeddedViewportPixelExtent(QSize{640, 360}, 0.0).isEmpty());
}

TEST_CASE("Engine log keeps a bounded UTF-8 tail on line boundaries",
          "[devstudio][log]") {
    EngineLogBuffer log{12};

    REQUIRE_FALSE(log.append(QStringLiteral("alpha\n")));
    REQUIRE_FALSE(log.append(QStringLiteral("beta\n")));
    REQUIRE(log.append(QStringLiteral("gamma\n")));
    REQUIRE(log.text() == QStringLiteral("beta\ngamma\n"));
    REQUIRE(log.text().toUtf8().size() <= log.maximumBytes());

    EngineLogBuffer multibyte_log{5};
    REQUIRE(multibyte_log.append(QString::fromUtf8("A\xc3\xa9\xe6\xbc\xa2")));
    REQUIRE(multibyte_log.text() == QString::fromUtf8("\xc3\xa9\xe6\xbc\xa2"));
    REQUIRE(multibyte_log.append(QStringLiteral("Z")));
    REQUIRE(multibyte_log.text() == QString::fromUtf8("\xe6\xbc\xa2Z"));
    REQUIRE(multibyte_log.text().toUtf8().size() <= multibyte_log.maximumBytes());

    EngineLogBuffer single_line_log{8};
    REQUIRE(single_line_log.append(QStringLiteral("1234567890")));
    REQUIRE(single_line_log.text() == QStringLiteral("34567890"));
}

TEST_CASE("Continuous embedded viewport resizes stay frozen and apply only the trailing extent",
          "[devstudio][viewport][resize]") {
    ViewportResizeCoalescer coalescer;
    coalescer.reset(QSize{640, 360});
    constexpr int InputEventCount = 200;
    int applied_count = 0;
    QSize applied_extent;

    for (int event_index = 0; event_index < InputEventCount; ++event_index) {
        const qint64 now_ms = event_index;
        const QSize extent{641 + event_index, 360};
        const ViewportResizeDecision decision =
            coalescer.request(extent, now_ms, ViewportExtentChangeKind::resize);
        if (decision.extent_to_apply) {
            ++applied_count;
            applied_extent = *decision.extent_to_apply;
        }
        REQUIRE(decision.next_wakeup_ms == EmbeddedViewportResizeDebounceMs);
    }

    REQUIRE(applied_count == 0);
    const qint64 last_event_time_ms = InputEventCount - 1;
    const ViewportResizeDecision early =
        coalescer.timerExpired(last_event_time_ms + EmbeddedViewportResizeDebounceMs - 1);
    REQUIRE_FALSE(early.extent_to_apply);
    REQUIRE(early.next_wakeup_ms == 1);

    const qint64 trailing_time_ms =
        last_event_time_ms + EmbeddedViewportResizeDebounceMs;
    const ViewportResizeDecision trailing = coalescer.timerExpired(trailing_time_ms);
    REQUIRE(trailing.extent_to_apply);
    ++applied_count;
    applied_extent = *trailing.extent_to_apply;
    REQUIRE_FALSE(trailing.next_wakeup_ms);

    REQUIRE(applied_count == 1);
    REQUIRE(applied_extent == QSize{640 + InputEventCount, 360});
    REQUIRE_FALSE(coalescer.timerExpired(trailing_time_ms + EmbeddedViewportResizeDebounceMs)
                      .extent_to_apply);
}

TEST_CASE("Device-pixel-ratio changes bypass viewport resize coalescing",
          "[devstudio][viewport][resize][dpi]") {
    ViewportResizeCoalescer coalescer;
    coalescer.reset(QSize{640, 360});

    const ViewportResizeDecision deferred =
        coalescer.request(QSize{650, 360}, 10, ViewportExtentChangeKind::resize);
    REQUIRE_FALSE(deferred.extent_to_apply);
    REQUIRE(deferred.next_wakeup_ms == EmbeddedViewportResizeDebounceMs);

    const ViewportResizeDecision dpr_change = coalescer.request(
        QSize{1300, 720}, 11, ViewportExtentChangeKind::device_pixel_ratio);
    REQUIRE(dpr_change.extent_to_apply == QSize{1300, 720});
    REQUIRE_FALSE(dpr_change.next_wakeup_ms);
    REQUIRE_FALSE(coalescer.timerExpired(50).extent_to_apply);
}

TEST_CASE("Native viewport surface uses the smallest live extent and clears exposed pixels",
          "[devstudio][viewport][minimum][paint]") {
    int argument_count = 1;
    char application_name[] = "pelican_viewport_surface_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};
    EmbeddedViewport viewport;

    QWidget *surface = viewport.findChild<QWidget *>(QStringLiteral("pelican.viewportHost"));
    REQUIRE(surface != nullptr);
    REQUIRE(surface->minimumSize() == QSize{1, 1});
    REQUIRE(viewport.minimumSizeHint().width() == 1);
    REQUIRE(surface->autoFillBackground());
    REQUIRE_FALSE(surface->testAttribute(Qt::WA_OpaquePaintEvent));
    REQUIRE_FALSE(surface->testAttribute(Qt::WA_NoSystemBackground));
    REQUIRE(surface->palette().color(QPalette::Window) == QColor{Qt::black});
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

TEST_CASE("Engine process forwards child stdout and stderr through its output signal",
          "[devstudio][viewport][process][log]") {
    EngineProcess process;
    QString output;
    QObject::connect(&process, &EngineProcess::outputReceived,
                     [&](const QString &text) { output += text; });

    QString error;
    REQUIRE(process.start(
        {fixtureExecutable(), {QStringLiteral("emit"), QStringLiteral("both-streams")}, {}},
        &error));
    REQUIRE(error.isEmpty());
    REQUIRE(process.waitForStarted(5000));
    REQUIRE(process.waitForFinished(5000));
    REQUIRE(output.contains(QStringLiteral("stdout-capture:both-streams")));
    REQUIRE(output.contains(QStringLiteral("stderr-capture:both-streams")));
}

#ifdef Q_OS_WIN

TEST_CASE("Normal engine process owner destruction terminates its child",
          "[devstudio][viewport][process][lifetime]") {
    std::unique_ptr<NativeProcessGuard> child;
    {
        EngineProcess process;
        QString error;
        REQUIRE(process.start({fixtureExecutable(), {QStringLiteral("hang")}, {}}, &error));
        REQUIRE(error.isEmpty());
        REQUIRE(process.waitForStarted(5000));
        child = std::make_unique<NativeProcessGuard>(process.processId());
        REQUIRE(child->isValid());
    }

    REQUIRE(child->waitForFinished(5000));
}

TEST_CASE("Abnormal engine process owner exit leaves no child process",
          "[devstudio][viewport][process][lifetime]") {
    QProcess owner;
    RunningProcessGuard owner_guard{owner};
    owner.setProgram(ownerFixtureExecutable());
    owner.setArguments({fixtureExecutable()});
    owner.start(QIODevice::ReadOnly);
    REQUIRE(owner.waitForStarted(5000));
    REQUIRE(owner.waitForReadyRead(5000));

    bool valid_process_id = false;
    const qint64 child_process_id = owner.readLine().trimmed().toLongLong(&valid_process_id);
    REQUIRE(valid_process_id);
    NativeProcessGuard child{child_process_id};
    REQUIRE(child.isValid());

    owner.kill();
    REQUIRE(owner.waitForFinished(5000));
    REQUIRE(owner.exitStatus() == QProcess::CrashExit);
    REQUIRE(child.waitForFinished(5000));
}

#endif

} // namespace PelicanStudio
