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

#include <optional>

class QLabel;
class QPushButton;
class QTimer;

namespace PelicanStudio {

class SelectionModel;
struct OutlinerObjectKey;

QStringList studioPlayerArguments(QStringList configured,
                                  const QString &project_root);

class EmbeddedViewport final : public QWidget {
    Q_OBJECT

  public:
    explicit EmbeddedViewport(QWidget *parent = nullptr);
    ~EmbeddedViewport() override;

    void openProject(const QString &project_root);
    void bindSelectionModel(const SelectionModel *selection_model) noexcept {
        selection_model_ = selection_model;
    }
    const OutlinerObjectKey *selectedObject() const noexcept;
    qint64 pickObject(const QPoint &pixel_position,
                      QString *error = nullptr);
    qint64 requestRpc(const QString &method, const QJsonObject &params,
                      QString *error = nullptr);
    bool rpcReady() const noexcept { return rpc_ready_; }
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

  signals:
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
    void engineInputActiveChanged(bool active);

  private:
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
    bool shutting_down_ = false;
    bool rpc_ready_ = false;
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
    const SelectionModel *selection_model_ = nullptr;

    EngineProcessLaunch launchCommand() const;
    void startEngine();
    void stopEngine();
    void discoverEngineWindow();
    void attachEngineWindow(NativeWindowHandle window);
    void setEngineLoadingNotice(const QString &message);
    void layoutEngineLoadingNotice();
    void markRpcReady();
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
