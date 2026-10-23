// NB-2b (docs/design_node_bodies.md 2.3): the body of a canvas.stack node --
// its layers as the rows of a paint program's Layers panel, the top row the
// layer drawn in front, each with the socket its layer's line ends on.
//
// The body draws; it never edits. Every part carries the stack (name role),
// the layer's input (port role) and its element of `layers` (index role), and
// the scene answers a press on it with the op list a model function builds
// (graphcanvaslayerstack), as the Layers dock does.

#include "graphcanvasdrawing.hpp"

#include "graphcanvaswidget.hpp"
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
constexpr qreal EyeLeft = 13.0;
constexpr qreal EyeSize = 18.0;
constexpr qreal ThumbLeft = 36.0;
constexpr qreal NameLeft = 76.0;
// A row's second and third sockets (`<input>.opacity`, the mask) stand this
// far apart, a size down from the layer's.
constexpr qreal ExtraSocketStep = 9.0;
constexpr qreal ExtraSocketScale = 0.72;

const Row *rowFor(const std::vector<Row> &rows, const QString &port) {
    const auto found = std::ranges::find(rows, port, &Row::port);
    return found == rows.end() ? nullptr : &*found;
}

// The sockets one row draws: its layer's, and `<input>.opacity` and the mask
// only while a line ends on them (design 2.3).
struct RowSockets {
    const Row *layer = nullptr;
    std::vector<const Row *> extras;
};

RowSockets socketsOf(const Node &entry, const LayerLine &line) {
    RowSockets sockets;
    sockets.layer = rowFor(entry.inputs, qtext(line.row.input));
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
    if (!line.filters.isEmpty()) marks.push_back({GraphCanvasStackFiltersItem, QStringLiteral("f"), line.filters});
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
    part.setData(GraphCanvasLayerPickedRole, entry.picked_layer == qtext(line.row.input));
    part.setData(GraphCanvasLayerReadOnlyRole, entry.layers_read_only);
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
    const bool picked = entry.picked_layer == qtext(line.row.input);
    row->setBrush(picked ? QBrush{pickedFill(builder.theme())} : QBrush{QColor{0, 0, 0, 0}});
    QString tip = QObject::tr("%1 (%2, input %3)")
                      .arg(qtext(line.row.name), qtext(graphCanvasLayerKindText(line.row)), qtext(line.row.input));
    if (entry.layers_read_only) tip += QObject::tr(" -- an imported canvas: read only");
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
    area->setToolTip(entry.layers_read_only ? QObject::tr("Shown or hidden (an imported canvas: read only)")
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
    if (entry.layers_read_only) area->setOpacity(0.6);
}

void addThumbFrame(const Builder &builder, QGraphicsItem &item, const Node &entry, const LayerLine &line,
                   const QRectF &frame) {
    // NB-3 draws the layer here; until then the frame is empty.
    auto *thumb = new QGraphicsRectItem(frame, &item);
    thumb->setBrush(builder.theme().canvas.pill_bg);
    thumb->setPen(QPen{builder.theme().canvas.node_border, 0.8});
    thumb->setToolTip(qtext(graphCanvasLayerKindText(line.row)));
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
    if (!line.row.visible || entry.layers_read_only) colour.setAlphaF(0.55F);
    auto *name = addText(builder, item, shown, colour);
    name->setPos(left, centre_y - metrics.height() / 2.0);
    name->setToolTip(entry.layers_read_only ? full : QObject::tr("%1 (double-click to rename)").arg(full));
    tagRowPart(builder, *name, entry, line, GraphCanvasStackNameItem);
    name->setData(GraphCanvasLabelRole, full);
}

class LayerStackNodeBody final : public NodeBody {
  public:
    void measure(Node &entry, const Builder &builder) const override {
        const QFontMetricsF &metrics = builder.rowMetrics();
        qreal width = std::max(entry.width, StackMinimumWidth);
        for (const auto &row : entry.outputs)
            width = std::max(width, 14.0 + CompositeSize + 16.0 + metrics.horizontalAdvance(row.label) + 22.0);
        for (const auto &line : entry.layers)
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
        case GraphCanvasNodeView::open:
            entry.height = GraphCanvasHeaderHeight + compositeHeight(entry) +
                           LayerRowHeight * static_cast<qreal>(entry.layers.size()) + BandHeight + BottomPadding;
            break;
        case GraphCanvasNodeView::compact: {
            qreal rows = 0.0;
            for (const auto &line : entry.layers) rows += slimRowHeight(entry, line);
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
        if (entry.view == GraphCanvasNodeView::minimal) {
            const qreal centre_y = GraphCanvasHeaderHeight / 2.0;
            if (!entry.inputs.empty()) builder.addAnchor(item, entry, QPointF{0.0, centre_y}, "input");
            if (!entry.outputs.empty()) builder.addAnchor(item, entry, QPointF{width, centre_y}, "output");
            return;
        }

        // The composite and the output it hands on.
        qreal top = GraphCanvasHeaderHeight;
        const qreal section = compositeHeight(entry);
        auto *composite = new QGraphicsRectItem(
            QRectF{14.0, top + (section - CompositeSize) / 2.0, CompositeSize, CompositeSize}, &item);
        composite->setBrush(theme.canvas.pill_bg);
        composite->setPen(QPen{theme.canvas.node_border, 0.8});
        composite->setToolTip(QObject::tr("The stack's composite"));
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
            for (const auto &line : entry.layers) {
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
        for (const auto &line : entry.layers) {
            addRowArea(builder, item, entry, line, QRectF{0.0, top, width, LayerRowHeight});
            const auto sockets = socketsOf(entry, line);
            const qreal centre_y = top + LayerRowHeight / 2.0;
            addRowSockets(builder, item, entry, line, sockets.extras.empty() ? centre_y : top + 11.0);
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
                if (std::string_view{mark->kind} == GraphCanvasStackFiltersItem) {
                    QFont font = builder.detailFont();
                    font.setItalic(true);
                    font.setBold(true);
                    text->setFont(font);
                }
                text->setPos(right - advance, centre_y - metrics.height() / 2.0);
                text->setToolTip(mark->tip);
                tagRowPart(builder, *text, entry, line, mark->kind);
                text->setData(GraphCanvasValueTextRole, mark->text);
                right -= advance + MarkGap;
            }
            addName(builder, item, entry, line, NameLeft, right - NameLeft - 4.0, centre_y);
            auto *between = new QGraphicsLineItem(QLineF{8.0, top + LayerRowHeight, width - 8.0, top + LayerRowHeight},
                                                  &item);
            between->setPen(QPen{rule, 0.6});
            top += LayerRowHeight;
        }

        // The band: + ▾, the bin, `+ layer` (a line dropped on it is a layer
        // on top) and the note that says which way the rows overlap.
        const qreal centre_y = top + BandHeight / 2.0;
        if (!entry.layers_read_only) {
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
                !entry.picked_layer.isEmpty() && std::ranges::any_of(entry.layers, [&](const LayerLine &line) {
                    return qtext(line.row.input) == entry.picked_layer;
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
        said->setToolTip(entry.layers_read_only
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
