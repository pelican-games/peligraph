#pragma once

#include "layoutpreset.hpp"
#include "toollayoutpreset.hpp"
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
class QCloseEvent;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QString;
class QTreeWidget;
class QTabWidget;
class QTreeWidgetItem;
class QTimer;

namespace PelicanStudio {

class EmbeddedViewport;
class FramePlanWidget;
class GraphCanvasWidget;
class FullscreenPassWidget;
class InspectorWidget;
class RenderFeaturesWidget;

class MainWindow : public QMainWindow {
  public:
    MainWindow();
    explicit MainWindow(const QString &application_config_directory);
    void openProject(const QString &path);
    // The render-tools tabs, by the id the command line uses. Kept here so the
    // launcher can reject an unknown name before a window exists.
    static QStringList renderToolsTabIds();
    // Bring one of them to the front, showing the dock that holds them.
    bool showRenderToolsTab(const QString &id);
    // The engine viewport this window drives. Read-only handle for the
    // launcher, which hands it to the test-automation channel so a scenario can
    // ask the running engine the same questions the panels ask it.
    [[nodiscard]] EmbeddedViewport *engineViewport() const noexcept {
        return viewport_;
    }

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    enum DockIndex {
        ProjectDock,
        OutlinerDock,
        InspectorDock,
        OutputDock,
        EngineLogDock,
        FramePlanDock,
        FullscreenPassDock,
        DockCount,
    };

    QString application_config_directory_;
    LayoutPresetManager layout_presets_;
    std::optional<ToolLayoutPresetManager> tool_layout_presets_;
    std::array<QDockWidget *, DockCount> docks_{};
    QListWidget *project_list_ = nullptr;
    QTreeWidget *outliner_ = nullptr;
    EmbeddedViewport *viewport_ = nullptr;
    InspectorWidget *inspector_ = nullptr;
    FramePlanWidget *frame_plan_ = nullptr;
    RenderFeaturesWidget *render_features_ = nullptr;
    GraphCanvasWidget *graph_canvas_ = nullptr;
    QTabWidget *render_tabs_ = nullptr;
    FullscreenPassWidget *fullscreen_pass_ = nullptr;
    QMenu *restore_layout_menu_ = nullptr;
    QMenu *delete_layout_menu_ = nullptr;
    QAction *save_tool_layout_action_ = nullptr;
    QMenu *restore_tool_layout_menu_ = nullptr;
    QMenu *delete_tool_layout_menu_ = nullptr;
    QPlainTextEdit *engine_log_ = nullptr;
    EngineLogBuffer engine_log_buffer_;
    std::optional<ProjectOutlinerModel> project_model_;
    SelectionModel selection_model_;
    GizmoModel gizmo_model_;
    std::array<QAction *, 3> gizmo_mode_actions_{};
    QTimer *modal_transform_timer_ = nullptr;
    QTimer *outliner_refresh_timer_ = nullptr;
    qint64 pending_outliner_request_ = 0;
    QHash<qint64, ViewportPickToken> pending_pick_tokens_;
    QHash<qint64, quint64> pending_gizmo_requests_;
    std::uint64_t presented_gizmo_notice_revision_ = 0;
    std::uint64_t presented_gizmo_binding_revision_ = 0;

    void createWorkspace();
    void createMenus();
    void chooseProject();
    void populateOutliner();
    void requestOutlinerRefresh();
    void completeOutlinerRpc(qint64 request_id,
                             const QByteArray &result_json);
    void failOutlinerRpc(qint64 request_id, const QString &message);
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
    LayoutRestoreResult applyLayoutPreset(const QString &name,
                                          QString *error);
    void saveLastPanelLayout();
    void restoreLastPanelLayout();
    void saveToolLayoutPreset();
    void restoreToolLayoutPreset(const QString &name);
    void deleteToolLayoutPreset(const QString &name);
    void saveLastToolLayout();
    QString restoreLastToolLayout();
    void applyDefaultLayout();
};

} // namespace PelicanStudio
