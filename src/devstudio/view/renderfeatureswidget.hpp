#pragma once

#include "../model/frameplanmodel.hpp"

#include <QWidget>
#include <QStringList>

#include <functional>
#include <memory>
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

  signals:
    void renderConfigCommitted();
};

} // namespace PelicanStudio
