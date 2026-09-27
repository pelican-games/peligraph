#include "embeddedviewport.hpp"
#include "studioplayerarguments.hpp"
#include "../model/selection.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <utility>
#include <QVBoxLayout>

#include <functional>
#include <string>
#include <vector>

namespace PelicanStudio {
namespace {

constexpr int DiagnosticsIntervalMs = 200;
constexpr int GracefulShutdownTimeoutMs = 2000;
constexpr int MinimumLiveViewportExtent = 1;

class HorizontallyCollapsibleViewportLayout final : public QVBoxLayout {
  public:
    explicit HorizontallyCollapsibleViewportLayout(QWidget *parent)
        : QVBoxLayout(parent) {}

    QSize minimumSize() const override {
        QSize minimum = QVBoxLayout::minimumSize();
        // Ancillary rows may clip at extreme widths, but they must never move
        // the dock's opposite edge. Enforce that rule at the layout boundary
        // so present and future rows cannot restore a horizontal width floor.
        minimum.setWidth(MinimumLiveViewportExtent);
        return minimum;
    }
};

class NativeViewportSurface final : public QWidget {
  public:
    explicit NativeViewportSurface(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("pelican.viewportHost"));
        setAttribute(Qt::WA_NativeWindow);
        setFocusPolicy(Qt::StrongFocus);
        // Positive framebuffer sizes are clamped against Vulkan's
        // capabilities.minImageExtent by the player, and Win32 validation
        // reaches a live 1x1 swapchain. Zero remains the existing
        // minimized/suspended path, not a useful visible panel size. Keep the
        // Qt layout at its smallest positive extent instead of imposing an
        // unrelated 160x90 boundary on dock resizing.
        setMinimumSize(MinimumLiveViewportExtent, MinimumLiveViewportExtent);
        setAutoFillBackground(true);
        QPalette viewport_palette = palette();
        viewport_palette.setColor(QPalette::Window, Qt::black);
        setPalette(viewport_palette);
    }

    std::function<void()> extent_changed;
    std::function<void()> device_pixel_ratio_changed;
    std::function<void()> focus_requested;

  protected:
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if (extent_changed) {
            extent_changed();
        }
    }

    void focusInEvent(QFocusEvent *event) override {
        QWidget::focusInEvent(event);
        if (focus_requested) {
            focus_requested();
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        QWidget::mousePressEvent(event);
        if (focus_requested) {
            focus_requested();
        }
    }

    bool event(QEvent *event) override {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::DevicePixelRatioChange && device_pixel_ratio_changed) {
            device_pixel_ratio_changed();
        }
        return handled;
    }
};

QString playerExecutableName() {
#ifdef Q_OS_WIN
    return QStringLiteral("pelican_player.exe");
#else
    return QStringLiteral("pelican_player");
#endif
}

QString defaultPlayerPath() {
    const QString configured = qEnvironmentVariable("PELICAN_STUDIO_PLAYER").trimmed();
    if (!configured.isEmpty()) {
        return QFileInfo(configured).absoluteFilePath();
    }

    const QDir application_directory{QCoreApplication::applicationDirPath()};
    // Build and installed applications use the same directory. Falling back
    // to a different build may silently select another version of the engine.
    return application_directory.absoluteFilePath(playerExecutableName());
}

// How long an engine may load in silence before Studio stops it. The default
// is generous on purpose; the override exists for the machines and the
// projects that are slower still, and it is what makes the give-up path
// reachable in a test without waiting two minutes for it.
int configuredEngineLoadTimeoutMs() {
    bool valid = false;
    const int configured =
        qEnvironmentVariable("PELICAN_STUDIO_ENGINE_LOAD_TIMEOUT_MS")
            .trimmed()
            .toInt(&valid);
    return valid && configured > 0 ? configured : EngineLoadTimeoutMs;
}

QString awarenessName(int awareness) {
    switch (awareness) {
    case 0: return QStringLiteral("unaware");
    case 1: return QStringLiteral("system");
    case 2: return QStringLiteral("per-monitor");
    default: return QStringLiteral("unknown");
    }
}

} // namespace

QStringList studioPlayerArguments(QStringList configured,
                                  const QString &project_root) {
    std::vector<std::string> configured_strings;
    configured_strings.reserve(static_cast<std::size_t>(configured.size()));
    for (const auto &argument : configured) {
        const auto utf8 = argument.toUtf8();
        configured_strings.emplace_back(utf8.constData(),
                                        static_cast<std::size_t>(utf8.size()));
    }

    const auto project_utf8 = project_root.toUtf8();
    const auto generated = studioPlayerArgumentStrings(
        std::move(configured_strings),
        std::string{project_utf8.constData(),
                    static_cast<std::size_t>(project_utf8.size())});
    QStringList arguments;
    arguments.reserve(static_cast<qsizetype>(generated.size()));
    for (const auto &argument : generated) {
        arguments.push_back(QString::fromUtf8(
            argument.data(), static_cast<qsizetype>(argument.size())));
    }
    return arguments;
}

EmbeddedViewport::EmbeddedViewport(QWidget *parent) : QWidget(parent), process_(this) {
    setObjectName(QStringLiteral("pelican.viewportPanel"));

    auto *layout = new HorizontallyCollapsibleViewportLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *surface = new NativeViewportSurface(this);
    native_host_ = surface;
    surface->extent_changed = [this]() {
        layoutEngineLoadingNotice();
        requestEmbeddedWindowResize(ViewportExtentChangeKind::resize);
    };
    surface->device_pixel_ratio_changed = [this]() {
        requestEmbeddedWindowResize(ViewportExtentChangeKind::device_pixel_ratio);
    };
    surface->focus_requested = [this]() { focusEmbeddedWindow(); };
    layout->addWidget(native_host_, 1);

    // Q-4. A plain child of the surface rather than a row of the panel's
    // layout: the surface must stay collapsible to one pixel, and this notice
    // must sit where the engine's picture will be, not above or below it.
    loading_notice_ = new QLabel(native_host_);
    loading_notice_->setObjectName(
        QStringLiteral("pelican.viewportLoadingNotice"));
    loading_notice_->setAlignment(Qt::AlignCenter);
    loading_notice_->setWordWrap(true);
    loading_notice_->setStyleSheet(QStringLiteral("color: #d6d6d6;"));
    loading_notice_->hide();

    auto *footer = new QWidget(this);
    auto *footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(4, 0, 4, 0);
    status_ = new QLabel(tr("Engine viewport is stopped."), footer);
    status_->setObjectName(QStringLiteral("pelican.viewportStatus"));
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer_layout->addWidget(status_, 1);

    picking_notice_ = new QLabel(this);
    picking_notice_->setObjectName(QStringLiteral("pelican.pickingNotice"));
    picking_notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    picking_notice_->setStyleSheet(QStringLiteral("color: #d98c00;"));
    picking_notice_->hide();

    gizmo_notice_ = new QLabel(this);
    gizmo_notice_->setObjectName(QStringLiteral("pelican.gizmoNotice"));
    gizmo_notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    gizmo_notice_->setStyleSheet(QStringLiteral("color: #d98c00;"));
    gizmo_notice_->setWordWrap(true);
    gizmo_notice_->hide();

    restart_button_ = new QPushButton(tr("Start Engine"), footer);
    restart_button_->setObjectName(QStringLiteral("pelican.viewportRestart"));
    stop_button_ = new QPushButton(tr("Stop Engine"), footer);
    stop_button_->setObjectName(QStringLiteral("pelican.viewportStop"));
    stop_button_->setEnabled(false);
    footer_layout->addWidget(restart_button_);
    footer_layout->addWidget(stop_button_);
    layout->addWidget(footer);
    layout->addWidget(picking_notice_);
    layout->addWidget(gizmo_notice_);

    engine_load_timeout_ms_ = configuredEngineLoadTimeoutMs();
    attach_policy_ = EngineAttachPolicy{EngineWindowDiscoveryTimeoutMs,
                                        engine_load_timeout_ms_};
    window_discovery_timer_ = new QTimer(this);
    window_discovery_timer_->setInterval(EngineWindowSearchIntervalMs);
    diagnostics_timer_ = new QTimer(this);
    diagnostics_timer_->setInterval(DiagnosticsIntervalMs);
    pointer_focus_timer_ = new QTimer(this);
    pointer_focus_timer_->setInterval(5);
    pointer_focus_timer_->setTimerType(Qt::PreciseTimer);
    resize_timer_ = new QTimer(this);
    resize_timer_->setSingleShot(true);
    resize_timer_->setTimerType(Qt::PreciseTimer);
    resize_elapsed_.start();

    connect(restart_button_, &QPushButton::clicked, this, [this]() { startEngine(); });
    connect(stop_button_, &QPushButton::clicked, this, [this]() { stopEngine(); });
    connect(window_discovery_timer_, &QTimer::timeout, this, [this]() { discoverEngineWindow(); });
    connect(diagnostics_timer_, &QTimer::timeout, this, [this]() { updateDiagnostics(); });
    connect(pointer_focus_timer_, &QTimer::timeout, this, [this]() { pollPointerFocus(); });
    connect(resize_timer_, &QTimer::timeout, this, [this]() {
        applyEmbeddedWindowResizeDecision(resize_coalescer_.timerExpired(resize_elapsed_.elapsed()));
    });
    connect(&process_, &EngineProcess::processStarted, this, [this](qint64 process_id) {
        engine_failure_model_.beginRun();
        rpc_ready_ = false;
        restart_button_->setEnabled(false);
        stop_button_->setEnabled(true);
        status_->setText(tr("Engine PID %1 started; waiting for its window...").arg(process_id));
        engine_clock_.restart();
        attach_policy_.engineStarted(0);
        window_discovery_timer_->setInterval(EngineWindowSearchIntervalMs);
        window_discovery_timer_->start();
        discoverEngineWindow();
    });
    connect(&process_, &EngineProcess::processStopped, this,
            [this](qint64 process_id, int exit_code, QProcess::ExitStatus exit_status) {
                window_discovery_timer_->stop();
                diagnostics_timer_->stop();
                pointer_focus_timer_->stop();
                resize_timer_->stop();
                resize_coalescer_.reset();
                pointer_button_was_down_ = false;
                primary_pointer_was_down_ = false;
                primary_pointer_owned_ = false;
                last_primary_pointer_position_.reset();
                setEngineInputActive(false);
                rpc_ready_ = false;
                restart_pending_ = false;
                readiness_request_ = 0;
                child_window_ = 0;
                setEngineLoadingNotice({});
                last_requested_extent_ = {};
                native_host_->update();
                restart_button_->setText(tr("Restart Engine"));
                restart_button_->setEnabled(true);
                stop_button_->setEnabled(false);
                const QString exit_kind = exit_status == QProcess::CrashExit ? tr("crashed") : tr("exited");
                // When Studio stopped this engine on purpose, the reason it
                // stopped it is the thing worth reading. The exit code of a
                // process Studio killed says only that Studio killed it.
                status_->setText(
                    engine_stop_reason_.isEmpty()
                        ? tr("Engine PID %1 %2 (code %3). Pelican Studio is "
                             "still running.")
                              .arg(process_id)
                              .arg(exit_kind)
                              .arg(exit_code)
                        : engine_stop_reason_);
                const auto failure =
                    engine_failure_model_.finishRun(exit_code);
                // G-4: a restart Studio asked for said so when it began.
                const bool announced =
                    std::exchange(restart_unavailable_announced_, false);
                if (!shutting_down_) {
                    if (!announced) {
                        emit engineRpcBecameUnavailable(
                            tr("The engine viewport process stopped."));
                    }
                    if (failure) {
                        emit engineProcessExitedWithFailure(
                            failure->fatal_error_line);
                    }
                }
                if (restart_after_stop_ && !shutting_down_) {
                    restart_after_stop_ = false;
                    QTimer::singleShot(0, this,
                                       [this]() { startEngine(); });
                }
            });
    connect(&process_, &EngineProcess::processFailed, this, [this](const QString &message) {
        window_discovery_timer_->stop();
        diagnostics_timer_->stop();
        pointer_focus_timer_->stop();
        resize_timer_->stop();
        resize_coalescer_.reset();
        pointer_button_was_down_ = false;
        primary_pointer_was_down_ = false;
        primary_pointer_owned_ = false;
        last_primary_pointer_position_.reset();
        setEngineInputActive(false);
        rpc_ready_ = false;
        restart_pending_ = false;
        readiness_request_ = 0;
        setEngineLoadingNotice({});
        restart_button_->setText(tr("Retry Engine"));
        restart_button_->setEnabled(true);
        stop_button_->setEnabled(false);
        status_->setText(message);
        if (!shutting_down_) {
            emit engineRpcBecameUnavailable(message);
        }
    });
    connect(&process_, &EngineProcess::outputReceived,
            this, &EmbeddedViewport::engineOutputReceived);
    // An engine that is writing its log is an engine that is loading, not one
    // that has stopped answering. Every line it writes pushes both give-up
    // deadlines out, so neither of them can end a slow project's startup.
    connect(&process_, &EngineProcess::outputReceived, this,
            [this](const QString &) {
                attach_policy_.engineSpoke(engine_clock_.isValid()
                                               ? engine_clock_.elapsed()
                                               : 0);
            });
    connect(&process_, &EngineProcess::standardErrorReceived, this,
            [this](const QString &output) {
                engine_failure_model_.appendStandardError(output);
                emit engineStandardErrorReceived(output);
            });
    // MG-1. A player whose project needs migrating says so on stderr and then
    // serves the migration RPCs with no window: it is not an engine that is
    // slow to show one, and it is not given up on for that.
    connect(&process_, &EngineProcess::standardErrorReceived, this,
            [this](const QString &) {
                const QString &refusal =
                    engine_failure_model_.migrationRequiredLine();
                if (migration_gate_ || refusal.isEmpty() ||
                    !process_.isRunning()) {
                    return;
                }
                migration_gate_ = true;
                window_discovery_timer_->stop();
                setEngineLoadingNotice({});
                status_->setText(tr("The project needs migrating before the "
                                    "engine can open it."));
                emit projectMigrationRequired(refusal);
            });
    connect(&process_, &EngineProcess::processStopped, this,
            [this] { migration_gate_ = false; });
    connect(&process_, &EngineProcess::processFailed, this,
            [this] { migration_gate_ = false; });
    connect(&process_, &EngineProcess::rpcResultReceived, this,
            [this](qint64 request_id, const QByteArray &result_json) {
                // Q-4: the readiness probe is Studio's own question and is
                // nobody else's answer to receive.
                if (request_id != 0 && request_id == readiness_request_) {
                    // PM-3a: the probe is a get_status, and what it says of
                    // the play state is known before anyone is told "ready".
                    emit engineStatusRead(result_json);
                    markRpcReady();
                    return;
                }
                if (status_requests_.remove(request_id)) {
                    emit engineStatusRead(result_json);
                }
                // Passed straight through. The reply was walked once by
                // EngineProcess and is parsed once by whichever model wants
                // its contents; nothing in between takes a copy of it as a
                // document.
                if (!pending_pick_requests_.remove(request_id)) {
                    emit inspectorRpcSucceeded(request_id, result_json);
                    return;
                }
                emit pickObjectSucceeded(request_id, result_json);
            });
    connect(&process_, &EngineProcess::rpcErrorReceived, this,
            [this](qint64 request_id, int code, const QString &message,
                   const QJsonValue &data) {
                // An engine that refuses the probe still answered it: what the
                // readiness probe establishes is that the RPC loop is running,
                // not that this particular method exists.
                if (request_id != 0 && request_id == readiness_request_) {
                    markRpcReady();
                    return;
                }
                status_requests_.remove(request_id);
                if (!pending_pick_requests_.remove(request_id)) {
                    emit engineRpcRefused(request_id, code, message, data);
                    // F-5: the engine's own words and error_code first, for
                    // a caller that tells one refusal from another; then the
                    // one-line failure every caller already reads.
                    emit inspectorRpcRejected(
                        request_id,
                        data.toObject()
                            .value(QStringLiteral("error_code"))
                            .toString(),
                        message);
                    emit inspectorRpcFailed(
                        request_id,
                        tr("RPC %1: %2").arg(code).arg(message));
                    return;
                }
                emit pickObjectFailed(
                    request_id,
                    tr("pick_object failed (RPC %1): %2")
                        .arg(code)
                        .arg(message));
            });
    connect(&process_, &EngineProcess::rpcTransportFailed, this,
            [this](qint64 request_id, const QString &message) {
                if (request_id != 0 && request_id == readiness_request_) {
                    // The transport, not the engine, failed. There is nothing
                    // left to be ready for; the process signals will say so.
                    readiness_request_ = 0;
                    return;
                }
                status_requests_.remove(request_id);
                if (!pending_pick_requests_.remove(request_id)) {
                    emit inspectorRpcFailed(request_id, message);
                    return;
                }
                emit pickObjectFailed(request_id, message);
            });

    restart_button_->setEnabled(false);
}

EmbeddedViewport::~EmbeddedViewport() {
    shutting_down_ = true;
    restart_after_stop_ = false;
    // Waiting for QProcess below can synchronously drain output and fail
    // pending RPCs. Our owning window's members may already be destroyed
    // while QWidget tears down this child, so none of those callbacks may
    // update the viewport or forward signals to its owner during teardown.
    disconnect(&process_, nullptr, this, nullptr);
    window_discovery_timer_->stop();
    diagnostics_timer_->stop();
    pointer_focus_timer_->stop();
    resize_timer_->stop();
    if (!process_.isRunning()) {
        return;
    }

    if (child_window_ != 0) {
        NativeWindowHost::requestClose(child_window_);
    }
    if (!process_.waitForFinished(1500)) {
        process_.kill();
        process_.waitForFinished(1500);
    }
}

EngineProcessLaunch EmbeddedViewport::launchCommand() const {
    const QString program = defaultPlayerPath();
    const QString configured_arguments = qEnvironmentVariable("PELICAN_STUDIO_PLAYER_ARGUMENTS");
    // The Studio deliberately launches the interactive windowed loop. That
    // loop calls updateFrameState() every frame, so an edit accepted while RPC
    // requests are serviced is applied by the next player frame. Adding a
    // headless/render-only driver here would strand the editor commit queue.
    return {
        .program = program,
        .arguments = studioPlayerArguments(
            configured_arguments.isEmpty()
                ? QStringList{}
                : QProcess::splitCommand(configured_arguments),
            project_root_),
        .working_directory = QFileInfo(program).absolutePath(),
    };
}

void EmbeddedViewport::openProject(const QString &project_root) {
    if (project_root.trimmed().isEmpty()) {
        setPickingNotice(tr("Picking is unavailable: no project is open."));
        return;
    }
    const QString normalized_root = QDir(project_root).absolutePath();
    if (project_root_ == normalized_root && process_.isRunning()) {
        return;
    }

    project_root_ = normalized_root;
    setPickingNotice({});
    if (process_.isRunning()) {
        restart_after_stop_ = true;
        status_->setText(tr("Switching the engine to %1...").arg(project_root_));
        stopEngine();
        return;
    }
    restart_after_stop_ = false;
    startEngine();
}

void EmbeddedViewport::restartEngine(const QString &reason) {
    if (project_root_.isEmpty()) {
        return;
    }
    if (!process_.isRunning()) {
        restart_after_stop_ = false;
        startEngine();
        return;
    }
    // Nothing may be sent to an engine that is going away: the gate closes
    // now. The stop itself waits for the event loop, because the caller is
    // usually answering one of this engine's replies.
    const bool was_ready = rpc_ready_;
    rpc_ready_ = false;
    restart_pending_ = false;
    readiness_request_ = 0;
    restart_after_stop_ = true;
    engine_stop_reason_ = reason;
    status_->setText(reason);
    QTimer::singleShot(0, this, [this, reason, was_ready]() {
        if (!restart_after_stop_ || !process_.isRunning()) {
            return;
        }
        if (was_ready) {
            restart_unavailable_announced_ = true;
            emit engineRpcBecameUnavailable(reason);
        }
        stopEngine();
        status_->setText(reason);
    });
}

const OutlinerObjectKey *EmbeddedViewport::selectedObject() const noexcept {
    if (selection_model_ == nullptr || !selection_model_->selected()) {
        return nullptr;
    }
    return &*selection_model_->selected();
}

qint64 EmbeddedViewport::pickObject(const QPoint &pixel_position,
                                    QString *error) {
    if (pixel_position.x() < 0 || pixel_position.y() < 0) {
        if (error != nullptr) {
            *error = tr("Picking coordinates must be non-negative.");
        }
        return 0;
    }
    if (child_window_ == 0 || !NativeWindowHost::isWindow(child_window_)) {
        if (error != nullptr) {
            *error = tr("The engine viewport is not ready for picking.");
        }
        return 0;
    }

    const qint64 request_id = process_.requestRpc(
        QStringLiteral("pick_object"),
        QJsonObject{
            {QStringLiteral("x"), pixel_position.x()},
            {QStringLiteral("y"), pixel_position.y()},
        },
        error);
    if (request_id != 0) {
        pending_pick_requests_.insert(request_id);
    }
    return request_id;
}

qint64 EmbeddedViewport::requestRpc(const QString &method,
                                    const QJsonObject &params,
                                    QString *error) {
    if (!rpc_ready_) {
        if (error != nullptr) {
            *error = tr("The engine viewport is not ready for editor RPC requests.");
        }
        return 0;
    }
    if (rpc_observer_.before) rpc_observer_.before(method, params);
    const qint64 request_id = process_.requestRpc(method, params, error);
    if (request_id != 0 && method == QLatin1String("get_status")) {
        status_requests_.insert(request_id);
    }
    if (rpc_observer_.sent) rpc_observer_.sent(request_id, method, params);
    return request_id;
}

qint64 EmbeddedViewport::requestMigrationRpc(const QString &method,
                                             const QJsonObject &params,
                                             QString *error) {
    if (method != QStringLiteral("plan_project_migration") &&
        method != QStringLiteral("apply_project_migration")) {
        if (error != nullptr) {
            *error = tr("%1 is not a migration request.").arg(method);
        }
        return 0;
    }
    // Only the player that stopped at the gate: it started no engine, so
    // nothing else in this process holds the documents while apply rewrites
    // them (MG-1b). A running engine answers these methods too, and is never
    // asked: its project is migrated by starting it again, through the gate.
    if (!migration_gate_) {
        if (error != nullptr) {
            *error = rpc_ready_
                         ? tr("The engine has this project open; it is "
                              "migrated only by a player that started no "
                              "engine.")
                         : tr("The engine is not ready for the migration "
                              "yet.");
        }
        return 0;
    }
    return process_.requestRpc(method, params, error);
}

void EmbeddedViewport::closeProject(const QString &reason) {
    project_root_.clear();
    restart_after_stop_ = false;
    setPickingNotice({});
    if (!process_.isRunning()) {
        status_->setText(reason);
        restart_button_->setEnabled(false);
        return;
    }
    engine_stop_reason_ = reason;
    stopEngine();
}

void EmbeddedViewport::setPickingNotice(const QString &message) {
    picking_notice_->setText(message);
    picking_notice_->setVisible(!message.isEmpty());
}

void EmbeddedViewport::startEngine() {
    if (!NativeWindowHost::isSupported()) {
        status_->setText(tr("Native engine viewport embedding is currently available only on Windows."));
        restart_button_->setEnabled(false);
        stop_button_->setEnabled(false);
        return;
    }
    if (process_.isRunning()) {
        return;
    }
    if (project_root_.isEmpty()) {
        status_->setText(tr("Open a project to start the engine viewport."));
        restart_button_->setEnabled(false);
        stop_button_->setEnabled(false);
        return;
    }

    child_window_ = 0;
    rpc_ready_ = false;
    restart_pending_ = false;
    readiness_request_ = 0;
    engine_stop_reason_.clear();
    status_requests_.clear();
    setEngineLoadingNotice({});
    last_requested_extent_ = {};
    resize_timer_->stop();
    resize_coalescer_.reset();
    primary_pointer_was_down_ = false;
    primary_pointer_owned_ = false;
    last_primary_pointer_position_.reset();
    setPickingNotice({});
    restart_button_->setEnabled(false);
    stop_button_->setEnabled(true);
    status_->setText(tr("Starting pelican_player..."));

    QString error;
    if (!process_.start(launchCommand(), &error)) {
        status_->setText(error);
        restart_button_->setText(tr("Retry Engine"));
        restart_button_->setEnabled(true);
        stop_button_->setEnabled(false);
    }
}

void EmbeddedViewport::stopEngine() {
    if (!process_.isRunning()) {
        return;
    }

    const qint64 stopping_process_id = process_.processId();
    status_->setText(tr("Stopping engine..."));
    stop_button_->setEnabled(false);
    if (migration_gate_) {
        // MG-1: the migration server has no window to close; it ends with
        // its input.
        process_.closeInput();
    } else if (child_window_ == 0 ||
               !NativeWindowHost::requestClose(child_window_)) {
        process_.terminate();
    }
    QTimer::singleShot(GracefulShutdownTimeoutMs, this, [this, stopping_process_id]() {
        finishProcessShutdown(stopping_process_id);
    });
}

void EmbeddedViewport::discoverEngineWindow() {
    if (!process_.isRunning()) {
        window_discovery_timer_->stop();
        return;
    }

    const qint64 now_ms = engine_clock_.isValid() ? engine_clock_.elapsed() : 0;
    const NativeWindowHandle window =
        NativeWindowHost::findTopLevelWindow(process_.processId());
    const EngineAttachDecision decision =
        attach_policy_.decide(window != 0, now_ms);
    if (decision.next_interval_ms > 0 &&
        decision.next_interval_ms != window_discovery_timer_->interval()) {
        window_discovery_timer_->setInterval(decision.next_interval_ms);
    }

    switch (decision.step) {
    case EngineAttachStep::search_window:
        return;
    case EngineAttachStep::give_up_no_window:
        abandonEngine(tr("The engine started but showed no window, and wrote "
                         "nothing for %1 seconds. It was stopped.")
                          .arg(decision.silent_ms / 1000));
        return;
    case EngineAttachStep::give_up_no_response:
        abandonEngine(tr("The engine did not answer for %1 seconds. "
                         "It was stopped.")
                          .arg(decision.silent_ms / 1000));
        return;
    case EngineAttachStep::probe_window:
        break;
    }

    // The window is up; its thread is still inside the project load. Say so,
    // and ask again on the next tick. Re-parenting it now would send
    // WM_STYLECHANGING to that thread and wait for it -- which is exactly the
    // freeze this is here to avoid.
    setEngineLoadingNotice(
        tr("Loading %1...\nThe engine is still starting (%2 s).")
            .arg(QDir(project_root_).dirName())
            .arg(decision.waiting_ms / 1000));
    status_->setText(tr("Waiting for the engine to finish starting (%1 s)...")
                         .arg(decision.waiting_ms / 1000));
    if (!NativeWindowHost::windowThreadAnswers(window,
                                               EngineWindowProbeTimeoutMs)) {
        return;
    }
    attachEngineWindow(window);
}

void EmbeddedViewport::attachEngineWindow(NativeWindowHandle window) {
    const NativeWindowHandle host = static_cast<NativeWindowHandle>(native_host_->winId());
    const QSize pixel_extent =
        embeddedViewportPixelExtent(native_host_->size(), native_host_->devicePixelRatioF());
    QString error;
    if (!NativeWindowHost::embed(window, host, pixel_extent, &error)) {
        window_discovery_timer_->stop();
        setEngineLoadingNotice({});
        status_->setText(error);
        engine_stop_reason_ = error;
        NativeWindowHost::requestClose(window);
        const qint64 stopping_process_id = process_.processId();
        QTimer::singleShot(GracefulShutdownTimeoutMs, this, [this, stopping_process_id]() {
            finishProcessShutdown(stopping_process_id);
        });
        return;
    }

    child_window_ = window;
    last_requested_extent_ = pixel_extent;
    resize_coalescer_.reset(pixel_extent);
    window_discovery_timer_->stop();
    setEngineLoadingNotice({});
    diagnostics_timer_->start();
    pointer_button_was_down_ = false;
    primary_pointer_was_down_ = false;
    primary_pointer_owned_ = false;
    last_primary_pointer_position_.reset();
    pointer_focus_timer_->start();
    // Q-4. The window moving is not the engine answering. Ask it one question
    // and let its reply be what makes Studio call the engine ready, so that
    // "ready" means the same thing it says: editor RPC will be served.
    readiness_request_ =
        process_.requestRpc(QStringLiteral("get_status"), QJsonObject{});
    updateDiagnostics();
}

void EmbeddedViewport::markRpcReady() {
    readiness_request_ = 0;
    if (rpc_ready_) {
        return;
    }
    rpc_ready_ = true;
    emit engineRpcBecameAvailable();
}

void EmbeddedViewport::abandonEngine(const QString &reason) {
    window_discovery_timer_->stop();
    setEngineLoadingNotice({});
    engine_stop_reason_ = reason;
    status_->setText(reason);
    process_.kill();
}

void EmbeddedViewport::setEngineLoadingNotice(const QString &message) {
    if (loading_notice_ == nullptr) {
        return;
    }
    if (message.isEmpty()) {
        loading_notice_->clear();
        loading_notice_->hide();
        return;
    }
    if (loading_notice_->text() == message && loading_notice_->isVisible()) {
        return;
    }
    loading_notice_->setText(message);
    layoutEngineLoadingNotice();
    loading_notice_->show();
    loading_notice_->raise();
}

void EmbeddedViewport::layoutEngineLoadingNotice() {
    if (loading_notice_ == nullptr || native_host_ == nullptr) {
        return;
    }
    loading_notice_->setGeometry(native_host_->rect());
}

void EmbeddedViewport::requestEmbeddedWindowResize(ViewportExtentChangeKind kind) {
    if (child_window_ == 0 || !NativeWindowHost::isWindow(child_window_)) {
        if (resize_timer_ != nullptr) {
            resize_timer_->stop();
        }
        resize_coalescer_.reset();
        return;
    }

    const QSize pixel_extent =
        embeddedViewportPixelExtent(native_host_->size(), native_host_->devicePixelRatioF());
    applyEmbeddedWindowResizeDecision(
        resize_coalescer_.request(pixel_extent, resize_elapsed_.elapsed(), kind));
}

void EmbeddedViewport::applyEmbeddedWindowResizeDecision(
    const ViewportResizeDecision &decision) {
    if (decision.extent_to_apply) {
        resizeEmbeddedWindow(*decision.extent_to_apply);
    }

    if (decision.next_wakeup_ms) {
        resize_timer_->start(*decision.next_wakeup_ms);
    } else {
        resize_timer_->stop();
    }
}

void EmbeddedViewport::resizeEmbeddedWindow(const QSize &pixel_extent) {
    if (child_window_ == 0 || !NativeWindowHost::isWindow(child_window_)) {
        return;
    }

    if (pixel_extent == last_requested_extent_) {
        return;
    }

    QString error;
    if (!NativeWindowHost::resize(child_window_, pixel_extent, &error)) {
        status_->setText(error);
        return;
    }
    last_requested_extent_ = pixel_extent;
}

void EmbeddedViewport::focusEmbeddedWindow() {
    if (child_window_ != 0) {
        NativeWindowHost::focus(child_window_);
    }
}

void EmbeddedViewport::pollPointerFocus() {
    const bool pointer_button_down =
        child_window_ != 0 && NativeWindowHost::pointerButtonDownOver(child_window_);
    if (pointer_button_down && !pointer_button_was_down_) {
        focusEmbeddedWindow();
    }
    pointer_button_was_down_ = pointer_button_down;

    const NativePrimaryPointerState primary_pointer =
        NativeWindowHost::primaryPointerState(child_window_);
    if (primary_pointer.button_down && !primary_pointer_was_down_) {
        primary_pointer_owned_ =
            primary_pointer.over_child &&
            primary_pointer.child_client_position.has_value();
        if (primary_pointer_owned_) {
            last_primary_pointer_position_ =
                primary_pointer.child_client_position;
            emit viewportPointerPressed(*last_primary_pointer_position_);
        }
    } else if (primary_pointer.button_down && primary_pointer_owned_ &&
               primary_pointer.child_client_position) {
        if (last_primary_pointer_position_ !=
            primary_pointer.child_client_position) {
            last_primary_pointer_position_ =
                primary_pointer.child_client_position;
            emit viewportPointerMoved(*last_primary_pointer_position_);
        }
    } else if (!primary_pointer.button_down && primary_pointer_was_down_ &&
               primary_pointer_owned_) {
        const auto released_position =
            primary_pointer.child_client_position
                ? primary_pointer.child_client_position
                : last_primary_pointer_position_;
        if (released_position) {
            emit viewportPointerReleased(*released_position);
        }
        primary_pointer_owned_ = false;
        last_primary_pointer_position_.reset();
    }
    primary_pointer_was_down_ = primary_pointer.button_down;

    // A modal transform begins with a key, and a key goes to whatever holds
    // the keyboard focus -- so the engine window holding it is exactly the
    // condition under which one can begin. Hovering is not enough, and a drag
    // that the gizmo claimed is a gesture, which is tracked separately.
    // Reported as a change, not as a reading: the 5 ms poll must not turn into
    // 200 signals a second.
    setEngineInputActive(child_window_ != 0 && primary_pointer.has_focus);
}

void EmbeddedViewport::setEngineInputActive(bool active) {
    if (engine_input_active_ == active) return;
    engine_input_active_ = active;
    emit engineInputActiveChanged(active);
}

void EmbeddedViewport::setGizmoNotice(const QString &message) {
    gizmo_notice_->setText(message);
    gizmo_notice_->setVisible(!message.isEmpty());
}

void EmbeddedViewport::updateDiagnostics() {
    if (child_window_ == 0) {
        return;
    }

    const NativeViewportDiagnostics diagnostic = NativeWindowHost::diagnostics(
        child_window_, static_cast<NativeWindowHandle>(native_host_->winId()));
    if (!diagnostic.embedded) {
        status_->setText(tr("The engine window is no longer attached to the viewport host."));
        return;
    }

    status_->setText(
        tr("PID %1 | host %2x%3 px, engine %4x%5 px | DPI %6/%7 (%8/%9) | focus %10 | z %11")
            .arg(process_.processId())
            .arg(diagnostic.host_client_extent.width())
            .arg(diagnostic.host_client_extent.height())
            .arg(diagnostic.child_client_extent.width())
            .arg(diagnostic.child_client_extent.height())
            .arg(diagnostic.host_dpi)
            .arg(diagnostic.child_dpi)
            .arg(awarenessName(diagnostic.host_dpi_awareness))
            .arg(awarenessName(diagnostic.child_dpi_awareness))
            .arg(diagnostic.child_has_focus ? tr("engine") : tr("editor"))
            .arg(diagnostic.child_is_top_sibling && diagnostic.child_style ? tr("top-child")
                                                                           : tr("unexpected")));
}

void EmbeddedViewport::finishProcessShutdown(qint64 expected_process_id) {
    if (process_.isRunning() && process_.processId() == expected_process_id) {
        process_.kill();
    }
}

} // namespace PelicanStudio
