#pragma once

#include "layoutpreset.hpp"
#include "panelshown.hpp"
#include "renderdocumentwatcher.hpp"
#include "toollayoutpreset.hpp"
#include "../model/gizmomodel.hpp"
#include "../model/modelimport.hpp"
#include "../model/project.hpp"
#include "../model/renderdocumentreload.hpp"
#include "../model/renderedithistory.hpp"
#include "../model/selection.hpp"
#include "../model/selectionhub.hpp"
#include "../viewport/enginelogbuffer.hpp"

#include <QByteArray>
#include <QHash>
#include <QKeySequence>
#include <QMainWindow>
#include <QPoint>
#include <QPointer>
#include <QStringList>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
class QTreeWidgetItem;
class QTimer;
class QFrame;
class QToolBar;
class QPushButton;

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
class ProblemsDock;
struct ProblemRow;
class ProjectMigrationWidget;
class RenderConfigDigest;
class RenderFeaturesWidget;
class RenderFallbackNotice;
class RenderingSettingsWidget;
class RayLayoutWidget;
class RuntimeOutliner;
class KeymapDialog;
enum class AddObjectKind : std::uint8_t;
struct SceneEditOutcome;
enum class SceneEditKind : std::uint8_t;

// KM-2b: the keymap split into parts the start-up path and the dialog share.
// parseKeymap and planKeymap are pure: they read bytes / a pending command
// state and return what would happen, including the same warnings KM-2a
// printed.  applyKeymapPlan is the only part that touches widgets.
struct KeymapEntry {
    QString id;
    QList<QKeySequence> keys;
    // Set by parseKeymap when the raw value could not be turned into a key
    // list.  planKeymap reports it after checking that the command is known,
    // which keeps the warnings identical to KM-2a's one-pass loader.
    QString parse_warning;
};

struct KeymapFile {
    // Well-formed entries, in file order.
    std::vector<KeymapEntry> entries;
    // Every entry, including malformed ones, in file order.  The startup
    // loader passes this to planKeymap so a bad entry for an unknown command
    // reports only the unknown-command warning.
    std::vector<KeymapEntry> scanned;
    // Entry-level warnings, already prefixed with "keymap:".
    QStringList warnings;
    // A malformed file; the caller prefixes the path (KM-2a's message).
    std::optional<QString> error;
};

struct KeymapRejection {
    QStringList keys;
    QStringList holders;
    QString reason;
};

using KeymapDefaults = std::map<QString, QList<QKeySequence>>;

struct KeymapPlan {
    // Only overridden commands the plan accepts, with their whole key list.
    // An id absent here keeps, or returns to, its original per-object default.
    KeymapDefaults accepted;
    std::map<QString, KeymapRejection> rejected;
    QStringList warnings;
};

struct KeymapObjectState {
    QObject *object = nullptr;
    QString id;
    // This object's own factory keys, not the command's aggregated keys.
    QList<QKeySequence> defaults;
};

KeymapFile parseKeymap(const QByteArray &bytes);
KeymapPlan planKeymap(const KeymapDefaults &defaults,
                      const KeymapDefaults &fixed_holders,
                      const std::vector<KeymapEntry> &overrides);
void applyKeymapPlan(const std::vector<KeymapObjectState> &objects,
                     const KeymapPlan &plan);

// Capture the command objects and the keys they have now.  MainWindow calls
// this once in its constructor, before applyUserKeymap, and keeps the result
// for the window's lifetime: a later rebind must not become a "default".
std::vector<KeymapObjectState> keymapObjectStates(QWidget &main_window);
KeymapDefaults keymapCommandDefaults(const std::vector<KeymapObjectState> &objects);
KeymapDefaults keymapFixedKeys();

using KeymapFileWriter =
    std::function<bool(const QString &path, const QByteArray &bytes, QString *error)>;

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
    // The render tools -- the Graph, the Frame Plan, Features and Rendering
    // settings, each a dock of its own (UI-2) -- by the id the command line
    // uses. Kept here so the launcher can reject an unknown name before a
    // window exists.
    static QStringList renderToolIds();
    // Bring one of them to the front: its dock is shown and raised.
    bool showRenderTool(const QString &id);
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
    // KM-2b: the atomic writer the keymap dialog uses.  A test replaces it to
    // make the final commit fail without depending on directory permissions.
    void setKeymapFileWriter(KeymapFileWriter writer);

  protected:
    void closeEvent(QCloseEvent *event) override;
    bool nativeEvent(const QByteArray &event_type, void *message, qintptr *result) override;

  private:
    friend class KeymapDialog;

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
        // PB-1s: beside Output and the Engine Log.
        ProblemsPanelDock,
        // SG-2: the canvas's Layers.
        LayersDock,
        // UI-2: the render tools that were tabs of the Frame Plan dock.
        GraphDock,
        RenderFeaturesDock,
        RenderingSettingsDock,
        DockCount,
    };

    QString application_config_directory_;
    LayoutPresetManager layout_presets_;
    std::optional<ToolLayoutPresetManager> tool_layout_presets_;
    // KM-2b: captured once, before the first keymap application.  The dialog
    // edits and saves from these, never from the keys a keymap left behind.
    std::vector<KeymapObjectState> keymap_objects_;
    KeymapDefaults keymap_defaults_;
    KeymapDefaults keymap_fixed_keys_;
    KeymapFileWriter keymap_file_writer_;
    std::array<QDockWidget *, DockCount> docks_{};
    QListWidget *project_list_ = nullptr;
    // HW-2s / AV-1s: the asset folders, their watch and the contents check.
    AssetsDock *assets_dock_ = nullptr;
    // MT-1s: the project's materials and their editor.
    MaterialsDock *materials_dock_ = nullptr;
    // PB-1s: what is wrong with the project now, and its count on the
    // status bar (a click brings the dock forward).
    ProblemsDock *problems_dock_ = nullptr;
    QPushButton *problems_count_ = nullptr;
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
    // RTL-2s: the Ray layout section of the Rendering settings tab.
    RayLayoutWidget *ray_layout_ = nullptr;
    GraphCanvasWidget *graph_canvas_ = nullptr;
    // UX-6b: the frame's GPU time, the plan's size and the last edit's
    // before and after, for the Frame Plan and the Graph tab.
    PlanPerfHub *plan_perf_ = nullptr;
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
    // HUB-1a: what the Inspector follows. Only user intent moves it: the
    // Outliner, a viewport pick, a Graph press and Back to scene.
    SelectionHub selection_hub_;
    QTimer *inspector_refresh_timer_ = nullptr;
    std::unique_ptr<PanelShown> inspector_shown_;
    bool inspector_dirty_ = false;
    // Set while the Outliner is rebuilt or a panel reveals a row: the
    // currentItemChanged that follows is not the user picking.
    bool outliner_rebuilding_ = false;
    // HUB-1a: the hub intent a viewport pick began under.
    QHash<qint64, std::uint64_t> pending_pick_intents_;
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
    enum class DocumentOperation { None, Open, Navigation, MarkOpen, MarkSave, RevertClose, RevertRecovery, KeepRecovery };
    DocumentOperation document_operation_ = DocumentOperation::None;
    qint64 document_request_ = 0;
    std::uint64_t document_generation_ = 0;
    std::uint64_t document_request_generation_ = 0;
    QPointer<QMessageBox> document_prompt_;
    void invalidateDocumentOperations(bool new_project = false);
    bool document_session_ready_ = false;
    bool document_dirty_ = false;
    bool document_save_pending_ = false;
    std::uint64_t document_save_completion_ = 0;
    void requestDocumentOperation(DocumentOperation operation);
    bool receiveDocumentResult(qint64 request, const QByteArray &bytes);
    bool receiveDocumentFailure(qint64 request, const QString &message);
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
    // MI-1s: register_model / unregister_model (engine MI-1), for Add Object
    // > Model from file..., the Inspector's Import... and the Assets dock's
    // Remove from project...; and the Outliner's Restart engine, shown when a
    // registration waits for the next start.
    std::unique_ptr<ModelRegistrationSession> model_registration_;
    QPushButton *outliner_restart_ = nullptr;
    QString outliner_restart_reason_;
    // Decision 4: the project's registered models by file (list_assets),
    // for the Assets dock's rows.
    std::map<std::string, std::string> registered_model_files_;
    qint64 pending_models_request_ = 0;
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
    std::unique_ptr<RenderDocumentWatcher> render_document_watcher_;
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
    // PB-1s, decision 3: a click on a row of Problems goes where it is fixed.
    void openProblem(const ProblemRow &row);
    // RTL-3s, decision 8: the Graph tab with a pass of the frame plan picked.
    void openPlanPassOnGraph(const QString &pass);
    void showProblems();
    void createMenus();
    bool sceneFocusGroup() const;
    // KM-2a: read `<application_config_directory>/keymap.json` once and apply
    // its per-command shortcuts over the ones the commands were made with.
    void applyUserKeymap();
    // KM-2b: snapshot the commands' own keys.  Called before applyUserKeymap
    // so a rebound command still knows the key it was made with.
    void captureKeymapDefaults();
    void showKeymapDialog();
    bool writeKeymapFile(const QString &path, const QByteArray &bytes,
                         QString *error) const;
    void updateSceneShortcuts();
    void showKeyboardShortcuts();
    void createPlayControls(QToolBar *band);
    void updatePlayPresentation();
    // PM-3b: a banner saying render edits stay after Stop, above `panel`.
    QWidget *withRenderBanner(QWidget *panel, const QString &name);
    // Shows the dock and brings it to the front of its stack.
    void showDock(DockIndex index);
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
    // MI-1s, decision 1: Add Object > Model from file...
    void addModelFromFile();
    // The file dialog and register_model; `registered` with the name once it
    // is drawn now (a registration that waits for a restart says so under
    // the Outliner instead).
    void chooseModelFile(std::function<void(const std::string &model)> registered);
    // The spawn of an object whose simplemodelview draws `model`.
    void addModelObject(const std::string &model);
    // Add Object's one spawn: the first free name from `base`, its object.
    void spawnNewObject(std::string_view base,
                        const std::function<nlohmann::json(const std::string &name)> &declaration);
    void showOutlinerRestart(const QString &line, const QString &reason);
    // Decision 4: list_assets, for the registered models' files.
    void requestRegisteredModels();
    void modelsChanged();
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
    // HUB-1a: one coalesced page build, and the Outliner's non-user current
    // row changes (a rebuild, a reveal from another panel).
    void applyInspectorPage();
    void scheduleInspectorRefresh();
    void refreshInspectorPageNow();
    void selectOutlinerRowProgrammatically(QTreeWidgetItem *item);
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
    // Last member: disconnect focus/native callbacks and cancel queued updates
    // before QWidget tears down children (which can itself emit focusChanged).
    QObject scene_shortcut_context_;
};

} // namespace PelicanStudio
