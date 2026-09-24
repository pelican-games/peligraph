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
#include <QPointer>

#include <array>
#include <optional>

class QDockWidget;
class QAction;
class QCloseEvent;
class QLabel;
class QListWidget;
class QMenu;
class QMessageBox;
class QDialog;
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
    int outliner_refresh_interval_ms_ = 0;
    qint64 pending_outliner_request_ = 0;
    QHash<qint64, ViewportPickToken> pending_pick_tokens_;
    QHash<qint64, quint64> pending_gizmo_requests_;
    std::uint64_t presented_gizmo_notice_revision_ = 0;
    std::uint64_t presented_gizmo_binding_revision_ = 0;
    enum class Navigation { None, Close, Project };
    Navigation pending_navigation_ = Navigation::None;
    QString pending_project_path_;
    qint64 navigation_revision_request_ = 0;
    bool navigation_waiting_for_save_ = false;
    std::uint64_t navigation_save_completion_ = 0;
    bool close_authorized_ = false;
    QPointer<QMessageBox> navigation_prompt_;
    QPointer<QDialog> new_project_dialog_;

    void createWorkspace();
    void createMenus();
    // View > Theme. One entry per theme in the table, each carrying the
    // objectName a scenario triggers (`pelican.view.theme.<name>`).
    void createThemeMenu(QMenu *view_menu);
    void chooseTheme(const QString &name);
    // The stored preference, applied before the first widget is built.
    void applyStoredTheme();
    void chooseProject();
    // File > New Project...: the dialog writes a template and hands the new
    // root to openProject, so the unsaved-changes guard applies to it too.
    void chooseNewProject();
    void openProjectNow(const QString &path);
    void requestNavigation(Navigation destination, const QString &path = {});
    void checkNavigationRevision();
    void continueNavigation();
    void answerNavigationPrompt(int answer);
    void completeNavigation();
    void cancelNavigation(const QString &message = {});
    void sceneStateChanged();
    void populateOutliner();
    void requestOutlinerRefresh();
    // The modal poll runs while, and only while, a modal transform could be
    // under way. Called wherever one of the three facts it reads can move.
    void updateModalTransformPolling();
    // The outliner asks again quickly while the scene is moving and slows
    // down while it is not, instead of asking four times a second forever.
    void setOutlinerRefreshInterval(int interval_ms);
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
