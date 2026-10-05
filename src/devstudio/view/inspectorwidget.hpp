#pragma once

#include "../model/gizmomodel.hpp"
#include "../model/inspectormodel.hpp"
#include "../model/project.hpp"
#include "../model/runtimeobjects.hpp"

#include <QWidget>

#include <memory>
#include <functional>
#include <optional>

class QEvent;
class QObject;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;

class InspectorWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit InspectorWidget(EmbeddedViewport *viewport,
                             QWidget *parent = nullptr);
    ~InspectorWidget() override;

    void setSelection(std::optional<OutlinerObjectKey> selection,
                      const QString &display_text);
    // PM-3c: an object the running game spawned, read-only: its runtime
    // Transform and the prefab it came from, and no field to edit. Called
    // again with each new reading; setSelection leaves it.
    void showRuntimeObject(const RuntimeObject &object);
    [[nodiscard]] bool showsRuntimeObject() const noexcept;
    std::optional<GizmoTransformBinding> gizmoTransformBinding() const;
    bool beginGizmoEdit(std::string_view field_key);
    bool previewGizmoEdit(std::string_view field_key,
                          nlohmann::json value);
    void finishGizmoEdit(std::string_view field_key, bool commit);
    bool saveScene();
    // G-2: Add object. One `spawn` through the Inspector's editor session, so
    // it shares its actor, its revision and its undo stack. Q-7: asked while
    // an edit is in flight, it waits for that edit (Queued). On refusal the
    // reason is in *error.
    SpawnOutcome spawnObject(const std::string &scene_id, nlohmann::json object,
                             QString *error);
    // OL-1: the Outliner's Duplicate, Delete and drag, through the same
    // session. Refused while another edit is in flight; the reason in *error.
    SpawnOutcome submitSceneEdit(SceneEditKind kind, std::uint64_t subject_id,
                                 nlohmann::json operation, const std::string &description,
                                 QString *error);
    // Called, once the reply has been handled, for each spawn, Outliner edit,
    // undo and redo that reached its end.
    void setSceneEditCompletedAction(std::function<void(const SceneEditOutcome &)> action);
    bool hasUnsavedSceneChanges() const noexcept;
    bool sceneEditPending() const noexcept;
    bool sceneSaveInFlight() const noexcept;
    std::uint64_t sceneSaveCompletion() const noexcept;
    bool lastSceneSaveSucceeded() const noexcept;
    void observeSceneRevision(std::uint64_t revision);
    void resetSceneSaveState();
    void setSceneStateChangedAction(std::function<void()> action);
    // PM-3b (design_play_mode 12.3). What the Inspector asks of the play
    // session: whether one runs (Take from game, "— reverts on Stop"),
    // whether an edit is marked Keep, how to mark it, and where to tell each
    // edit it committed.
    struct PlaySessionHooks {
        std::function<bool()> session;
        std::function<bool(const std::string &transaction_id)> kept;
        std::function<void(const std::string &transaction_id)> keep;
        std::function<void(const CommittedEdit &edit)> committed;
    };
    void setPlaySessionHooks(PlaySessionHooks hooks);
    // The session began, ended or its Keep marks moved.
    void playSessionChanged();
    // Reads the selected object again (after Stop put the scene back).
    void refreshFromEngine();
    // See InspectorModel::adoptRestoredRevisionAsSaved.
    void adoptRestoredRevisionAsSaved();
    void setRayGroupAction(std::function<void(const QString &)> action);
    // RTL-2s, decision 4: Assign hit group... on the Material rows, with the
    // material they show (its name).
    void setAssignHitGroupAction(std::function<void(const QString &material)> action);
    // RTL-2s-c (RTL-2s decision 5): Add component's Ray geometry, which needs
    // a ray layout, offers Enable hit groups... -- the Ray layout section.
    void setEnableHitGroupsAction(std::function<void()> action);
    // Where "Open the source" on a graph input row goes. The Inspector names
    // the producing node; the composition root decides which view shows it.
    void setGraphSourceRevealAction(
        std::function<void(const QString &node, const QString &port)> action);
    // RSX, decision 1. A graph attachment edit that committed with
    // restart_required hands the restart to this action.
    void setEngineRestartAction(std::function<void(const QString &reason)> action);
    // MT-1s, decision 5. The selected object's model (its simplemodelview's)
    // and the object's name; nullopt when it shows none. The Materials dock's
    // Assign to selected object binds to it.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> materialSubject() const;
    // The materials (a new one), or the model's bindings, changed elsewhere:
    // the Material rows read them again.
    void resetMaterialState();
    void materialsChanged();
    void materialBindingsChanged();
    // EB-6s, decision 5: the Material rows scrolled into view -- now, or once
    // the selected object's are shown (a Problems row picked the object).
    void revealMaterialRows();
    // MI-1s, decision 2. Import... beside a simplemodelview's model field
    // (`pelican.inspectorModelImport`, shown once an action is set): the
    // action chooses a file and registers it, and calls `registered` with
    // the model's name when it is drawn now (not when the engine has to start
    // again); the field is then set to that name and committed.
    void setModelImportAction(
        std::function<void(std::function<void(const std::string &name)> registered)> action);
    // A model was registered or taken back elsewhere: the model field's list
    // is read again.
    void modelsChanged();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace PelicanStudio
