#pragma once

#include "layoutpreset.hpp"
#include "../model/gizmomodel.hpp"
#include "../model/project.hpp"
#include "../model/selection.hpp"
#include "../viewport/enginelogbuffer.hpp"

#include <QByteArray>
#include <QHash>
#include <QMainWindow>
#include <QPoint>

#include <array>
#include <optional>

class QDockWidget;
class QAction;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QString;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace PelicanStudio {

class EmbeddedViewport;
class FramePlanWidget;
class InspectorWidget;

class MainWindow : public QMainWindow {
  public:
    MainWindow();

  private:
    enum DockIndex {
        ProjectDock,
        OutlinerDock,
        InspectorDock,
        OutputDock,
        EngineLogDock,
        FramePlanDock,
        DockCount,
    };

    LayoutPresetManager layout_presets_;
    std::array<QDockWidget *, DockCount> docks_{};
    QListWidget *project_list_ = nullptr;
    QTreeWidget *outliner_ = nullptr;
    EmbeddedViewport *viewport_ = nullptr;
    InspectorWidget *inspector_ = nullptr;
    FramePlanWidget *frame_plan_ = nullptr;
    QMenu *restore_layout_menu_ = nullptr;
    QMenu *delete_layout_menu_ = nullptr;
    QPlainTextEdit *engine_log_ = nullptr;
    EngineLogBuffer engine_log_buffer_;
    std::optional<ProjectOutlinerModel> project_model_;
    SelectionModel selection_model_;
    GizmoModel gizmo_model_;
    std::array<QAction *, 3> gizmo_mode_actions_{};
    QTimer *modal_transform_timer_ = nullptr;
    QHash<qint64, ViewportPickToken> pending_pick_tokens_;
    QHash<qint64, quint64> pending_gizmo_requests_;
    std::uint64_t presented_gizmo_notice_revision_ = 0;
    std::uint64_t presented_gizmo_binding_revision_ = 0;

    void createWorkspace();
    void createMenus();
    void chooseProject();
    void openProject(const QString &path);
    void populateOutliner();
    void selectOutlinerItem(QTreeWidgetItem *item);
    void beginViewportPick(const QPoint &pixel_position);
    void completeViewportPick(qint64 request_id,
                              const QByteArray &result_json);
    void failViewportPick(qint64 request_id, const QString &message);
    void beginViewportPointer(const QPoint &pixel_position);
    void moveViewportPointer(const QPoint &pixel_position);
    void releaseViewportPointer(const QPoint &pixel_position);
    void completeGizmoRpc(qint64 request_id,
                          const QByteArray &result_json);
    void failGizmoRpc(qint64 request_id, const QString &message);
    void dispatchGizmoModel();
    void setGizmoMode(GizmoMode mode);
    void updateGizmoToolbar();
    void presentGizmoNotice();
    void refreshSelectionViews();
    void presentPickingFailure(const QString &message);
    void appendEngineOutput(const QString &output);
    void refreshLayoutMenus();
    void saveLayoutPreset();
    void restoreLayoutPreset(const QString &name);
    void deleteLayoutPreset(const QString &name);
    void applyDefaultLayout();
};

} // namespace PelicanStudio
