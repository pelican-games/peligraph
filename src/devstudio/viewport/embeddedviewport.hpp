#pragma once

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
    void setPickingNotice(const QString &message);
    void setGizmoNotice(const QString &message);

  signals:
    void engineOutputReceived(const QString &output);
    void viewportPointerPressed(const QPoint &pixel_position);
    void viewportPointerMoved(const QPoint &pixel_position);
    void viewportPointerReleased(const QPoint &pixel_position);
    void pickObjectSucceeded(qint64 request_id,
                             const QByteArray &result_json);
    void pickObjectFailed(qint64 request_id, const QString &message);
    void engineRpcBecameAvailable();
    void engineRpcBecameUnavailable(const QString &message);
    void inspectorRpcSucceeded(qint64 request_id,
                               const QByteArray &result_json);
    void inspectorRpcFailed(qint64 request_id, const QString &message);

  private:
    EngineProcess process_;
    QWidget *native_host_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *picking_notice_ = nullptr;
    QLabel *gizmo_notice_ = nullptr;
    QPushButton *restart_button_ = nullptr;
    QPushButton *stop_button_ = nullptr;
    QTimer *window_discovery_timer_ = nullptr;
    QTimer *diagnostics_timer_ = nullptr;
    QTimer *pointer_focus_timer_ = nullptr;
    QTimer *resize_timer_ = nullptr;
    QElapsedTimer window_discovery_elapsed_;
    QElapsedTimer resize_elapsed_;
    ViewportResizeCoalescer resize_coalescer_;
    NativeWindowHandle child_window_ = 0;
    bool pointer_button_was_down_ = false;
    bool primary_pointer_was_down_ = false;
    bool primary_pointer_owned_ = false;
    std::optional<QPoint> last_primary_pointer_position_;
    bool restart_after_stop_ = false;
    bool shutting_down_ = false;
    bool rpc_ready_ = false;
    QSize last_requested_extent_;
    QString project_root_;
    QSet<qint64> pending_pick_requests_;
    const SelectionModel *selection_model_ = nullptr;

    EngineProcessLaunch launchCommand() const;
    void startEngine();
    void stopEngine();
    void discoverEngineWindow();
    void requestEmbeddedWindowResize(ViewportExtentChangeKind kind);
    void applyEmbeddedWindowResizeDecision(const ViewportResizeDecision &decision);
    void resizeEmbeddedWindow(const QSize &pixel_extent);
    void focusEmbeddedWindow();
    void pollPointerFocus();
    void updateDiagnostics();
    void finishProcessShutdown(qint64 expected_process_id);
};

} // namespace PelicanStudio
