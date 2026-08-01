#include "embeddedviewport.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace PelicanStudio {
namespace {

constexpr int WindowDiscoveryIntervalMs = 25;
constexpr int WindowDiscoveryTimeoutMs = 15000;
constexpr int DiagnosticsIntervalMs = 200;
constexpr int GracefulShutdownTimeoutMs = 2000;

class NativeViewportSurface final : public QWidget {
  public:
    explicit NativeViewportSurface(QWidget *parent) : QWidget(parent) {
        setObjectName(QStringLiteral("pelican.viewportHost"));
        setAttribute(Qt::WA_NativeWindow);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(160, 90);
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
    const QString executable_name = playerExecutableName();
    const QStringList candidates{
        application_directory.absoluteFilePath(executable_name),
        application_directory.absoluteFilePath(
            QStringLiteral("../build/src/player/Debug/%1").arg(executable_name)),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QFileInfo(candidate).absoluteFilePath();
        }
    }
    return QFileInfo(candidates.back()).absoluteFilePath();
}

QString awarenessName(int awareness) {
    switch (awareness) {
    case 0: return QStringLiteral("unaware");
    case 1: return QStringLiteral("system");
    case 2: return QStringLiteral("per-monitor");
    default: return QStringLiteral("unknown");
    }
}

QString clippedOutput(QString output) {
    output = output.trimmed();
    constexpr qsizetype MaximumOutputCharacters = 4000;
    if (output.size() > MaximumOutputCharacters) {
        output = output.right(MaximumOutputCharacters);
    }
    return output;
}

} // namespace

EmbeddedViewport::EmbeddedViewport(QWidget *parent) : QWidget(parent), process_(this) {
    setObjectName(QStringLiteral("pelican.viewportPanel"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    auto *surface = new NativeViewportSurface(this);
    native_host_ = surface;
    surface->extent_changed = [this]() {
        requestEmbeddedWindowResize(ViewportExtentChangeKind::resize);
    };
    surface->device_pixel_ratio_changed = [this]() {
        requestEmbeddedWindowResize(ViewportExtentChangeKind::device_pixel_ratio);
    };
    surface->focus_requested = [this]() { focusEmbeddedWindow(); };
    layout->addWidget(native_host_, 1);

    auto *footer = new QWidget(this);
    auto *footer_layout = new QHBoxLayout(footer);
    footer_layout->setContentsMargins(4, 0, 4, 0);
    status_ = new QLabel(tr("Engine viewport is stopped."), footer);
    status_->setObjectName(QStringLiteral("pelican.viewportStatus"));
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer_layout->addWidget(status_, 1);

    restart_button_ = new QPushButton(tr("Start Engine"), footer);
    restart_button_->setObjectName(QStringLiteral("pelican.viewportRestart"));
    stop_button_ = new QPushButton(tr("Stop Engine"), footer);
    stop_button_->setObjectName(QStringLiteral("pelican.viewportStop"));
    stop_button_->setEnabled(false);
    footer_layout->addWidget(restart_button_);
    footer_layout->addWidget(stop_button_);
    layout->addWidget(footer);

    window_discovery_timer_ = new QTimer(this);
    window_discovery_timer_->setInterval(WindowDiscoveryIntervalMs);
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
        restart_button_->setEnabled(false);
        stop_button_->setEnabled(true);
        status_->setText(tr("Engine PID %1 started; waiting for its window...").arg(process_id));
        window_discovery_elapsed_.restart();
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
                child_window_ = 0;
                last_requested_extent_ = {};
                native_host_->update();
                restart_button_->setText(tr("Restart Engine"));
                restart_button_->setEnabled(true);
                stop_button_->setEnabled(false);
                const QString exit_kind = exit_status == QProcess::CrashExit ? tr("crashed") : tr("exited");
                status_->setText(
                    tr("Engine PID %1 %2 (code %3). Pelican Studio is still running.")
                        .arg(process_id)
                        .arg(exit_kind)
                        .arg(exit_code));
                if (!recent_output_.isEmpty()) {
                    status_->setToolTip(recent_output_);
                }
            });
    connect(&process_, &EngineProcess::processFailed, this, [this](const QString &message) {
        window_discovery_timer_->stop();
        diagnostics_timer_->stop();
        pointer_focus_timer_->stop();
        resize_timer_->stop();
        resize_coalescer_.reset();
        pointer_button_was_down_ = false;
        restart_button_->setText(tr("Retry Engine"));
        restart_button_->setEnabled(true);
        stop_button_->setEnabled(false);
        status_->setText(message);
    });
    connect(&process_, &EngineProcess::outputReceived, this, [this](const QString &output) {
        recent_output_ = clippedOutput(recent_output_ + output);
        status_->setToolTip(recent_output_);
    });

    QTimer::singleShot(0, this, [this]() { startEngine(); });
}

EmbeddedViewport::~EmbeddedViewport() {
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
    return {
        .program = program,
        .arguments = configured_arguments.isEmpty() ? QStringList{} : QProcess::splitCommand(configured_arguments),
        .working_directory = QFileInfo(program).absolutePath(),
    };
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

    child_window_ = 0;
    last_requested_extent_ = {};
    resize_timer_->stop();
    resize_coalescer_.reset();
    recent_output_.clear();
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
    if (child_window_ == 0 || !NativeWindowHost::requestClose(child_window_)) {
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

    const NativeWindowHandle window = NativeWindowHost::findTopLevelWindow(process_.processId());
    if (window == 0) {
        if (window_discovery_elapsed_.elapsed() >= WindowDiscoveryTimeoutMs) {
            window_discovery_timer_->stop();
            status_->setText(tr("The engine started, but no native window appeared within 15 seconds."));
            process_.kill();
        }
        return;
    }

    const NativeWindowHandle host = static_cast<NativeWindowHandle>(native_host_->winId());
    const QSize pixel_extent =
        embeddedViewportPixelExtent(native_host_->size(), native_host_->devicePixelRatioF());
    QString error;
    if (!NativeWindowHost::embed(window, host, pixel_extent, &error)) {
        window_discovery_timer_->stop();
        status_->setText(error);
        NativeWindowHost::requestClose(window);
        const qint64 stopping_process_id = process_.processId();
        QTimer::singleShot(GracefulShutdownTimeoutMs, this, [this, stopping_process_id]() {
            finishProcessShutdown(stopping_process_id);
        });
        return;
    }

    child_window_ = window;
    last_requested_extent_ = pixel_extent;
    resize_coalescer_.reset(pixel_extent, resize_elapsed_.elapsed());
    window_discovery_timer_->stop();
    diagnostics_timer_->start();
    pointer_button_was_down_ = false;
    pointer_focus_timer_->start();
    updateDiagnostics();
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
