#pragma once

#include "../model/gizmomodel.hpp"
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
    // it shares its actor, its revision and its undo stack. On refusal the
    // reason is in *error.
    bool spawnObject(const std::string &scene_id, nlohmann::json object,
                     QString *error);
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
