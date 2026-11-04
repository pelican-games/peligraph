// SDK-2a: the fixed-size plot under an open value.curve's rows. It is read
// only -- no press, no drag, no edit -- and draws the same frame and path as
// the large GraphCanvasCurveGraph. It adds height only to the open view, the
// same 72 px for one key or fifty, so compact/minimal nodes keep the default
// body's size and never show a plot.

#include "graphcanvasdrawing.hpp"

#include "graphcanvascurvegeometry.hpp"
#include "graphcanvasparts.hpp"
#include "studiotheme.hpp"

#include <QBrush>
#include <QGraphicsPathItem>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <memory>

namespace PelicanStudio::GraphCanvasDrawing {
namespace {

using Json = nlohmann::json;

struct CurvePlotData final : NodeBodyData {
    std::vector<GraphCanvasCurveKey> keys;
    QRectF frame;
    bool valid = false;
};

QRectF curvePlotRect(const Node &entry) {
    const qreal top =
        entry.height - GraphCanvasCurvePlotHeight - GraphCanvasSectionPadding;
    const qreal width = std::max(1.0, entry.width - 2.0 * GraphCanvasCurvePlotMargin);
    return QRectF{GraphCanvasCurvePlotMargin, top, width, GraphCanvasCurvePlotHeight};
}

// The plot parts are children of the node item, so a press that misses the
// curve still lands on the node's own path. Qt would otherwise let a path
// item with the default mouse buttons accept it.
void readOnly(QGraphicsItem &item) { item.setAcceptedMouseButtons(Qt::NoButton); }

class CurveNodeBody final : public NodeBody {
  public:
    void prepare(Node &entry, const Builder &, const GraphCanvasModel &,
                 const GraphCanvasNode &node, const std::string &) const override {
        auto data = std::make_shared<CurvePlotData>();
        if (auto keys = curveKeysFromParameters(node.parameters)) {
            data->keys = std::move(*keys);
            data->frame = GraphCanvasCurveGeometry::curveFrame(data->keys);
            data->valid = true;
        }
        entry.body_data = std::move(data);
    }

    void measure(Node &entry, const Builder &builder) const override {
        defaultNodeBody().measure(entry, builder);
        if (entry.view == GraphCanvasNodeView::open)
            entry.height += GraphCanvasCurvePlotHeight + GraphCanvasSectionPadding;
    }

    void draw(Builder &builder, QGraphicsPathItem &item, const Node &entry) const override {
        defaultNodeBody().draw(builder, item, entry);
        if (entry.view != GraphCanvasNodeView::open) return;
        const auto *data = dynamic_cast<const CurvePlotData *>(entry.body_data.get());

        const QRectF plot = curvePlotRect(entry);
        QPainterPath frame;
        frame.addRect(plot);
        auto *frame_item = new QGraphicsPathItem(frame, &item);
        frame_item->setBrush(Qt::NoBrush);
        frame_item->setPen(QPen{builder.theme().canvas.node_border, 1.0});
        frame_item->setZValue(0.5);
        readOnly(*frame_item);
        builder.tagPart(*frame_item, entry, GraphCanvasCurveFrameItem);

        if (data == nullptr || !data->valid) return;
        const auto paths = GraphCanvasCurveGeometry::curvePaths(
            data->keys, data->frame, plot, 2.6, 64);

        auto *curve_item = new QGraphicsPathItem(paths.curve, &item);
        QPen curve_pen{builder.theme().domainTint(QStringLiteral("values")), 1.6};
        curve_pen.setCosmetic(true);
        curve_item->setPen(curve_pen);
        curve_item->setBrush(Qt::NoBrush);
        curve_item->setZValue(1.0);
        readOnly(*curve_item);
        builder.tagPart(*curve_item, entry, GraphCanvasCurvePlotItem);

        auto *keys_item = new QGraphicsPathItem(paths.keys, &item);
        keys_item->setPen(QPen{builder.theme().canvas.node_border, 1.0});
        keys_item->setBrush(builder.theme().domainTint(QStringLiteral("values")));
        keys_item->setZValue(1.5);
        readOnly(*keys_item);
        builder.tagPart(*keys_item, entry, GraphCanvasCurveKeysItem);
    }
};

} // namespace

const NodeBody &curveNodeBody() {
    static const CurveNodeBody body;
    return body;
}

} // namespace PelicanStudio::GraphCanvasDrawing
