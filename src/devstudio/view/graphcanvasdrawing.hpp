#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "nodegraphics.hpp"
#include "nodelayout.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace PelicanStudio {

struct StudioTheme;
struct GraphCanvasEdgeKey;
struct GraphCanvasTie;

// Internal drawing helpers. The scene owns document/session state and all
// item callbacks; this module only measures rows and creates their Qt items.
namespace GraphCanvasDrawing {

QString portSummary(const GraphCanvasPort &port);
QString numberText(double value);

struct Value {
    QString text;
    QString source;
    [[nodiscard]] bool shown() const { return !text.isEmpty(); }
};

struct Row {
    QString port;
    QString label;
    QString type;
    SocketStyle style;
    Value value;
    bool connected = false;
    bool declared = false;
};

// Borrowed from the model's node snapshot for the duration of one rebuild.
// No graphics item or callback retains this drawing data.
struct Node {
    const GraphCanvasNode *node = nullptr;
    std::vector<Row> outputs;
    std::vector<Row> inputs;
    qreal width = 0.0;
    qreal height = 0.0;
    bool folded = false;
};

NodeGraphLayout placeNodes(const std::vector<Node> &nodes,
                          const std::vector<GraphCanvasEdge> &edges, qreal top);

class Builder {
  public:
    using Moved = std::function<void(const QPointF &)>;

    Builder(QGraphicsScene &scene, QString scope);
    Node prepareNode(const GraphCanvasModel &model, const GraphCanvasNode &node,
                     bool folded) const;
    void addNode(const Node &entry, int column, const QPointF &position, Moved moved);
    QGraphicsPathItem *addEdge(const GraphCanvasEdge &edge, const QVector<QPointF> &bends);
    void addReroute(const GraphCanvasEdgeKey &key, std::size_t index,
                   const QPointF &position, Moved moved);
    void addTie(const GraphCanvasTie &tie, std::size_t index, Moved moved);
    void addBreadcrumb(const QStringList &trail, qreal top,
                       std::function<void()> leave_scope);

  private:
    void addNodeHeader(QGraphicsPathItem *item, const Node &entry);
    void addNodeRows(QGraphicsPathItem *item, const Node &entry);

    QGraphicsScene &scene_;
    QString scope_text_;
    const StudioTheme &theme_;
    QFont label_font_;
    QFont detail_font_;
    QFont heading_font_;
    QFontMetricsF heading_metrics_;
    QFontMetricsF row_metrics_;
};

} // namespace GraphCanvasDrawing
} // namespace PelicanStudio
