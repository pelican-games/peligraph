#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "graphcanvasreadcapability.hpp"
#include "nodegraphics.hpp"

#include <QGraphicsScene>
#include <QPointF>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class QGraphicsSceneMouseEvent;

namespace PelicanStudio {

// Data roles make the rendered canvas inspectable without coupling callers to
// private QGraphicsItem subclasses. Identity is the scope plus the node id,
// which is exactly the key a retained position is filed under.
inline constexpr int GraphCanvasOpRole = Qt::UserRole + 3120;
inline constexpr int GraphCanvasDomainRole = Qt::UserRole + 3121;
inline constexpr int GraphCanvasEvaluationRole = Qt::UserRole + 3122;
inline constexpr int GraphCanvasSinkRole = Qt::UserRole + 3123;
inline constexpr int GraphCanvasColumnRole = Qt::UserRole + 3124;
inline constexpr int GraphCanvasDefinitionRole = Qt::UserRole + 3125;
inline constexpr int GraphCanvasInputPortsRole = Qt::UserRole + 3126;
inline constexpr int GraphCanvasOutputPortsRole = Qt::UserRole + 3127;
inline constexpr int GraphCanvasDeclaredPortsRole = Qt::UserRole + 3128;
inline constexpr int GraphCanvasFromPortRole = Qt::UserRole + 3129;
inline constexpr int GraphCanvasToPortRole = Qt::UserRole + 3130;
inline constexpr int GraphCanvasScopeRole = NodeGraphGraphRole;

inline constexpr auto GraphCanvasNodeItem = "graph_node";
inline constexpr auto GraphCanvasNodeLabelItem = "graph_node_label";
inline constexpr auto GraphCanvasEdgeItem = "graph_edge";
inline constexpr auto GraphCanvasEdgeArrowItem = "graph_edge_arrow";
inline constexpr auto GraphCanvasEdgeLabelItem = "graph_edge_label";
inline constexpr auto GraphCanvasBreadcrumbItem = "graph_breadcrumb";

const NodeGraphKinds &graphCanvasKinds();

// Read-only canvas over one scope of the common graph. Editing is N-2; this
// scene changes nothing but which scope is being looked at and where the nodes
// sit inside this session.
class GraphCanvasScene final : public QGraphicsScene {
  public:
    explicit GraphCanvasScene(QObject *parent = nullptr);
    ~GraphCanvasScene() override;

    void setModel(GraphCanvasModel *model);
    // Draw the model's current scope. Positions the user moved are kept for
    // (scope, node id) and survive a refresh that leaves the node in place.
    void repopulate();

    // The scene changed which scope is shown (a double-click descended into a
    // definition, or the breadcrumb came back out).
    std::function<void()> scopeChanged;
    std::function<void()> selectionChanged;

    [[nodiscard]] const std::optional<std::string> &selectedNode() const noexcept {
        return selected_node_;
    }

  protected:
    void mousePressEvent(QGraphicsSceneMouseEvent *event) override;
    void mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) override;

  private:
    GraphCanvasModel *model_ = nullptr;
    // Editor session state: node positions never reach disk, and they are
    // filed under the scope so the same node id in two definitions does not
    // share one position.
    std::map<std::pair<std::string, std::string>, QPointF> session_node_positions_;
    std::optional<std::uint64_t> pruned_revision_;
    std::optional<std::string> selected_node_;
    bool rebuilding_ = false;
    bool scene_rect_update_queued_ = false;
    // A breadcrumb click rebuilds the scene from inside the base class's event
    // dispatch. Anything that ran before it must not then read the new items
    // as if they were the ones the user pressed on.
    std::uint64_t rebuild_generation_ = 0;

    void leaveCurrentScope();
    void enterInstanceScope(const std::string &id);
    void pruneSessionPositions();
    void scheduleSceneRectUpdate();
    void applySceneRectNow();
};

// The Graph tab of the render tools dock.
class GraphCanvasWidget final : public QWidget {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    explicit GraphCanvasWidget(GraphCanvasReadCapability graph,
                               QWidget *parent = nullptr);
    ~GraphCanvasWidget() override;

    void refresh();
    // The same ingestion boundary the production RPC callback uses.
    void receiveResult(qint64 request_id, const QByteArray &result_json);
    void receiveFailure(qint64 request_id, const QString &message);
    void setConnected(bool connected);

    GraphCanvasModel &model();
    GraphCanvasScene &scene();
};

} // namespace PelicanStudio
