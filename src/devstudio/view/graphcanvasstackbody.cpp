// NB-2b (docs/design_node_bodies.md 2.3): the body of a canvas.stack node --
// its layers as the rows of a paint program's Layers panel, the top row the
// layer drawn in front, each with the socket its layer's line ends on.
//
// The body draws; it never edits. Every part carries the stack (name role),
// the layer's input (port role) and its element of `layers` (index role), and
// the scene answers a press on it with the op list a model function builds
// (graphcanvaslayerstack), as the Layers dock does.
//
// NB-2c: a row opened by the small triangle at its left draws its stage under
// it -- the Opacity line (the `<input>.opacity` socket, and a slider or the
// name of the node whose line drives it) and, for a masked layer, the Mask
// line (its socket and the node it reads). A closed row, and every row of a
// compact node, keeps the small socket of a line that ends there.

#include "graphcanvasdrawing.hpp"

#include "graphcanvasthumbnails.hpp"
#include "studiotheme.hpp"

#include <QBrush>
#include <QGraphicsLineItem>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QPen>

#include <algorithm>
#include <cmath>

namespace PelicanStudio::GraphCanvasDrawing {
namespace {

QString qtext(const std::string &value) { return QString::fromStdString(value); }

// The composite's frame (NB-3 draws the stack's output in it) and its margin.
constexpr qreal CompositeSize = 64.0;
constexpr qreal CompositeMargin = 6.0;
constexpr qreal ThumbSize = 32.0;
constexpr qreal BandHeight = 28.0;
constexpr qreal BottomPadding = 4.0;
constexpr qreal StackMinimumWidth = 228.0;
// Where the parts of an open row stand, from the node's left edge.
constexpr qreal ExpandCentre = 11.5;
constexpr qreal ExpandSize = 11.0;
constexpr qreal EyeLeft = 19.0;
constexpr qreal EyeSize = 18.0;
constexpr qreal ThumbLeft = 38.0;
constexpr qreal NameLeft = 78.0;
// NB-2c: the stage of an open row -- its words at the left, the slider (or
// the line's name) from FieldLeft to the right edge less FieldRight.
constexpr qreal StageLabelLeft = 18.0;
constexpr qreal FieldLeft = 78.0;
constexpr qreal FieldRight = 12.0;
constexpr qreal FieldHeight = 15.0;
constexpr qreal StagePadding = 3.0;
// A row's second and third sockets (`<input>.opacity`, the mask) stand this
// far apart, a size down from the layer's.
constexpr qreal ExtraSocketStep = 9.0;
constexpr qreal ExtraSocketScale = 0.72;

const Row *rowFor(const std::vector<Row> &rows, const QString &port) {
    const auto found = std::ranges::find(rows, port, &Row::port);
    return found == rows.end() ? nullptr : &*found;
}

// SDK-1b: the data the stack's body prepared for an entry (a canvas.stack's
// body, so its Node always holds this kind).
const LayerStackData &layerData(const Node &entry) {
    return *static_cast<const LayerStackData *>(entry.body_data.get());
}

// The sockets one row draws: its layer's, and `<input>.opacity` and the mask
// only while a line ends on them (design 2.3). NB-2c: an open row of an open
// node draws those two on its stage instead, line or none.
struct RowSockets {
    const Row *layer = nullptr;
    std::vector<const Row *> extras;
};

bool stageShown(const Node &entry, const LayerLine &line) {
    return line.open && entry.view == GraphCanvasNodeView::open;
}

RowSockets socketsOf(const Node &entry, const LayerLine &line) {
    RowSockets sockets;
    sockets.layer = rowFor(entry.inputs, qtext(line.row.input));
    if (stageShown(entry, line)) return sockets;
    if (line.row.opacity_driven)
        if (const Row *opacity = rowFor(entry.inputs, qtext(graphCanvasLayerOpacityPort(line.row.input)));
            opacity != nullptr && opacity->connected)
            sockets.extras.push_back(opacity);
    if (!line.row.mask.empty() && !line.row.mask_source.empty())
        if (const Row *mask = rowFor(entry.inputs, qtext(line.row.mask)); mask != nullptr)
            sockets.extras.push_back(mask);
    return sockets;
}

qreal compositeHeight(const Node &entry) {
    return std::max(CompositeSize + 2.0 * CompositeMargin,
                    2.0 * GraphCanvasSectionPadding +
                        GraphCanvasSocketRowHeight * static_cast<qreal>(entry.outputs.size()));
}

qreal slimRowHeight(const Node &entry, const LayerLine &line) {
    return LayerSlimRowHeight + ExtraSocketStep * static_cast<qreal>(socketsOf(entry, line).extras.size());
}

// NB-2c: the stage's lines -- Opacity, and Mask when the layer has one.
std::size_t stageLines(const LayerLine &line) { return line.row.mask.empty() ? 1 : 2; }

qreal stageHeight(const Node &entry, const LayerLine &line) {
    if (!stageShown(entry, line)) return 0.0;
    return LayerStageLineHeight * static_cast<qreal>(stageLines(line)) + 2.0 * StagePadding;
}

qreal openRowHeight(const Node &entry, const LayerLine &line) { return LayerRowHeight + stageHeight(entry, line); }

QString percentText(double opacity) { return QStringLiteral("%1 %").arg(std::lround(opacity * 100.0)); }

QString opacityText(double opacity) {
    const long percent = std::lround(opacity * 100.0);
    return percent == 100 ? QString{} : QStringLiteral("%1%").arg(percent);
}

// One mark at the right of an open row, in the order they stand left to right.
struct Mark {
    const char *kind;
    QString text;
    QString tip;
};

std::vector<Mark> marksOf(const LayerLine &line) {
    std::vector<Mark> marks;
    if (!line.blend.isEmpty())
        marks.push_back({GraphCanvasStackBlendItem, line.blend, QObject::tr("Blend: %1").arg(line.blend)});
    if (const auto text = opacityText(line.row.opacity); !text.isEmpty())
        marks.push_back({GraphCanvasStackOpacityItem, text,
                         line.row.opacity_driven ? QObject::tr("Opacity %1: a value line drives it").arg(text)
                                                 : QObject::tr("Opacity %1: drag sideways to change it").arg(text)});
    if (!line.row.mask.empty())
        marks.push_back({GraphCanvasStackMaskItem, QObject::tr("mask"),
                         QObject::tr("Masked by %1").arg(qtext(line.row.mask_source))});
    if (line.enters)
        marks.push_back({GraphCanvasStackEnterItem, QString::fromUtf8("\xE2\x80\xBA"),
                         line.row.kind == "group" ? QObject::tr("Show this group's own stack")
                                                  : QObject::tr("Go into the imported canvas (read only)")});
    return marks;
}

constexpr qreal MarkGap = 7.0;

qreal marksWidth(const LayerLine &line, const QFontMetricsF &metrics) {
    qreal width = 0.0;
    for (const auto &mark : marksOf(line)) width += metrics.horizontalAdvance(mark.text) + MarkGap;
    return width;
}

// The kind, scope, node name, the layer's input and index, and the row's
// state on one part of a row.
void tagRowPart(const Builder &builder, QGraphicsItem &part, const Node &entry, const LayerLine &line,
                const char *kind) {
    builder.tagPart(part, entry, kind);
    part.setData(GraphCanvasPortRole, qtext(line.row.input));
    part.setData(GraphCanvasLayerIndexRole, static_cast<qulonglong>(line.row.index));
    part.setData(GraphCanvasLayerVisibleRole, line.row.visible);
    part.setData(GraphCanvasLayerPickedRole, layerData(entry).picked_layer == qtext(line.row.input));
    part.setData(GraphCanvasLayerReadOnlyRole, layerData(entry).read_only);
}

QColor pickedFill(const StudioTheme &theme) {
    QColor fill = theme.canvas.edge_selected;
    fill.setAlphaF(0.24F);
    return fill;
}

// The row's own area: what a press picks and a drag moves.
QGraphicsRectItem *addRowArea(const Builder &builder, QGraphicsItem &item, const Node &entry,
                              const LayerLine &line, const QRectF &area) {
    auto *row = new QGraphicsRectItem(area, &item);
    row->setPen(Qt::NoPen);
    const bool picked = layerData(entry).picked_layer == qtext(line.row.input);
    row->setBrush(picked ? QBrush{pickedFill(builder.theme())} : QBrush{QColor{0, 0, 0, 0}});
    QString tip = QObject::tr("%1 (%2, input %3)")
                      .arg(qtext(line.row.name), qtext(graphCanvasLayerKindText(line.row)), qtext(line.row.input));
    if (layerData(entry).read_only) tip += QObject::tr(" -- an imported canvas: read only");
    row->setToolTip(tip);
    tagRowPart(builder, *row, entry, line, GraphCanvasStackRowItem);
    return row;
}

void addRowSockets(Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line,
                   qreal first_centre) {
    const auto sockets = socketsOf(entry, line);
    if (sockets.layer != nullptr) builder.addSocket(item, entry, *sockets.layer, QPointF{0.0, first_centre}, "input");
    qreal centre = first_centre + ExtraSocketStep + 2.0;
    for (const Row *extra : sockets.extras) {
        Row small = *extra;
        small.style.size *= ExtraSocketScale;
        builder.addSocket(item, entry, small, QPointF{0.0, centre}, "input");
        centre += ExtraSocketStep;
    }
}

QGraphicsSimpleTextItem *addText(const Builder &builder, QGraphicsItem &parent, const QString &text,
                                 const QColor &colour) {
    auto *label = new QGraphicsSimpleTextItem(text, &parent);
    label->setFont(builder.detailFont());
    label->setBrush(colour);
    return label;
}

QPainterPath eyePath() {
    QPainterPath path;
    path.moveTo(-7.0, 0.0);
    path.quadTo(0.0, -6.5, 7.0, 0.0);
    path.quadTo(0.0, 6.5, -7.0, 0.0);
    path.closeSubpath();
    return path;
}

void addEye(const Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line,
            const QPointF &centre) {
    const StudioTheme &theme = builder.theme();
    // The press area is the square; the drawing inside it carries the same
    // data, since a press finds the topmost item first.
    auto *area = new QGraphicsRectItem(QRectF{-EyeSize / 2.0, -EyeSize / 2.0, EyeSize, EyeSize}, &item);
    area->setPos(centre);
    area->setPen(Qt::NoPen);
    area->setBrush(QColor{0, 0, 0, 0});
    area->setToolTip(layerData(entry).read_only ? QObject::tr("Shown or hidden (an imported canvas: read only)")
                     : line.row.visible    ? QObject::tr("Shown: click to hide the layer")
                                           : QObject::tr("Hidden: click to show the layer"));
    tagRowPart(builder, *area, entry, line, GraphCanvasStackEyeItem);
    auto *eye = new QGraphicsPathItem(eyePath(), area);
    eye->setPen(QPen{theme.canvas.heading_text, 1.1});
    eye->setBrush(Qt::NoBrush);
    tagRowPart(builder, *eye, entry, line, GraphCanvasStackEyeItem);
    if (line.row.visible) {
        QPainterPath pupil;
        pupil.addEllipse(QPointF{0.0, 0.0}, 2.4, 2.4);
        auto *dot = new QGraphicsPathItem(pupil, area);
        dot->setPen(Qt::NoPen);
        dot->setBrush(theme.canvas.heading_text);
        tagRowPart(builder, *dot, entry, line, GraphCanvasStackEyeItem);
    } else {
        eye->setOpacity(0.4);
        auto *slash = new QGraphicsLineItem(QLineF{-6.0, 5.0, 6.0, -5.0}, area);
        slash->setPen(QPen{theme.canvas.dim_text, 1.2});
        tagRowPart(builder, *slash, entry, line, GraphCanvasStackEyeItem);
    }
    if (layerData(entry).read_only) area->setOpacity(0.6);
}

// NB-3: the layer alone, drawn as its own stack would draw it (32 px).
void addThumbFrame(const Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line,
                   const QRectF &frame) {
    GraphCanvasThumbnailTarget target;
    target.kind = GraphCanvasThumbnailKind::layer;
    target.scope = builder.scope().toStdString();
    target.node = entry.node->id;
    target.input = line.row.input;
    target.pixels = graphCanvasLayerThumbnailPixels;
    auto *thumb = builder.addThumbnail(item, frame, target, qtext(graphCanvasLayerKindText(line.row)));
    tagRowPart(builder, *thumb, entry, line, GraphCanvasStackThumbItem);
}

void addName(const Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line,
             qreal left, qreal room, qreal centre_y) {
    const QFontMetricsF &metrics = builder.rowMetrics();
    const QString full = qtext(line.row.name);
    const QString shown = metrics.horizontalAdvance(full) > room
                              ? metrics.elidedText(full, Qt::ElideRight, std::max<qreal>(room, 12.0))
                              : full;
    QColor colour = builder.theme().canvas.heading_text;
    if (!line.row.visible || layerData(entry).read_only) colour.setAlphaF(0.55F);
    auto *name = addText(builder, item, shown, colour);
    name->setPos(left, centre_y - metrics.height() / 2.0);
    name->setToolTip(layerData(entry).read_only ? full : QObject::tr("%1 (double-click to rename)").arg(full));
    tagRowPart(builder, *name, entry, line, GraphCanvasStackNameItem);
    name->setData(GraphCanvasLabelRole, full);
}

// NB-2c: the small triangle at an open node's row's left: closed or open.
void addExpand(const Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line,
               qreal centre_y) {
    auto *area = new QGraphicsRectItem(QRectF{-ExpandSize / 2.0, -ExpandSize / 2.0, ExpandSize, ExpandSize}, &item);
    area->setPos(ExpandCentre, centre_y);
    area->setPen(Qt::NoPen);
    area->setBrush(QColor{0, 0, 0, 0});
    area->setToolTip(line.open ? QObject::tr("Close the row") : QObject::tr("Open the row: its opacity and mask"));
    tagRowPart(builder, *area, entry, line, GraphCanvasStackExpandItem);
    area->setData(GraphCanvasLayerOpenRole, line.open);
    QPainterPath arrow;
    if (line.open) {
        arrow.moveTo(-3.5, -2.0);
        arrow.lineTo(3.5, -2.0);
        arrow.lineTo(0.0, 2.5);
    } else {
        arrow.moveTo(-2.0, -3.5);
        arrow.lineTo(2.5, 0.0);
        arrow.lineTo(-2.0, 3.5);
    }
    arrow.closeSubpath();
    auto *drawn = new QGraphicsPathItem(arrow, area);
    drawn->setPen(Qt::NoPen);
    drawn->setBrush(builder.theme().canvas.dim_text);
    tagRowPart(builder, *drawn, entry, line, GraphCanvasStackExpandItem);
    drawn->setData(GraphCanvasLayerOpenRole, line.open);
}

// NB-2c: `text`, cut to `room` at the right.
QString elided(const QFontMetricsF &metrics, const QString &text, qreal room) {
    return metrics.horizontalAdvance(text) > room
               ? metrics.elidedText(text, Qt::ElideRight, std::max<qreal>(room, 12.0))
               : text;
}

// NB-2c: "<- <node>", where the line into the stage's socket comes from.
void addLineSource(const Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line,
                   const char *kind, const QString &from, const QString &tip, qreal right, qreal centre_y) {
    const QFontMetricsF &metrics = builder.rowMetrics();
    const QString full = QString::fromUtf8("\xE2\x86\x90 ") + from;
    auto *text = addText(builder, item, elided(metrics, full, right - FieldLeft), builder.theme().canvas.heading_text);
    text->setPos(FieldLeft, centre_y - metrics.height() / 2.0);
    text->setToolTip(tip);
    tagRowPart(builder, *text, entry, line, kind);
    text->setData(GraphCanvasValueTextRole, full);
}

// NB-2c: an open row's stage, from `top` down: the Opacity line (its socket,
// and the slider or the driving line's node) and the mask's.
void addStage(Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line, qreal top) {
    const StudioTheme &theme = builder.theme();
    const QFontMetricsF &metrics = builder.rowMetrics();
    const qreal width = entry.width;
    auto *stage = new QGraphicsRectItem(QRectF{0.0, top, width, stageHeight(entry, line)}, &item);
    stage->setPen(Qt::NoPen);
    QColor tint = theme.canvas.pill_bg;
    tint.setAlphaF(0.35F);
    stage->setBrush(tint);
    stage->setToolTip(QObject::tr("%1: its opacity and mask").arg(qtext(line.row.name)));
    tagRowPart(builder, *stage, entry, line, GraphCanvasStackStageItem);

    const qreal right = width - FieldRight;
    const auto label = [&](const QString &words, qreal centre_y) {
        auto *said = addText(builder, item, words, theme.canvas.dim_text);
        said->setPos(StageLabelLeft, centre_y - metrics.height() / 2.0);
        tagRowPart(builder, *said, entry, line, GraphCanvasStackStageItem);
    };

    // Opacity: `<input>.opacity`'s socket takes a value line.
    qreal centre_y = top + StagePadding + LayerStageLineHeight / 2.0;
    if (const Row *opacity = rowFor(entry.inputs, qtext(graphCanvasLayerOpacityPort(line.row.input))))
        builder.addSocket(item, entry, *opacity, QPointF{0.0, centre_y}, "input");
    label(QObject::tr("Opacity"), centre_y);
    if (line.row.opacity_driven) {
        addLineSource(builder, item, entry, line, GraphCanvasStackOpacitySourceItem,
                      line.opacity_from.isEmpty() ? QObject::tr("a line") : line.opacity_from,
                      QObject::tr("A value line from %1 drives the opacity (%2 stored)")
                          .arg(line.opacity_from, percentText(line.row.opacity)),
                      right, centre_y);
    } else {
        // The slider: the track, its filled part and the percent on it.
        const qreal field_width = std::max<qreal>(right - FieldLeft, 40.0);
        auto *track = new QGraphicsPathItem(roundedEntityPath(field_width, FieldHeight, 4.0), &item);
        track->setPos(FieldLeft, centre_y - FieldHeight / 2.0);
        track->setBrush(theme.canvas.pill_bg);
        track->setPen(QPen{theme.canvas.node_border, 0.8});
        track->setToolTip(layerData(entry).read_only
                              ? QObject::tr("Opacity %1 (an imported canvas: read only)")
                                    .arg(percentText(line.row.opacity))
                              : QObject::tr("Opacity %1: drag sideways to change it, double-click to type it")
                                    .arg(percentText(line.row.opacity)));
        tagRowPart(builder, *track, entry, line, GraphCanvasStackOpacityFieldItem);
        track->setData(GraphCanvasValueTextRole, percentText(line.row.opacity));
        QColor fill = theme.canvas.edge_selected;
        fill.setAlphaF(0.45F);
        auto *bar = new QGraphicsRectItem(
            QRectF{1.0, 1.0, std::clamp(line.row.opacity, 0.0, 1.0) * (field_width - 2.0), FieldHeight - 2.0}, track);
        bar->setPen(Qt::NoPen);
        bar->setBrush(fill);
        tagRowPart(builder, *bar, entry, line, GraphCanvasStackOpacityFieldItem);
        auto *text = addText(builder, *track, percentText(line.row.opacity), theme.canvas.heading_text);
        text->setPos((field_width - metrics.horizontalAdvance(text->text())) / 2.0,
                     (FieldHeight - metrics.height()) / 2.0);
        tagRowPart(builder, *text, entry, line, GraphCanvasStackOpacityFieldItem);
        text->setData(GraphCanvasValueTextRole, text->text());
        if (layerData(entry).read_only) track->setOpacity(0.6);
    }

    // Mask: the layer's mask input, the line into it or none.
    if (line.row.mask.empty()) return;
    centre_y += LayerStageLineHeight;
    if (const Row *mask = rowFor(entry.inputs, qtext(line.row.mask)))
        builder.addSocket(item, entry, *mask, QPointF{0.0, centre_y}, "input");
    label(QObject::tr("Mask"), centre_y);
    addLineSource(builder, item, entry, line, GraphCanvasStackMaskSourceItem,
                  line.mask_from.isEmpty() ? QObject::tr("no line") : line.mask_from,
                  line.mask_from.isEmpty() ? QObject::tr("The mask (%1) has no line").arg(qtext(line.row.mask))
                                           : QObject::tr("Masked by %1 (input %2)")
                                                 .arg(line.mask_from, qtext(line.row.mask)),
                  right, centre_y);
}

class LayerStackNodeBody final : public NodeBody {
  public:
    void prepare(Node &entry, const Builder &builder, const GraphCanvasModel &model,
                 const GraphCanvasNode &node, const std::string &scope) const override {
        // SDK-1b: the canvas's UI state is fetched once, here; the data keeps
        // values only, so nothing holds a node or the scene's state after.
        const auto state = builder.bodyState(scope, node);
        auto data = std::make_shared<LayerStackData>();
        data->read_only = builder.layerScopeReadOnlyCached(model, scope);
        data->picked_layer = state.picked_layer;
        // NB-5: a shape in the air is drawn for the rows, not the document's.
        data->pending = state.layer_shape_parameters.has_value();
        auto nodes = model.nodes(scope);
        if (state.layer_shape_parameters)
            if (const auto found = std::ranges::find(nodes, node.id, &GraphCanvasNode::id); found != nodes.end())
                found->parameters = *state.layer_shape_parameters;
        const auto &blends = graphCanvasBlendChoices();
        for (auto &row : graphCanvasLayerRows(nodes, node.id)) {
            LayerLine line;
            if (row.blend != "normal") {
                const auto choice = std::ranges::find(blends, row.blend, &GraphCanvasBlendChoice::name);
                line.blend = qtext(choice != blends.end() ? choice->label : row.blend);
            }
            line.enters = (row.kind == "group" && !row.source.empty()) ||
                          (row.kind == "import" && !row.definition.empty());
            // NB-2c: the open rows, and the nodes the stage names.
            line.open = std::ranges::find(state.open_rows, row.input) != state.open_rows.end();
            const auto heading_of = [&](const std::string &id) {
                const auto found = std::ranges::find(nodes, id, &GraphCanvasNode::id);
                return found == nodes.end() ? qtext(id) : qtext(found->heading());
            };
            const auto opacity_port = graphCanvasLayerOpacityPort(row.input);
            if (const auto port = std::ranges::find(node.inputs, opacity_port, &GraphCanvasPort::port);
                port != node.inputs.end() && port->connected)
                line.opacity_from = heading_of(port->source_node);
            if (!row.mask_source.empty()) line.mask_from = heading_of(row.mask_source);
            line.row = std::move(row);
            data->rows.push_back(std::move(line));
        }
        entry.body_data = std::move(data);
    }

    void measure(Node &entry, const Builder &builder) const override {
        const QFontMetricsF &metrics = builder.rowMetrics();
        qreal width = std::max(entry.width, StackMinimumWidth);
        for (const auto &row : entry.outputs)
            width = std::max(width, 14.0 + CompositeSize + 16.0 + metrics.horizontalAdvance(row.label) + 22.0);
        for (const auto &line : layerData(entry).rows)
            width = std::max(width, NameLeft + metrics.horizontalAdvance(qtext(line.row.name)) + 12.0 +
                                        marksWidth(line, metrics) + 12.0);
        entry.width = std::min(width, GraphCanvasNodeMaximumWidth);

        // Every socket row is drawn by the parts that stand for it, or not
        // at all; the rows keep their own height for whoever reads it.
        for (auto *side : {&entry.outputs, &entry.inputs})
            for (Row &row : *side) {
                row.height = GraphCanvasSocketRowHeight;
                row.shown = entry.view != GraphCanvasNodeView::minimal;
            }
        switch (entry.view) {
        case GraphCanvasNodeView::open: {
            qreal rows = 0.0;
            for (const auto &line : layerData(entry).rows) rows += openRowHeight(entry, line);
            entry.height = GraphCanvasHeaderHeight + compositeHeight(entry) + rows + BandHeight + BottomPadding;
            break;
        }
        case GraphCanvasNodeView::compact: {
            qreal rows = 0.0;
            for (const auto &line : layerData(entry).rows) rows += slimRowHeight(entry, line);
            entry.height = GraphCanvasHeaderHeight + compositeHeight(entry) + rows + GraphCanvasSectionPadding;
            break;
        }
        case GraphCanvasNodeView::minimal:
            entry.height = GraphCanvasHeaderHeight;
            break;
        }
    }

    void draw(Builder &builder, QGraphicsPathItem &item, const Node &entry) const override {
        const qreal width = entry.width;
        const StudioTheme &theme = builder.theme();
        const QFontMetricsF &metrics = builder.rowMetrics();
        // NB-5: on the band, left of the operation's name, while the engine
        // has not answered for the rows drawn.
        if (layerData(entry).pending) {
            const QString op = qtext(entry.node->badge.empty() ? entry.node->op : entry.node->badge);
            const QString dots = QString::fromUtf8("\xE2\x8B\xAF");
            auto *mark = addText(builder, item, dots, theme.canvas.heading_text);
            mark->setPos(width - 10.0 - metrics.horizontalAdvance(op) - 8.0 - metrics.horizontalAdvance(dots),
                         (GraphCanvasHeaderHeight - metrics.height()) / 2.0);
            mark->setZValue(1.0);
            mark->setAcceptedMouseButtons(Qt::NoButton);
            mark->setToolTip(QObject::tr("Saving: the engine has not answered for these rows yet"));
            builder.tagPart(*mark, entry, GraphCanvasStackPendingItem);
        }
        if (entry.view == GraphCanvasNodeView::minimal) {
            const qreal centre_y = GraphCanvasHeaderHeight / 2.0;
            if (!entry.inputs.empty()) builder.addAnchor(item, entry, QPointF{0.0, centre_y}, "input");
            if (!entry.outputs.empty()) builder.addAnchor(item, entry, QPointF{width, centre_y}, "output");
            return;
        }

        // The composite and the output it hands on.
        qreal top = GraphCanvasHeaderHeight;
        const qreal section = compositeHeight(entry);
        // NB-3: the stack's output, as a canvas.output reading it draws it.
        GraphCanvasThumbnailTarget target;
        target.kind = GraphCanvasThumbnailKind::composite;
        target.scope = builder.scope().toStdString();
        target.node = entry.node->id;
        target.pixels = graphCanvasThumbnailPixels;
        auto *composite = builder.addThumbnail(
            item, QRectF{14.0, top + (section - CompositeSize) / 2.0, CompositeSize, CompositeSize}, target,
            QObject::tr("The stack's composite"));
        builder.tagPart(*composite, entry, GraphCanvasStackCompositeItem);
        for (std::size_t at = 0; at < entry.outputs.size(); ++at) {
            const Row &output = entry.outputs[at];
            const qreal centre_y = entry.outputs.size() == 1
                                       ? top + section / 2.0
                                       : top + GraphCanvasSectionPadding +
                                             GraphCanvasSocketRowHeight * (static_cast<qreal>(at) + 0.5);
            auto *name = addText(builder, item, output.label, theme.canvas.dim_text);
            name->setPos(width - 14.0 - metrics.horizontalAdvance(output.label), centre_y - metrics.height() / 2.0);
            builder.tagPart(*name, entry, GraphCanvasNodeLabelItem);
            name->setData(GraphCanvasPortRole, output.port);
            builder.addSocket(item, entry, output, QPointF{width, centre_y}, "output");
        }
        top += section;
        QColor rule = theme.canvas.node_border;
        auto *line_under = new QGraphicsLineItem(QLineF{0.0, top, width, top}, &item);
        line_under->setPen(QPen{rule, 0.8});

        if (entry.view == GraphCanvasNodeView::compact) {
            for (const auto &line : layerData(entry).rows) {
                const qreal height = slimRowHeight(entry, line);
                addRowArea(builder, item, entry, line, QRectF{0.0, top, width, height});
                const qreal centre_y = top + LayerSlimRowHeight / 2.0;
                addRowSockets(builder, item, entry, line, centre_y);
                addName(builder, item, entry, line, 14.0, width - 28.0, centre_y);
                top += height;
            }
            return;
        }

        // Open: the rows, top first.
        rule.setAlphaF(0.45F);
        for (const auto &line : layerData(entry).rows) {
            const qreal row_height = openRowHeight(entry, line);
            addRowArea(builder, item, entry, line, QRectF{0.0, top, width, row_height});
            const auto sockets = socketsOf(entry, line);
            const qreal centre_y = top + LayerRowHeight / 2.0;
            addRowSockets(builder, item, entry, line, sockets.extras.empty() ? centre_y : top + 11.0);
            addExpand(builder, item, entry, line, centre_y);
            addEye(builder, item, entry, line, QPointF{EyeLeft + EyeSize / 2.0 - 3.0, centre_y});
            addThumbFrame(builder, item, entry, line,
                          QRectF{ThumbLeft, top + (LayerRowHeight - ThumbSize) / 2.0, ThumbSize, ThumbSize});
            // The marks from the right edge in, then the name in what is left.
            qreal right = width - 10.0;
            const auto marks = marksOf(line);
            for (auto mark = marks.rbegin(); mark != marks.rend(); ++mark) {
                const qreal advance = metrics.horizontalAdvance(mark->text);
                QColor colour = theme.canvas.dim_text;
                if (std::string_view{mark->kind} == GraphCanvasStackEnterItem) colour = theme.canvas.heading_text;
                auto *text = addText(builder, item, mark->text, colour);
                text->setPos(right - advance, centre_y - metrics.height() / 2.0);
                text->setToolTip(mark->tip);
                tagRowPart(builder, *text, entry, line, mark->kind);
                text->setData(GraphCanvasValueTextRole, mark->text);
                right -= advance + MarkGap;
            }
            addName(builder, item, entry, line, NameLeft, right - NameLeft - 4.0, centre_y);
            if (stageShown(entry, line)) addStage(builder, item, entry, line, top + LayerRowHeight);
            auto *between =
                new QGraphicsLineItem(QLineF{8.0, top + row_height, width - 8.0, top + row_height}, &item);
            between->setPen(QPen{rule, 0.6});
            top += row_height;
        }

        // The band: + ▾, the bin, `+ layer` (a line dropped on it is a layer
        // on top) and the note that says which way the rows overlap.
        const qreal centre_y = top + BandHeight / 2.0;
        if (!layerData(entry).read_only) {
            if (const Row *add = rowFor(entry.inputs, qtext(std::string{graphCanvasStackAddPort})))
                builder.addSocket(item, entry, *add, QPointF{0.0, centre_y}, "input");
            const auto button = [&](qreal left, qreal button_width, const char *kind, const QString &tip,
                                    bool enabled) {
                auto *pressed = new QGraphicsPathItem(roundedEntityPath(button_width, 18.0, 5.0), &item);
                pressed->setPos(left, centre_y - 9.0);
                pressed->setBrush(theme.canvas.pill_bg);
                pressed->setPen(QPen{theme.canvas.node_border, 0.8});
                pressed->setToolTip(tip);
                pressed->setOpacity(enabled ? 1.0 : 0.45);
                builder.tagPart(*pressed, entry, kind);
                pressed->setData(GraphCanvasLayerReadOnlyRole, false);
                return pressed;
            };
            auto *add = button(14.0, 34.0, GraphCanvasStackAddItem,
                               QObject::tr("Add a layer just above the picked one (on top when none is picked)"), true);
            auto *plus = addText(builder, *add, QString::fromUtf8("+ \xE2\x96\xBE"), theme.canvas.heading_text);
            plus->setPos((34.0 - metrics.horizontalAdvance(plus->text())) / 2.0, (18.0 - metrics.height()) / 2.0);
            builder.tagPart(*plus, entry, GraphCanvasStackAddItem);
            const bool picked =
                !layerData(entry).picked_layer.empty() && std::ranges::any_of(layerData(entry).rows, [&](const LayerLine &line) {
                    return qtext(line.row.input) == layerData(entry).picked_layer;
                });
            auto *bin = button(54.0, 22.0, GraphCanvasStackDeleteItem,
                               picked ? QObject::tr("Delete the picked layer (Delete)")
                                      : QObject::tr("Delete the picked layer: pick a row first"),
                               picked);
            QPainterPath can;
            can.moveTo(6.0, 5.5);
            can.lineTo(16.0, 5.5);
            can.moveTo(9.0, 5.5);
            can.lineTo(9.5, 3.5);
            can.lineTo(12.5, 3.5);
            can.lineTo(13.0, 5.5);
            can.moveTo(7.0, 6.5);
            can.lineTo(8.0, 15.0);
            can.lineTo(14.0, 15.0);
            can.lineTo(15.0, 6.5);
            auto *drawn = new QGraphicsPathItem(can, bin);
            drawn->setPen(QPen{theme.canvas.heading_text, 1.1});
            builder.tagPart(*drawn, entry, GraphCanvasStackDeleteItem);
        }
        const QString note = QObject::tr("\xE2\x86\x91 in front");
        auto *said = addText(builder, item, note, theme.canvas.dim_text);
        said->setOpacity(0.8);
        said->setPos(width - 10.0 - metrics.horizontalAdvance(note), centre_y - metrics.height() / 2.0);
        said->setToolTip(layerData(entry).read_only
                             ? QObject::tr("A row is drawn over the rows under it. An imported canvas: read only.")
                             : QObject::tr("A row is drawn over the rows under it."));
        builder.tagPart(*said, entry, GraphCanvasStackNoteItem);
    }
};

} // namespace

const NodeBody &layerStackNodeBody() {
    static const LayerStackNodeBody body;
    return body;
}

bool layerScopeReadOnly(const GraphCanvasModel &model, std::string_view scope) {
    if (scope.empty()) return false;
    if (graphCanvasImportedDefinition(model.nodes(std::string_view{}), scope)) return true;
    for (const auto &each : model.scopes())
        if (!each.root && each.id != scope && graphCanvasImportedDefinition(model.nodes(each.id), scope))
            return true;
    return false;
}

} // namespace PelicanStudio::GraphCanvasDrawing
