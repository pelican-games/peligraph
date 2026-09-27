#include "automationcommands.hpp"
#include "automationserver.hpp"
#include "embeddedviewport.hpp"
#include "engineattach.hpp"
#include "enginelogbuffer.hpp"
#include "engineprocess.hpp"
#include "nativewindowhost.hpp"
#include "viewportgeometry.hpp"
#include "../src/core/os/enginewindowproperty.hpp"
#include "../src/project/renderfeatureoverlay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLayout>
#include <QPalette>
#include <QProcess>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <limits>
#include <memory>
#include <vector>

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

TEST_CASE("Studio player arguments force public editor feature and input overlays",
          "[devstudio][viewport][feature-overlay][input-overlay][negative-contrast][wp289][wp286]") {
    const auto arguments = studioPlayerArguments(
        {QStringLiteral("--rpc"), QStringLiteral("--project"),
         QStringLiteral("stale-project"),
         QStringLiteral("--feature-overlay"),
         QStringLiteral("project://stale-overlay.json"),
         QStringLiteral("--frames"), QStringLiteral("5")},
        QStringLiteral("C:/projects/example"));
    const auto expected_overlay = QString::fromUtf8(
        Pelican::editorFeatureOverlayReference.data(),
        static_cast<qsizetype>(
            Pelican::editorFeatureOverlayReference.size()));

    REQUIRE(arguments ==
            QStringList{QStringLiteral("--rpc"),
                        QStringLiteral("--project"),
                        QStringLiteral("C:/projects/example"),
                        QStringLiteral("--feature-overlay"), expected_overlay,
                        QStringLiteral("--embedded"),
                        QStringLiteral("--editor-transform"),
                        QStringLiteral("--log-file"),
                        QStringLiteral("C:/projects/example/.pelican/logs/player.log"),
                        // PM-3b: the engine waits in edit until Play.
                        QStringLiteral("--start-state"), QStringLiteral("edit"),
                        QStringLiteral("--frames"), QStringLiteral("5")});
    // A configured log file is the user's choice and is not replaced.
    const auto own_log = studioPlayerArguments(
        {QStringLiteral("--log-file"), QStringLiteral("D:/logs/mine.log")},
        QStringLiteral("C:/projects/example"));
    REQUIRE(own_log.count(QStringLiteral("--log-file")) == 1);
    REQUIRE(own_log.contains(QStringLiteral("D:/logs/mine.log")));
    REQUIRE(arguments.count(QStringLiteral("--feature-overlay")) == 1);
    REQUIRE(arguments.count(QStringLiteral("--editor-transform")) == 1);

    const auto handle_only = studioPlayerArguments(
        {QStringLiteral("--editor-transform=grab")},
        QStringLiteral("C:/projects/example"));
    REQUIRE(handle_only.contains(
        QStringLiteral("--editor-transform=grab")));
    REQUIRE_FALSE(handle_only.contains(
        QStringLiteral("--editor-transform")));
    REQUIRE(handle_only != arguments);
}

// EW-2. The engine Studio starts creates its window hidden (--embedded), so
// nothing appears outside Studio while the project loads. A configured
// --headless run has no window, and the engine refuses the two together.
TEST_CASE("EW-2: Studio starts the engine --embedded, unless it was told to "
          "run it headless",
          "[devstudio][viewport][ew2]") {
    const QString project = QStringLiteral("C:/projects/example");
    const auto plain = studioPlayerArguments({}, project);
    REQUIRE(plain.count(QStringLiteral("--embedded")) == 1);

    const auto headless = studioPlayerArguments(
        {QStringLiteral("--headless"), QStringLiteral("--frames"),
         QStringLiteral("0")},
        project);
    REQUIRE(headless.contains(QStringLiteral("--headless")));
    REQUIRE_FALSE(headless.contains(QStringLiteral("--embedded")));

    // Asked for already: passed on once, not twice.
    const auto configured = studioPlayerArguments(
        {QStringLiteral("--embedded")}, project);
    REQUIRE(configured.count(QStringLiteral("--embedded")) == 1);
}

#ifdef Q_OS_WIN
namespace {

class FixtureWindows {
  public:
    FixtureWindows() {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(WNDCLASSEXW);
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = className();
        registered_ = RegisterClassExW(&window_class) != 0;
    }
    ~FixtureWindows() {
        for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
            DestroyWindow(*it);
        }
        if (registered_) {
            UnregisterClassW(className(), GetModuleHandleW(nullptr));
        }
    }
    bool registered() const noexcept { return registered_; }

    // Off every monitor: these are facts about HWNDs, not something to flash
    // at whoever is at the desktop.
    HWND create(HWND owner, bool marked, bool visible) {
        HWND window = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, className(),
            L"pelican ew2 fixture", WS_OVERLAPPEDWINDOW, -32000, -32000, 64,
            64, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (window == nullptr) {
            return nullptr;
        }
        windows_.push_back(window);
        if (marked) {
            SetPropW(window, Pelican::engineRenderWindowProperty,
                     reinterpret_cast<HANDLE>(1));
        }
        if (visible) {
            ShowWindow(window, SW_SHOWNOACTIVATE);
        }
        return window;
    }

  private:
    static const wchar_t *className() noexcept {
        return L"PelicanEw2WindowSearchFixture";
    }
    bool registered_ = false;
    std::vector<HWND> windows_;
};

} // namespace

// EW-2. What Studio embeds is the process's top-level, ownerless window that
// carries the engine's mark (EW-1) -- visible or not, since an --embedded
// engine's window is hidden until Studio shows it.
TEST_CASE("EW-2: the engine window is found by its mark, not by being visible",
          "[devstudio][viewport][ew2][attach]") {
    FixtureWindows fixture;
    REQUIRE(fixture.registered());
    const HWND marked_hidden =
        fixture.create(nullptr, /*marked=*/true, /*visible=*/false);
    const HWND unmarked_visible =
        fixture.create(nullptr, /*marked=*/false, /*visible=*/true);
    const HWND owned_marked =
        fixture.create(unmarked_visible, /*marked=*/true, /*visible=*/true);
    REQUIRE(marked_hidden != nullptr);
    REQUIRE(unmarked_visible != nullptr);
    REQUIRE(owned_marked != nullptr);
    REQUIRE_FALSE(IsWindowVisible(marked_hidden));
    REQUIRE(IsWindowVisible(unmarked_visible));
    // The two wrong answers come first in EnumWindows' (z-)order, so a search
    // that stops at the first visible window, or the first window at all,
    // returns one of them.
    for (const HWND ahead : {unmarked_visible, owned_marked}) {
        SetWindowPos(ahead, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    const auto process_id = static_cast<qint64>(GetCurrentProcessId());
    REQUIRE(NativeWindowHost::findTopLevelWindow(process_id) ==
            reinterpret_cast<NativeWindowHandle>(marked_hidden));

    // Without the mark there is nothing to embed yet: a visible window and an
    // owned one are not the engine's.
    RemovePropW(marked_hidden, Pelican::engineRenderWindowProperty);
    REQUIRE(NativeWindowHost::findTopLevelWindow(process_id) == 0);
}
#endif

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

TEST_CASE("Embedded viewport stays horizontally collapsible with visible layout rows",
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

    SECTION("the picking notice is visible") {
        viewport.setPickingNotice(
            QStringLiteral("Object picking is unavailable for this project."));
        QApplication::processEvents();

        QWidget *notice = viewport.findChild<QWidget *>(
            QStringLiteral("pelican.pickingNotice"));
        REQUIRE(notice != nullptr);
        REQUIRE_FALSE(notice->isHidden());
        REQUIRE(viewport.minimumSizeHint().width() == 1);
    }

    SECTION("a future text row is added") {
        auto *future_row = new QLabel(
            QStringLiteral("A future viewport message must not restore a width floor."),
            &viewport);
        viewport.layout()->addWidget(future_row);
        future_row->show();
        QApplication::processEvents();

        REQUIRE(viewport.minimumSizeHint().width() == 1);
    }
}

// Q-4. The rules for waiting, with time supplied rather than spent. Every
// number the panel shows and every reason it has to stop an engine comes from
// this one object, so this is where the "never kill an engine that is still
// loading" rule is either kept or broken.
TEST_CASE("The attach policy waits for a window, then for its thread, and "
          "gives up only on silence",
          "[devstudio][viewport][q4][attach]") {
    SECTION("no window, and an engine that has stopped saying anything") {
        EngineAttachPolicy policy{15000, 120000};
        policy.engineStarted(0);
        REQUIRE(policy.decide(false, 14999).step ==
                EngineAttachStep::search_window);
        REQUIRE(policy.decide(false, 14999).next_interval_ms ==
                EngineWindowSearchIntervalMs);
        REQUIRE(policy.decide(false, 15000).step ==
                EngineAttachStep::give_up_no_window);
        REQUIRE(policy.decide(false, 15000).next_interval_ms == 0);
    }

    SECTION("the control: an engine writing its log is starting, not stuck") {
        EngineAttachPolicy policy{15000, 120000};
        policy.engineStarted(0);
        // One line at a time, all the way past the budget. `example` in a
        // Debug build needs more than fifteen seconds to reach the point where
        // its window becomes visible, and it logs the whole way.
        for (qint64 now = 1000; now <= 60000; now += 1000) {
            policy.engineSpoke(now);
            REQUIRE(policy.decide(false, now + 999).step ==
                    EngineAttachStep::search_window);
        }
        // And the moment it does stop, the budget starts from there.
        REQUIRE(policy.decide(false, 60000 + 14999).step ==
                EngineAttachStep::search_window);
        REQUIRE(policy.decide(false, 60000 + 15000).step ==
                EngineAttachStep::give_up_no_window);
    }

    SECTION("a window that is there but not yet answering") {
        EngineAttachPolicy policy{15000, 5000};
        policy.engineStarted(0);
        REQUIRE_FALSE(policy.windowSeen());
        const auto first = policy.decide(true, 200);
        REQUIRE(first.step == EngineAttachStep::probe_window);
        REQUIRE(policy.windowSeen());
        REQUIRE(first.waiting_ms == 0);
        // Eager at first, so a small project embeds promptly; then backed off,
        // so a long load does not pay for a probe every 25 ms.
        REQUIRE(first.next_interval_ms == EngineWindowProbeIntervalMs);
        REQUIRE(policy.decide(true, 200 + EngineWindowProbeEagerMs)
                    .next_interval_ms == EngineWindowProbeBackoffMs);
        const auto later = policy.decide(true, 4200);
        REQUIRE(later.step == EngineAttachStep::probe_window);
        REQUIRE(later.waiting_ms == 4000);
        // Silence since the window appeared, past the load budget.
        const auto given_up = policy.decide(true, 5200);
        REQUIRE(given_up.step == EngineAttachStep::give_up_no_response);
        REQUIRE(given_up.silent_ms == 5000);
        REQUIRE(given_up.next_interval_ms == 0);
    }

    SECTION("the control: a window that is silent while its engine is not") {
        EngineAttachPolicy policy{15000, 5000};
        policy.engineStarted(0);
        REQUIRE(policy.decide(true, 200).step ==
                EngineAttachStep::probe_window);
        for (qint64 now = 1200; now <= 30000; now += 1000) {
            policy.engineSpoke(now);
            REQUIRE(policy.decide(true, now + 999).step ==
                    EngineAttachStep::probe_window);
        }
        // The waiting figure the panel shows is still measured from the
        // window, not from the last line.
        REQUIRE(policy.decide(true, 30200).waiting_ms == 30000);
        REQUIRE(policy.decide(true, 35000).step ==
                EngineAttachStep::give_up_no_response);
    }
}

#ifdef Q_OS_WIN

namespace {

// Pump Qt until the question answers yes, or until the time is up. Returns how
// long it took, or -1: the widget tests below are about *when* things happen,
// so the time is part of the answer.
qint64 pumpUntil(const std::function<bool()> &ready, int timeout_ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout_ms) {
        if (ready()) return elapsed.elapsed();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return ready() ? elapsed.elapsed() : -1;
}

QLabel *namedLabel(EmbeddedViewport &viewport, const char *name) {
    return viewport.findChild<QLabel *>(QString::fromLatin1(name));
}

class EngineFixtureEnvironment {
  public:
    EngineFixtureEnvironment(int pump_delay_ms, int heartbeat_ms,
                             int rpc_delay_ms, int load_timeout_ms) {
        qputenv("PELICAN_STUDIO_PLAYER", fixtureExecutable().toUtf8());
        qputenv("PELICAN_WP173_CHILD_MODE", "viewport-window");
        qputenv("PELICAN_Q4_PUMP_DELAY_MS",
                QByteArray::number(pump_delay_ms));
        qputenv("PELICAN_Q4_HEARTBEAT_MS", QByteArray::number(heartbeat_ms));
        qputenv("PELICAN_Q4_RPC_DELAY_MS", QByteArray::number(rpc_delay_ms));
        qputenv("PELICAN_STUDIO_ENGINE_LOAD_TIMEOUT_MS",
                QByteArray::number(load_timeout_ms));
        qunsetenv("PELICAN_STUDIO_PLAYER_ARGUMENTS");
    }

    ~EngineFixtureEnvironment() {
        qunsetenv("PELICAN_STUDIO_PLAYER");
        qunsetenv("PELICAN_WP173_CHILD_MODE");
        qunsetenv("PELICAN_Q4_PUMP_DELAY_MS");
        qunsetenv("PELICAN_Q4_HEARTBEAT_MS");
        qunsetenv("PELICAN_Q4_RPC_DELAY_MS");
        qunsetenv("PELICAN_STUDIO_ENGINE_LOAD_TIMEOUT_MS");
    }
};

} // namespace

// Q-4. The freeze this work is about, end to end. The fixture engine puts up a
// real top-level window in a few milliseconds and then takes no message at all
// for a second and a half -- exactly what pelican_player does while it loads a
// project. Re-parenting it during that time is what used to hold Studio's UI
// thread for the whole of the remaining load (2.2 s on `playground_pose`,
// 17 s on `example`, in a Debug build).
TEST_CASE("The engine window is embedded only once its own thread takes "
          "messages, and the panel says so meanwhile",
          "[devstudio][viewport][q4][attach][process]") {
    EngineFixtureEnvironment fixture{/*pump_delay_ms=*/1500,
                                     /*heartbeat_ms=*/0,
                                     /*rpc_delay_ms=*/700,
                                     /*load_timeout_ms=*/60000};
    int argument_count = 1;
    char application_name[] = "pelican_viewport_attach_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

    EmbeddedViewport viewport;
    viewport.resize(320, 200);
    QLabel *loading = namedLabel(viewport, "pelican.viewportLoadingNotice");
    QLabel *status = namedLabel(viewport, "pelican.viewportStatus");
    REQUIRE(loading != nullptr);
    REQUIRE(status != nullptr);
    REQUIRE(loading->isHidden());

    viewport.openProject(QDir::tempPath());
    // The window appears almost at once, and the panel starts saying that the
    // engine is still starting.
    REQUIRE(pumpUntil([&] { return !loading->isHidden(); }, 10000) >= 0);
    REQUIRE(loading->text().contains(QStringLiteral("engine")));
    REQUIRE(status->text().contains(QStringLiteral("Waiting for the engine")));
    // Nothing has been re-parented: this is the whole point.
    REQUIRE_FALSE(viewport.engineWindowEmbedded());

    QElapsedTimer since_window;
    since_window.start();
    // And it stays that way while the fixture is "loading". A mutation that
    // drops the wait embeds here instead, in the first tick.
    REQUIRE(pumpUntil([&] { return viewport.engineWindowEmbedded(); }, 700) < 0);
    REQUIRE_FALSE(loading->isHidden());

    const qint64 embedded_ms =
        pumpUntil([&] { return viewport.engineWindowEmbedded(); }, 20000);
    REQUIRE(embedded_ms >= 0);
    REQUIRE(since_window.elapsed() >= 700);
    // The notice goes away with the thing it was standing in for.
    REQUIRE(loading->isHidden());

    // The window moving is not the engine answering: the fixture waits another
    // 700 ms before it will reply to anything, and Studio calls itself ready
    // only when it has a reply.
    REQUIRE_FALSE(viewport.rpcReady());
    REQUIRE(pumpUntil([&] { return viewport.rpcReady(); }, 20000) >= 0);
}

TEST_CASE("Viewport destruction does not forward pending engine RPC callbacks",
          "[devstudio][viewport][process][rpc][shutdown]") {
    EngineFixtureEnvironment fixture{/*pump_delay_ms=*/0,
                                     /*heartbeat_ms=*/0,
                                     /*rpc_delay_ms=*/0,
                                     /*load_timeout_ms=*/60000};
    int argument_count = 1;
    char application_name[] = "pelican_viewport_shutdown_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};
    auto viewport = std::make_unique<EmbeddedViewport>();
    viewport->resize(320, 200);
    QString output;
    int replies = 0;
    int failures = 0;
    QObject::connect(viewport.get(), &EmbeddedViewport::engineOutputReceived,
                     &application, [&](const QString &text) { output += text; });
    QObject::connect(viewport.get(), &EmbeddedViewport::inspectorRpcSucceeded,
                     &application, [&](qint64, const QByteArray &) { ++replies; });
    QObject::connect(viewport.get(), &EmbeddedViewport::inspectorRpcFailed,
                     &application, [&](qint64, const QString &) { ++failures; });
    viewport->openProject(QDir::tempPath());
    REQUIRE(pumpUntil([&] { return viewport->rpcReady(); }, 20000) >= 0);

    // Live replies still reach the owner. The next request is deliberately
    // left pending in the child until the destructor closes its native window.
    REQUIRE(viewport->requestRpc(QStringLiteral("get_status"), {}) != 0);
    REQUIRE(pumpUntil([&] { return replies == 1; }, 10000) >= 0);
    REQUIRE(viewport->requestRpc(QStringLiteral("wait_for_shutdown"), {}) != 0);
    REQUIRE(pumpUntil([&] { return output.contains(QStringLiteral("fixture-rpc-held")); },
                      10000) >= 0);
    REQUIRE(failures == 0);
    viewport.reset();
    QCoreApplication::processEvents();
    CHECK(replies == 1);
    CHECK(failures == 0);
}

// The other half of the rule: an engine that never answers is stopped, but
// only when it has also stopped writing its log.
TEST_CASE("An engine that goes silent is stopped, and one that is still "
          "logging is waited for",
          "[devstudio][viewport][q4][attach][process]") {
    int argument_count = 1;
    char application_name[] = "pelican_viewport_silence_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

    SECTION("silent past the load budget: stopped, and told why") {
        EngineFixtureEnvironment fixture{/*pump_delay_ms=*/600000,
                                         /*heartbeat_ms=*/0,
                                         /*rpc_delay_ms=*/0,
                                         /*load_timeout_ms=*/800};
        EmbeddedViewport viewport;
        viewport.resize(320, 200);
        QLabel *loading = namedLabel(viewport, "pelican.viewportLoadingNotice");
        QLabel *status = namedLabel(viewport, "pelican.viewportStatus");
        REQUIRE(loading != nullptr);
        REQUIRE(status != nullptr);

        viewport.openProject(QDir::tempPath());
        REQUIRE(pumpUntil([&] { return !loading->isHidden(); }, 10000) >= 0);
        REQUIRE(pumpUntil(
                    [&] {
                        return status->text().contains(
                            QStringLiteral("did not answer"));
                    },
                    15000) >= 0);
        REQUIRE_FALSE(viewport.engineWindowEmbedded());
        // The exit notice does not overwrite the reason with an exit code.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
        REQUIRE(status->text().contains(QStringLiteral("did not answer")));
        REQUIRE(loading->isHidden());
    }

    SECTION("the control: the same silence, with the log still moving") {
        EngineFixtureEnvironment fixture{/*pump_delay_ms=*/600000,
                                         /*heartbeat_ms=*/100,
                                         /*rpc_delay_ms=*/0,
                                         /*load_timeout_ms=*/800};
        EmbeddedViewport viewport;
        viewport.resize(320, 200);
        QLabel *loading = namedLabel(viewport, "pelican.viewportLoadingNotice");
        QLabel *status = namedLabel(viewport, "pelican.viewportStatus");
        REQUIRE(loading != nullptr);
        REQUIRE(status != nullptr);

        viewport.openProject(QDir::tempPath());
        REQUIRE(pumpUntil([&] { return !loading->isHidden(); }, 10000) >= 0);
        // Four times the budget later it is still being waited for.
        REQUIRE(pumpUntil(
                    [&] {
                        return status->text().contains(
                            QStringLiteral("did not answer"));
                    },
                    3500) < 0);
        REQUIRE_FALSE(loading->isHidden());
        REQUIRE(status->text().contains(QStringLiteral("Waiting for the engine")));
    }
}

// Q-9. The race behind `engine_not_ready` right after a G-4 restart. The
// window asks the running engine about unsaved edits before it restarts it
// (G-5), so for one round trip after the commit the old engine still answers
// -- and `wait_engine` used to agree with it, and the next step found it gone.
// Here the same `wait_engine` the automation pipe runs is asked across a
// restart of a fixture engine that waits 700 ms before its first answer.
TEST_CASE("Q-9: once a restart is asked for, wait_engine waits for the new "
          "engine's first answer, not the old engine's",
          "[devstudio][viewport][q9][restart][process]") {
    EngineFixtureEnvironment fixture{/*pump_delay_ms=*/0,
                                     /*heartbeat_ms=*/0,
                                     /*rpc_delay_ms=*/700,
                                     /*load_timeout_ms=*/60000};
    int argument_count = 1;
    char application_name[] = "pelican_viewport_restart_ready_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

    EmbeddedViewport viewport;
    viewport.resize(320, 200);
    AutomationHost host;
    host.viewport = &viewport;
    const auto waitEngine = [&](int timeout_ms) {
        return runAutomationCommand(
            host, QJsonObject{{QStringLiteral("cmd"), QStringLiteral("wait_engine")},
                              {QStringLiteral("timeout_ms"), timeout_ms}});
    };
    // Each run of the fixture says this once, when its window is up and
    // before it waits to answer. The second one is the restarted engine.
    int engines_started = 0;
    QElapsedTimer since_second_engine;
    QObject::connect(&viewport, &EmbeddedViewport::engineOutputReceived,
                     &application, [&](const QString &output) {
                         if (!output.contains(QStringLiteral("viewport-window-ready")))
                             return;
                         if (++engines_started == 2) since_second_engine.start();
                     });

    viewport.openProject(QDir::tempPath());
    const auto first = waitEngine(30000);
    REQUIRE(first.ok);
    REQUIRE(viewport.rpcReady());
    REQUIRE(engines_started == 1);

    // The commit answered restart_required and the window is asking the old
    // engine about unsaved edits. It still answers -- that question goes to
    // it -- but it is not the engine anyone waiting for "ready" wants.
    viewport.setRestartPending(true);
    CHECK(viewport.rpcReady());
    CHECK_FALSE(viewport.currentEngineReady());
    const auto during_question = waitEngine(300);
    CHECK_FALSE(during_question.ok);
    CHECK(during_question.code == QStringLiteral("automation.engine_not_ready@1"));

    // The restart itself: not ready from the first instant, and still not
    // ready while the old engine goes and the new one loads.
    viewport.restartEngine(QStringLiteral("Q-9 restart"));
    CHECK_FALSE(viewport.rpcReady());
    CHECK_FALSE(viewport.currentEngineReady());
    CHECK_FALSE(viewport.restartPending());

    // Ready again only from the new process's answer to its readiness probe:
    // the second engine is up, and 700 ms have passed since it said so.
    const auto second = waitEngine(30000);
    REQUIRE(second.ok);
    CHECK(engines_started == 2);
    CHECK(since_second_engine.isValid());
    CHECK(since_second_engine.elapsed() >= 500);
    CHECK(viewport.engineWindowEmbedded());

    // A restart asked for and then declined (Cancel on the unsaved-edits
    // question) leaves the engine that is running ready again.
    viewport.setRestartPending(true);
    CHECK_FALSE(waitEngine(100).ok);
    viewport.setRestartPending(false);
    CHECK(waitEngine(100).ok);
}

// MG-1 (GUI). A player whose project needs migrating writes the refusal on
// stderr, starts no engine and shows no window, and serves the two migration
// methods until its input ends. The fixture does exactly that.
TEST_CASE("MG-1 a player stopped at migration_required is named, served for "
          "the migration only, and closed through its input",
          "[devstudio][viewport][mg1][migration][process]") {
    EngineFixtureEnvironment fixture{/*pump_delay_ms=*/0,
                                     /*heartbeat_ms=*/0,
                                     /*rpc_delay_ms=*/0,
                                     /*load_timeout_ms=*/60000};
    qputenv("PELICAN_WP173_CHILD_MODE", "migration-gate");
    int argument_count = 1;
    char application_name[] = "pelican_viewport_migration_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

    EmbeddedViewport viewport;
    viewport.resize(320, 200);
    QLabel *status = namedLabel(viewport, "pelican.viewportStatus");
    REQUIRE(status != nullptr);
    QStringList refusals;
    QString standard_error;
    QHash<qint64, QByteArray> results;
    QObject::connect(&viewport, &EmbeddedViewport::projectMigrationRequired,
                     &application,
                     [&](const QString &refusal) { refusals.push_back(refusal); });
    QObject::connect(&viewport, &EmbeddedViewport::engineStandardErrorReceived,
                     &application,
                     [&](const QString &text) { standard_error += text; });
    QObject::connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded,
                     &application, [&](qint64 id, const QByteArray &json) {
                         results.insert(id, json);
                     });

    viewport.openProject(QDir::tempPath());
    REQUIRE(pumpUntil([&] { return !refusals.isEmpty(); }, 10000) >= 0);
    REQUIRE(refusals.size() == 1);
    REQUIRE(refusals.front().startsWith(
        QStringLiteral("project.migration_required@1: {\"documents\"")));
    REQUIRE(viewport.migrationGateOpen());
    REQUIRE(status->text().contains(QStringLiteral("migrating")));

    // Editor requests stay closed: this player is no engine.
    REQUIRE_FALSE(viewport.rpcReady());
    QString error;
    REQUIRE(viewport.requestRpc(QStringLiteral("get_status"), {}, &error) == 0);
    REQUIRE(viewport.requestMigrationRpc(QStringLiteral("get_status"), {},
                                         &error) == 0);
    // The migration ones are served.
    const qint64 plan = viewport.requestMigrationRpc(
        QStringLiteral("plan_project_migration"), {}, &error);
    REQUIRE(plan != 0);
    REQUIRE(pumpUntil([&] { return results.contains(plan); }, 10000) >= 0);
    REQUIRE(QJsonDocument::fromJson(results.value(plan))
                .object()
                .value(QStringLiteral("required"))
                .toBool());

    // Not opening the project ends the player through its input -- it has no
    // window to close -- and nothing is started again.
    viewport.closeProject(QStringLiteral("The project was not opened."));
    REQUIRE(pumpUntil(
                [&] {
                    return standard_error.contains(
                        QStringLiteral("migration-gate-input-closed"));
                },
                10000) >= 0);
    REQUIRE(pumpUntil([&] { return !viewport.migrationGateOpen(); }, 10000) >= 0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    REQUIRE(status->text() == QStringLiteral("The project was not opened."));
    REQUIRE(refusals.size() == 1);
}

// MG-1b. Recovery inside an apply can roll back an edit another process is
// committing, so a migration is applied only by the player that started no
// engine. An engine that has the project open is never asked, even though it
// answers the method: it is stopped, and the project comes back through the
// gate before anything is applied.
TEST_CASE("MG-1b a migration is never sent to a running engine, only to the "
          "player that started none after it",
          "[devstudio][viewport][mg1][migration][process]") {
    EngineFixtureEnvironment fixture{/*pump_delay_ms=*/0,
                                     /*heartbeat_ms=*/0,
                                     /*rpc_delay_ms=*/0,
                                     /*load_timeout_ms=*/60000};
    int argument_count = 1;
    char application_name[] = "pelican_viewport_migration_order_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

    EmbeddedViewport viewport;
    viewport.resize(320, 200);
    QStringList refusals;
    QHash<qint64, QByteArray> results;
    QObject::connect(&viewport, &EmbeddedViewport::projectMigrationRequired,
                     &application,
                     [&](const QString &refusal) { refusals.push_back(refusal); });
    QObject::connect(&viewport, &EmbeddedViewport::inspectorRpcSucceeded,
                     &application, [&](qint64 id, const QByteArray &json) {
                         results.insert(id, json);
                     });
    const QJsonObject apply_params{
        {QStringLiteral("expected"),
         QJsonObject{{QStringLiteral("project.json"),
                      QStringLiteral("sha256:00")}}}};

    // An engine with the project open, ready for editor requests.
    viewport.openProject(QDir::tempPath());
    REQUIRE(pumpUntil([&] { return viewport.rpcReady(); }, 20000) >= 0);
    QString error;
    REQUIRE(viewport.requestMigrationRpc(
                QStringLiteral("apply_project_migration"), apply_params,
                &error) == 0);
    REQUIRE(error.contains(QStringLiteral("has this project open")));
    REQUIRE(viewport.requestMigrationRpc(
                QStringLiteral("plan_project_migration"), {}, &error) == 0);

    // Stopped first; the next player stops at the gate, and is the one asked.
    qputenv("PELICAN_WP173_CHILD_MODE", "migration-gate");
    viewport.restartEngine(QStringLiteral("migrating"));
    REQUIRE(pumpUntil([&] { return !refusals.isEmpty(); }, 20000) >= 0);
    REQUIRE(viewport.migrationGateOpen());
    REQUIRE_FALSE(viewport.rpcReady());
    const qint64 apply = viewport.requestMigrationRpc(
        QStringLiteral("apply_project_migration"), apply_params, &error);
    REQUIRE(apply != 0);
    REQUIRE(pumpUntil([&] { return results.contains(apply); }, 10000) >= 0);
    REQUIRE(QJsonDocument::fromJson(results.value(apply))
                .object()
                .value(QStringLiteral("applied"))
                .toBool());
}

#endif

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

TEST_CASE("Engine process multiplexes JSON-RPC replies without swallowing ordinary output",
          "[devstudio][viewport][rpc][wp264]") {
    int argument_count = 1;
    char application_name[] = "pelican_engine_process_rpc_test";
    char *arguments[] = {application_name};
    QCoreApplication application{argument_count, arguments};

    EngineProcess process;
    QString output;
    QByteArray rpc_result;
    bool response_received = false;
    QEventLoop response_loop;
    QObject::connect(&process, &EngineProcess::outputReceived,
                     [&](const QString &text) { output += text; });
    QObject::connect(
        &process, &EngineProcess::rpcResultReceived,
        [&](qint64 request_id, const QByteArray &result_json) {
            REQUIRE(request_id == 1);
            rpc_result = result_json;
            response_received = true;
            response_loop.quit();
        });

    QString error;
    REQUIRE(process.start(
        {fixtureExecutable(), {QStringLiteral("rpc")}, {}}, &error));
    REQUIRE(error.isEmpty());
    REQUIRE(process.waitForStarted(5000));
    REQUIRE(process.requestRpc(
                QStringLiteral("pick_object"),
                QJsonObject{{QStringLiteral("x"), 12},
                            {QStringLiteral("y"), 34}},
                &error) == 1);
    REQUIRE(error.isEmpty());

    QTimer::singleShot(5000, &response_loop, &QEventLoop::quit);
    response_loop.exec();
    REQUIRE(response_received);
    // Q-2: the reply travels as the bytes the engine wrote. Byte equality
    // with the fixture's own `result` member is what says nobody parsed it
    // into a document and serialized it back out on the way here -- a
    // re-serialized object would still carry the same meaning, but not the
    // same bytes.
    const QByteArray expected_pick_result{
        R"json({"contract":1,"hit":null})json"};
    REQUIRE(rpc_result == expected_pick_result);
    const QJsonDocument parsed_once = QJsonDocument::fromJson(rpc_result);
    REQUIRE(parsed_once.isObject());
    REQUIRE(parsed_once.object().value(QStringLiteral("contract")).toInt() ==
            1);
    REQUIRE(parsed_once.object().value(QStringLiteral("hit")).isNull());
    REQUIRE(process.waitForFinished(5000));
    REQUIRE(output.contains(QStringLiteral("ordinary-stdout-before-rpc")));
    REQUIRE(output.contains(QStringLiteral("ordinary-stderr-before-rpc")));
    REQUIRE(output.contains(QStringLiteral("\"method\":\"pick_object\"")));
    REQUIRE(output.contains(QStringLiteral("\"x\":12")));
    REQUIRE_FALSE(output.contains(QStringLiteral("\"jsonrpc\":\"2.0\",\"id\":1,\"result\"")));
}

// Q-2. `get_frame_plan` answers with 128-181 KB, and that reply used to be
// parsed into a QJsonDocument here, serialized back to bytes in the viewport,
// and parsed a third time by the model. The line is now walked once and the
// `result` member travels as the bytes the engine wrote. This is the test that
// the walk is a real one: braces and brackets inside strings, an escaped
// quote, whitespace around every token, members in an order nobody promised,
// and a line that begins like a reply but does not close -- which must stay
// ordinary output rather than be routed as a result.
TEST_CASE("Engine process hands on the reply's result bytes without parsing them",
          "[devstudio][viewport][process][rpc][q2]") {
    int argument_count = 1;
    char application_name[] = "pelican_engine_process_envelope_test";
    char *arguments[] = {application_name};
    QCoreApplication application{argument_count, arguments};

    EngineProcess process;
    QString output;
    QByteArray routed_result;
    bool result_received = false;
    int error_code = 0;
    QString error_message;
    bool error_received = false;
    QEventLoop loop;
    QObject::connect(&process, &EngineProcess::outputReceived,
                     [&](const QString &text) { output += text; });
    QObject::connect(&process, &EngineProcess::rpcResultReceived,
                     [&](qint64 request_id, const QByteArray &result_json) {
                         REQUIRE(request_id == 1);
                         routed_result = result_json;
                         result_received = true;
                     });
    QObject::connect(&process, &EngineProcess::rpcErrorReceived,
                     [&](qint64 request_id, int code, const QString &message,
                         const QJsonValue &) {
                         REQUIRE(request_id == 2);
                         error_code = code;
                         error_message = message;
                         error_received = true;
                         loop.quit();
                     });

    QString error;
    REQUIRE(process.start(
        {fixtureExecutable(), {QStringLiteral("rpc-envelope")}, {}}, &error));
    REQUIRE(error.isEmpty());
    REQUIRE(process.waitForStarted(5000));
    REQUIRE(process.requestRpc(QStringLiteral("get_frame_plan"),
                               QJsonObject{}, &error) == 1);
    REQUIRE(process.requestRpc(QStringLiteral("get_status"), QJsonObject{},
                               &error) == 2);
    REQUIRE(error.isEmpty());

    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();

    REQUIRE(result_received);
    // Byte equality with what the fixture wrote. A document round trip would
    // reorder the members and rewrite the number; these are the same bytes.
    // (Kept out of the assertion macro: MSVC's preprocessor cannot stringize
    // a raw string literal that contains an escape.)
    const QByteArray expected_result{
        R"json({"text":"a\"b{}[],:","nested":[{"}":"]"}],"n":1.5,"b":true})json"};
    REQUIRE(routed_result == expected_result);

    REQUIRE(error_received);
    REQUIRE(error_code == -32001);
    REQUIRE(error_message == QStringLiteral("envelope probe"));
    // The unbalanced line was never a reply. It reached the log instead, and
    // it did not consume request 2 -- the error that followed it did.
    const QString unbalanced_line{
        QStringLiteral(R"json("result":{"a":[1,2})json")};
    REQUIRE(output.contains(unbalanced_line));

    process.terminate();
    (void)process.waitForFinished(5000);
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
