#pragma once

#include "../model/frameplanmodel.hpp"

#include <QWidget>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QByteArray;
class QString;

namespace PelicanStudio {

class EmbeddedViewport;
class RenderConfigDigest;

// RPC-only Studio surface for the engine-owned authored config features[].
// It deliberately has no pelican_core types or feature-name constants.
class RenderFeaturesWidget final : public QWidget {
    Q_OBJECT

    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit RenderFeaturesWidget(
        EmbeddedViewport *viewport,
        QWidget *parent = nullptr);
    ~RenderFeaturesWidget() override;

    // Applies the successful list_render_features payload. Production RPC
    // delivery and focused widget tests intentionally share this entry.
    void editFrameNodeOrder(const QString &graph, const QStringList &names, quint64 generation);
    void editComputeTask(const QString &name);
    void editComputeTaskConnection(const QString &name, const QStringList &outputs);
    void receiveCatalogResult(const QByteArray &result_json);
    // Q-12. The Task Editor's "Open in Graph" for a task a render.compute
    // node made: (scope, node, instance) as get_compute_task_authoring's
    // graph_node names them. False when the Graph tab could not show it.
    using GraphNodeOpenAction = std::function<bool(
        const QString &scope, const QString &node, const QString &instance)>;
    void setGraphNodeOpenAction(GraphNodeOpenAction action);
    // LP-4b. The view axes of a task's node in the current frame plan (the
    // engine's view_axes), for the Task Editor's View field. Empty -- no
    // field -- for a task whose family has none, or that the plan does not
    // hold.
    using TaskViewAxesProvider =
        std::function<std::vector<FramePlanViewAxis>(const QString &task)>;
    void setTaskViewAxesProvider(TaskViewAxesProvider provider);
    // Q-13. The rendering document's digest, shared with the other panels
    // that edit it: the panel refreshes when another panel's commit moved
    // the document, and sends an edit refused for that once more.
    void setRenderConfigDigest(std::shared_ptr<RenderConfigDigest> digest);
    // RU-1. The render edit history's Undo / Redo: their keys work in this
    // panel and in every editor it opens (the Task Editor among them).
    void setRenderHistoryActions(const QList<QAction *> &actions);
    // LP-4f2. "Replace with loop version..." on a feature the engine ships a
    // decomposed graph for (get_graph_templates' decomposes.feature): the
    // confirmation, then apply_graph_template. The Frame Plan's right-click
    // on a feature's group comes here too.
    void replaceWithLoopVersion(const QString &feature_reference);
    // nullopt when the feature has no loop version (or is not listed); an
    // empty string when it can be taken in now, the reason otherwise.
    [[nodiscard]] std::optional<QString> loopVersionOffered(
        const QString &feature_reference) const;
    // UX-12b, decision 2. A feature refused because the pipeline has no pass
    // it is inserted after (feature.insertion_anchor_missing@1, or the
    // composer's sentence) offers "Place <pass> in the Graph": this action
    // places a pass named `anchor` there (the main window's, the Graph tab's
    // New… path). False when it could not.
    void setPlaceInsertionAnchorAction(std::function<bool(const QString &anchor)> action);
    // Who hears the loop an apply_graph_template added, once it committed.
    void setLoopVersionAppliedAction(std::function<void(const QString &loop)> action);
    // RSX, decision 1. A commit -- of this panel or of an editor it opens
    // (Tasks, Fork, Regions, Records, Scene AS, the node order) -- that
    // answers restart_required hands the restart to this action (the main
    // window's requestEngineRestart), and the notice says "Restarting the
    // engine to apply this change (<code>)..." instead of "Applied without
    // restarting". When the restart is declined, engineRestartDeclined says
    // why and reads the list from the engine that stays.
    void setEngineRestartAction(std::function<void(const QString &reason)> action);
    void engineRestartDeclined(const QString &message);

  signals:
    void renderConfigCommitted();
};

} // namespace PelicanStudio
