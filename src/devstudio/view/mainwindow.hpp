#pragma once

#include "layoutpreset.hpp"
#include "toollayoutpreset.hpp"
#include "../model/gizmomodel.hpp"
#include "../model/project.hpp"
#include "../model/renderdocumentreload.hpp"
#include "../model/renderedithistory.hpp"
#include "../model/selection.hpp"
#include "../viewport/enginelogbuffer.hpp"

#include <QByteArray>
#include <QHash>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
class QFrame;
class QToolBar;

namespace PelicanStudio {

class AssetsDock;
class MaterialsDock;
class EmbeddedViewport;
class FramePlanWidget;
class GraphCanvasWidget;
class PlanPerfHub;
class FullscreenPassWidget;
class InspectorWidget;
class OutlinerTree;
class PlayControls;
class ProjectMigrationWidget;
class RenderConfigDigest;
class RenderFeaturesWidget;
class RenderFallbackNotice;
class RenderingSettingsWidget;
class RuntimeOutliner;
enum class AddObjectKind : std::uint8_t;
struct SceneEditOutcome;
enum class SceneEditKind : std::uint8_t;

class MainWindow : public QMainWindow {
  public:
    MainWindow();
    explicit MainWindow(const QString &application_config_directory);
    void openProject(const QString &path);
    // G-5. Restart the engine on the same project for a committed setting it
    // reads only at start (the Frame Plan's restart_required). A scene with
    // unsaved edits is asked about first -- Save / Discard / Cancel, the same
    // prompt a project switch shows -- and Cancel leaves the engine running.
    void requestEngineRestart(const QString &reason);
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
    // F-3. How a shader file is handed to an editor, for both panels that
    // open one (the Frame Plan's Open and the Pass Editor's New...). The
    // desktop's own by default; the automation launch replaces it so a
    // scenario never starts an application.
    void setShaderSourceOpenAction(
        std::function<bool(const std::filesystem::path &)> action);
    // UX-7, decision 9. The automation channel's quit: a run in Play or Pause
    // is stopped (nothing kept) and `then` is called once it has; with no
    // run, at once. A person closing the window is still asked; a scenario's
    // shutdown is no person, and the prompt nobody answered held it 30 s.
    void stopPlayThen(std::function<void()> then);

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
        // HW-2s: beside Project.
        AssetsPanelDock,
        // MT-1s: beside Assets.
        MaterialsPanelDock,
        DockCount,
    };

    QString application_config_directory_;
    LayoutPresetManager layout_presets_;
    std::optional<ToolLayoutPresetManager> tool_layout_presets_;
    std::array<QDockWidget *, DockCount> docks_{};
    QListWidget *project_list_ = nullptr;
    // HW-2s / AV-1s: the asset folders, their watch and the contents check.
    AssetsDock *assets_dock_ = nullptr;
    // MT-1s: the project's materials and their editor.
    MaterialsDock *materials_dock_ = nullptr;
    OutlinerTree *outliner_ = nullptr;
    // Q-7: one line under the Outliner for what became of an Add object.
    QLabel *outliner_notice_ = nullptr;
    EmbeddedViewport *viewport_ = nullptr;
    // MG-1: shown instead of the viewport while the project being opened was
    // written by an earlier engine.
    ProjectMigrationWidget *migration_panel_ = nullptr;
    InspectorWidget *inspector_ = nullptr;
    FramePlanWidget *frame_plan_ = nullptr;
    RenderFeaturesWidget *render_features_ = nullptr;
    // RSX: the rendering section's own keys (EL-3), beside Features.
    RenderingSettingsWidget *rendering_settings_ = nullptr;
    GraphCanvasWidget *graph_canvas_ = nullptr;
    // UX-6b: the frame's GPU time, the plan's size and the last edit's
    // before and after, for the Frame Plan and the Graph tab.
    PlanPerfHub *plan_perf_ = nullptr;
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
    // UX-13c: the Outliner reads scene_outline, conditioned on the revision
    // it holds. An engine that does not know the method (-32601) is read
    // with scene_tree for as long as it runs; a refused conditional poll is
    // followed by an unconditional one.
    bool pending_outliner_is_tree_ = false;
    bool pending_outliner_unknown_method_ = false;
    bool outliner_uses_scene_tree_ = false;
    bool outliner_ask_unconditionally_ = false;
    QHash<qint64, ViewportPickToken> pending_pick_tokens_;
    QHash<qint64, quint64> pending_gizmo_requests_;
    std::uint64_t presented_gizmo_notice_revision_ = 0;
    std::uint64_t presented_gizmo_binding_revision_ = 0;
    enum class Navigation { None, Close, Project, Restart };
    Navigation pending_navigation_ = Navigation::None;
    QString pending_project_path_;
    QString pending_restart_reason_;
    qint64 navigation_revision_request_ = 0;
    bool navigation_waiting_for_save_ = false;
    std::uint64_t navigation_save_completion_ = 0;
    bool close_authorized_ = false;
    QPointer<QMessageBox> navigation_prompt_;
    // PM-3b (design_play_mode 12.6): leaving while a play session runs is
    // asked first; once agreed, the session is stopped (nothing kept) and
    // the navigation goes on when the engine is back in edit.
    QPointer<QMessageBox> play_session_prompt_;
    bool play_session_confirmed_ = false;
    std::function<void()> after_play_stop_;
    QPointer<QDialog> new_project_dialog_;
    // G-2: Add object. One menu, shown by the Outliner's right click and by
    // the Scene toolbar, so a scenario fires the same four named actions.
    QMenu *add_object_menu_ = nullptr;
    // The name a spawn took, by scene, until it is in (or refused): the next
    // Add Object numbers past it.
    std::optional<std::pair<std::string, std::string>> pending_spawn_name_;
    // OL-1: the object an edit created (a spawn, a duplicate, an undone
    // delete), by scene and authoring id, until the Outliner shows it and it
    // can be selected there.
    std::optional<std::pair<std::string, std::uint64_t>> pending_created_selection_;
    // OL-1: the one Outliner edit in flight (the Inspector's session takes
    // one at a time): what it was about, by name for its refusal, and where
    // the selection goes when a delete commits.
    struct OutlinerEdit {
        std::uint64_t subject_id = 0;
        std::string subject_name;
        std::string target_name;
        std::optional<OutlinerObjectKey> select_after;
    };
    std::optional<OutlinerEdit> pending_outliner_edit_;
    QMenu *outliner_menu_ = nullptr;
    // RU-1. The one history of render edits, shared by the Frame Plan, the
    // Pass Editor, the Task Editor and Render Features, and its two actions:
    // on the Frame Plan's toolbar, in the Edit menu, and Ctrl+Z / Ctrl+Y
    // while one of those panels holds the focus.
    std::shared_ptr<RenderConfigDigest> render_digest_;
    std::optional<RenderEditHistory> render_history_;
    QAction *render_undo_action_ = nullptr;
    QAction *render_redo_action_ = nullptr;
    std::uint64_t shown_render_history_line_ = 0;
    // RL-1s. The render documents rewritten on disk, taken in through
    // reload_render_documents; and the Graph menu's "Reload from disk".
    std::optional<RenderDocumentReload> render_reload_;
    QAction *reload_from_disk_action_ = nullptr;
    // BS-1s: the row above the viewport while the engine draws its
    // placeholder, and how Open file opens a file (the desktop's editor;
    // under automation, a line in the log).
    RenderFallbackNotice *render_fallback_ = nullptr;
    std::function<bool(const std::filesystem::path &)> file_open_action_;
    // PM-3a: Pause / Step on the gizmo band, the play line of the status bar,
    // and what a paused game turns the viewport's frame and that band.
    PlayControls *play_controls_ = nullptr;
    QFrame *viewport_frame_ = nullptr;
    QToolBar *gizmo_toolbar_ = nullptr;
    // PM-3c: the Outliner's rows for what the running game spawned.
    RuntimeOutliner *runtime_outliner_ = nullptr;
    // PM-3b: Save waits for Stop (12.4); the render panels say their edits
    // stay (12.4); whether the last reading had a session, to read the
    // scene again when it ends.
    QAction *save_scene_action_ = nullptr;
    std::vector<QLabel *> render_banners_;
    bool shown_play_session_ = false;
    // Whether the scene was saved while in edit, and when Play began; a Stop
    // that kept nothing then leaves it saved.
    bool clean_in_edit_ = true;
    bool clean_at_play_ = false;
    std::uint64_t shown_stop_serial_ = 0;

    void createWorkspace();
    void createRenderHistory(QWidget *pass_editor);
    void renderHistoryChanged();
    void renderDocumentsRestored(const nlohmann::json &commit, bool restart_required);
    void createRenderReload();
    void renderDocumentsReloaded(const nlohmann::json &commit);
    void createMenus();
    void createPlayControls(QToolBar *band);
    void updatePlayPresentation();
    // PM-3b: a banner saying render edits stay after Stop, above `panel`.
    QWidget *withRenderBanner(QWidget *panel, const QString &name);
    // PM-3b: asks whether to end the play session before leaving; `proceed`
    // runs once the engine is back in edit. False when nothing had to be
    // asked.
    bool askToEndPlaySession(const QString &action, bool stop_first,
                             std::function<void()> proceed, std::function<void()> cancelled);
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
    // MG-1. The project at `project_root` needs migrating: whatever was open
    // is left, the migration panel takes the viewport's place, and the player
    // is started on the project to serve the plan.
    void beginProjectMigration(const std::filesystem::path &project_root,
                               const QString &refusal);
    void endProjectMigration();
    // What opening another project resets, without opening one.
    void leaveCurrentProject(const std::filesystem::path &next_project_root);
    void requestNavigation(Navigation destination, const QString &path = {});
    void checkNavigationRevision();
    void continueNavigation();
    void answerNavigationPrompt(int answer);
    void completeNavigation();
    void cancelNavigation(const QString &message = {});
    void resetNavigation();
    void sceneStateChanged();
    void populateOutliner();
    void createAddObjectMenu();
    void addObject(AddObjectKind kind);
    // Selects the pending created object once the Outliner holds it.
    void selectCreatedObject();
    // OL-1: Duplicate, Delete, and a row dropped on another.
    void duplicateSelectedObject();
    void deleteSelectedObject();
    void deleteObject(const OutlinerObjectKey &key);
    void moveObject(const OutlinerObjectKey &dragged,
                    const std::optional<OutlinerObjectKey> &onto,
                    bool preserve_local);
    void refuseObjectMove(const OutlinerObjectKey &dragged,
                          const std::optional<OutlinerObjectKey> &onto);
    bool submitOutlinerEdit(SceneEditKind kind, const OutlinerObject &subject,
                            nlohmann::json operation, const QString &description,
                            OutlinerEdit pending);
    void sceneEditCompleted(const SceneEditOutcome &outcome);
    void updateOutlinerActions();
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
    // PM-3c: the runtime rows were read again.
    void runtimeObjectsChanged();
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
    // F-7: opening another project stops the engine that is running. What it
    // still owes the gizmo is abandoned there, not reported as a failure: the
    // session is ended before the switch and starts again with the new engine.
    void endGizmoSessionForProjectSwitch(
        const std::filesystem::path &next_project_root);
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
