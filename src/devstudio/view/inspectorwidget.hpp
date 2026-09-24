#pragma once

#include "../model/gizmomodel.hpp"
#include "../model/inspectormodel.hpp"
#include "../model/project.hpp"

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
    void setRayGroupAction(std::function<void(const QString &)> action);
    // Where "Open the source" on a graph input row goes. The Inspector names
    // the producing node; the composition root decides which view shows it.
    void setGraphSourceRevealAction(
        std::function<void(const QString &node, const QString &port)> action);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace PelicanStudio
