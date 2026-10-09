#pragma once

#include "engineattach.hpp"
#include "enginefailuremodel.hpp"
#include "engineprocess.hpp"
#include "nativewindowhost.hpp"
#include "viewportgeometry.hpp"

#include <QElapsedTimer>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>

class QLabel;
class QPushButton;
class QTimer;

namespace PelicanStudio {

class SelectionModel;
struct OutlinerObjectKey;

QStringList studioPlayerArguments(QStringList configured,
                                  const QString &project_root);
// PM-3b: why the engine does not start in edit, when the configured arguments
// keep it from doing so; empty otherwise.
QString studioPlayerStartStateNotice(QStringList configured,
                                     const QString &project_root);

class EmbeddedViewport final : public QWidget {
    Q_OBJECT

  public:
    explicit EmbeddedViewport(QWidget *parent = nullptr);
    ~EmbeddedViewport() override;

    void openProject(const QString &project_root);
    // G-4. Stops the running engine and starts it again on the same project,
    // for a committed setting the engine reads only at start. The editor gate
    // closes at once (engineRpcBecameUnavailable carries `reason`) and opens
    // again when the new engine answers its readiness probe.
    void restartEngine(const QString &reason);
    void bindSelectionModel(const SelectionModel *selection_model) noexcept {
        selection_model_ = selection_model;
    }
    const OutlinerObjectKey *selectedObject() const noexcept;
    qint64 pickObject(const QPoint &pixel_position,
                      QString *error = nullptr);
    qint64 requestRpc(const QString &method, const QJsonObject &params,
                      QString *error = nullptr);
    // RU-1. Told about every editor request around its sending: `before`
    // runs before the request is written (a request it sends itself lands
    // ahead of it on the wire), `sent` after, with the id its reply comes
    // back under (0: it was not sent). The render edit history rides here,
    // so every panel's edit is seen without any panel knowing.
    struct RpcObserver {
        std::function<void(const QString &method, const QJsonObject &params)> before;
        std::function<void(qint64 id, const QString &method,
                           const QJsonObject &params)>
            sent;
    };
    void setRpcObserver(RpcObserver observer) { rpc_observer_ = std::move(observer); }
    bool rpcReady() const noexcept { return rpc_ready_; }
    // Q-9. A restart has been asked for and has not begun: G-5 is still asking
    // whether the scene has unsaved edits, and that question -- and a Save --
    // go to the engine running now, so rpcReady() stays true. That engine is
    // the one about to be stopped, though, and a caller waiting for "the
    // engine is ready" is waiting for the one after it. restartEngine, a new
    // start and a stop each end the hold; the window ends it on a decline.
    void setRestartPending(bool pending) noexcept { restart_pending_ = pending; }
    bool restartPending() const noexcept { return restart_pending_; }
    // Q-9. The engine running now has started, embedded, and answered its
    // readiness probe, and no restart is waiting to replace it. Not true
    // again after a restart until the new process has answered.
    bool currentEngineReady() const noexcept {
        return rpc_ready_ && !restart_pending_;
    }
    // MG-1. The player stopped opening the project at
    // project.migration_required@1 (projectMigrationRequired): it runs no
    // engine and shows no window, and serves plan_project_migration and
    // apply_project_migration only. Those two go through here, and only to
    // that player -- never to a running engine (MG-1b); every editor request
    // stays closed.
    bool migrationGateOpen() const noexcept { return migration_gate_; }
    // The project this viewport runs a player on, and whether one runs.
    const QString &projectRoot() const noexcept { return project_root_; }
    bool engineRunning() const noexcept { return process_.isRunning(); }
    // MR-1s: the engine process's id (0 when none runs) -- a scenario tells a
    // restart from none by it.
    qint64 engineProcessId() const noexcept { return process_.processId(); }
    qint64 requestMigrationRpc(const QString &method,
                               const QJsonObject &params,
                               QString *error = nullptr);
    // MG-1. Stops the engine and forgets the project; nothing starts again.
    void closeProject(const QString &reason);
    // Q-4. The engine's window is a child of this panel: the re-parenting has
    // happened, so its thread was taking messages when it did.
    bool engineWindowEmbedded() const noexcept { return child_window_ != 0; }
    // True while the engine viewport is where the user's input is going: the
    // pointer is over it, or its window holds the keyboard focus. Only then
    // can the engine begin a modal transform of its own, so only then does
    // Studio have to ask it whether one has begun.
    bool engineInputActive() const noexcept { return engine_input_active_; }
    void setPickingNotice(const QString &message);
    void setGizmoNotice(const QString &message);
    // PM-3b. Asked before the footer's Stop Engine stops a running engine:
    // true stops it now; false keeps it running for now, and the guard calls
    // `stop` later if the engine is to stop after all. A play session's
    // changes go with the engine, so the window asks first (design_play_mode
    // 12.6).
    void setStopGuard(std::function<bool(std::function<void()> stop)> guard) {
        stop_guard_ = std::move(guard);
    }

  signals:
    // PM-3b. One line about how the engine was started, when the configured
    // arguments keep it from starting in edit.
    void engineLaunchNotice(const QString &line);
    void engineOutputReceived(const QString &output);
    void engineStandardErrorReceived(const QString &output);
    void viewportPointerPressed(const QPoint &pixel_position);
    void viewportPointerMoved(const QPoint &pixel_position);
    void viewportPointerReleased(const QPoint &pixel_position);
    void pickObjectSucceeded(qint64 request_id,
                             const QByteArray &result_json);
    void pickObjectFailed(qint64 request_id, const QString &message);
    void engineRpcBecameAvailable();
    void engineRpcBecameUnavailable(const QString &message);
    void engineProcessExitedWithFailure(const QString &fatal_error_line);
    void inspectorRpcSucceeded(qint64 request_id,
                               const QByteArray &result_json);
    void inspectorRpcFailed(qint64 request_id, const QString &message);
    // F-5. Emitted just before inspectorRpcFailed when the engine answered
    // with a JSON-RPC error: its data.error_code (empty when it named none)
    // and its message as the engine wrote it.
    void inspectorRpcRejected(qint64 request_id, const QString &error_code,
                              const QString &message);
    // PM-3a. The whole JSON-RPC error of an editor request, emitted before
    // inspectorRpcRejected: a refusal whose data names its code as `code`
    // (the play refusals) and carries fields beside it is read from here.
    void engineRpcRefused(qint64 request_id, int code, const QString &message,
                          const QJsonValue &data);
    // PM-3a. Every get_status result this viewport receives -- the readiness
    // probe's and any panel's -- before it is handed on. The play state rides
    // on these reads instead of on a clock of its own.
    void engineStatusRead(const QByteArray &result_json);
    // UX-7, decision 10. The same reply, parsed here once and handed to
    // every reader (a null value when it was not JSON), with the request it
    // answers: a get_status of a large graph is megabytes, and each reader
    // parsing it again on the GUI thread stopped the window.
    void engineStatusParsed(qint64 request_id, const nlohmann::json &status);
    void engineInputActiveChanged(bool active);
    void studioShortcutPressed(int key, Qt::KeyboardModifiers modifiers);
    // MG-1. `refusal` is the line the player wrote:
    // "project.migration_required@1: {documents:[...]}".
    void projectMigrationRequired(const QString &refusal);

  private:
    quint32 studio_shortcut_keys_down_ = 0;
    EngineProcess process_;
    EngineFailureModel engine_failure_model_;
    QWidget *native_host_ = nullptr;
    QLabel *status_ = nullptr;
    // Drawn inside the viewport panel while the engine's window is up but its
    // thread is still loading. The engine's window is not this panel's child
    // yet, so there is nothing to draw over -- this is what fills the black.
    QLabel *loading_notice_ = nullptr;
    QLabel *picking_notice_ = nullptr;
    QLabel *gizmo_notice_ = nullptr;
    QPushButton *restart_button_ = nullptr;
    QPushButton *stop_button_ = nullptr;
    QTimer *window_discovery_timer_ = nullptr;
    QTimer *diagnostics_timer_ = nullptr;
    QTimer *pointer_focus_timer_ = nullptr;
    QTimer *resize_timer_ = nullptr;
    // One monotonic clock for the whole of a run of the engine: the attach
    // policy compares a window's age against a log line's age, so both have to
    // be read off the same clock.
    QElapsedTimer engine_clock_;
    QElapsedTimer resize_elapsed_;
    EngineAttachPolicy attach_policy_;
    ViewportResizeCoalescer resize_coalescer_;
    NativeWindowHandle child_window_ = 0;
    bool pointer_button_was_down_ = false;
    bool primary_pointer_was_down_ = false;
    bool primary_pointer_owned_ = false;
    std::optional<QPoint> last_primary_pointer_position_;
    bool restart_after_stop_ = false;
    // G-4: restartEngine already emitted engineRpcBecameUnavailable.
    bool restart_unavailable_announced_ = false;
    bool shutting_down_ = false;
    bool rpc_ready_ = false;
    bool restart_pending_ = false;
    bool migration_gate_ = false;
    bool engine_input_active_ = false;
    // Q-4. Why this run of the engine was stopped, when Studio stopped it on
    // purpose. Kept so the exit notice does not overwrite the reason with
    // "exited (code ...)", which says nothing about what went wrong.
    QString engine_stop_reason_;
    // The one RPC whose answer turns rpc_ready_ on: the engine is ready for
    // editor requests when it has answered one, not when its window moved.
    qint64 readiness_request_ = 0;
    int engine_load_timeout_ms_ = EngineLoadTimeoutMs;
    QSize last_requested_extent_;
    QString project_root_;
    QSet<qint64> pending_pick_requests_;
    // PM-3a: the get_status requests in flight, whoever sent them.
    QSet<qint64> status_requests_;
    RpcObserver rpc_observer_;
    std::function<bool(std::function<void()>)> stop_guard_;
    const SelectionModel *selection_model_ = nullptr;

    EngineProcessLaunch launchCommand() const;
    void startEngine();
    void stopEngine();
    void discoverEngineWindow();
    void attachEngineWindow(NativeWindowHandle window);
    void setEngineLoadingNotice(const QString &message);
    void layoutEngineLoadingNotice();
    void markRpcReady();
    // UX-7, decision 10: engineStatusRead, then engineStatusParsed with the
    // one parse of it.
    void emitStatus(qint64 request_id, const QByteArray &result_json);
    void abandonEngine(const QString &reason);
    void requestEmbeddedWindowResize(ViewportExtentChangeKind kind);
    void applyEmbeddedWindowResizeDecision(const ViewportResizeDecision &decision);
    void resizeEmbeddedWindow(const QSize &pixel_extent);
    void focusEmbeddedWindow();
    void pollPointerFocus();
    void setEngineInputActive(bool active);
    void updateDiagnostics();
    void finishProcessShutdown(qint64 expected_process_id);
};

} // namespace PelicanStudio
