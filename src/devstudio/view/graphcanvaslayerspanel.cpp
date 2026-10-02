#include "graphcanvaslayerspanel.hpp"

#include "../model/graphcanvasmodel.hpp"
#include "studiotheme.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

QString named(const char *suffix) {
    return QStringLiteral("pelican.canvasLayers.") + QLatin1String{suffix};
}

QLabel *caption(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setFont(studioSmallFont());
    return label;
}

QDoubleSpinBox *numberSpin(QWidget *parent, const QString &name, double low, double high) {
    auto *spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(low, high);
    spin->setDecimals(1);
    spin->setSingleStep(1.0);
    spin->setKeyboardTracking(false);
    return spin;
}

const GraphCanvasNode *findNode(const std::vector<GraphCanvasNode> &nodes, const std::string &id) {
    const auto found = std::ranges::find(nodes, id, &GraphCanvasNode::id);
    return found == nodes.end() ? nullptr : &*found;
}

double number(const Json &parameters, const char *key, std::size_t component, double fallback) {
    const auto found = parameters.find(key);
    if (found == parameters.end()) return fallback;
    if (found->is_array() && component < found->size() && found->at(component).is_number())
        return found->at(component).get<double>();
    if (found->is_number()) return found->get<double>();
    return fallback;
}

bool driven(const GraphCanvasNode &node, const std::string &port) {
    return std::ranges::any_of(node.inputs, [&](const GraphCanvasPort &input) {
        return input.port == port && input.connected;
    });
}

QPixmap swatchIcon(const std::array<double, 4> &color) {
    QPixmap pixmap{12, 12};
    pixmap.fill(QColor::fromRgbF(static_cast<float>(color[0]), static_cast<float>(color[1]),
                                 static_cast<float>(color[2])));
    return pixmap;
}

} // namespace

GraphCanvasLayersPanel::GraphCanvasLayersPanel(GraphCanvasModel &model, std::function<void()> submitted,
                                               QWidget *parent)
    : QWidget(parent), model_(model), submitted_(std::move(submitted)) {
    setObjectName(QStringLiteral("pelican.canvasLayers"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    title_ = new QLabel(this);
    title_->setObjectName(named("title"));
    title_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(title_);

    rows_ = new QWidget(this);
    rows_layout_ = new QVBoxLayout(rows_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(2);
    layout->addWidget(rows_);
    empty_ = caption(tr("No layer yet: + Rect or + Ellipse adds one on top."), this);
    empty_->setObjectName(named("empty"));
    layout->addWidget(empty_);

    const auto button = [this](const QString &text, const char *name, const QString &tip) {
        auto *pressed = new QPushButton(text, this);
        pressed->setObjectName(named(name));
        pressed->setToolTip(tip);
        return pressed;
    };
    auto *adds = new QHBoxLayout;
    adds->setContentsMargins(0, 0, 0, 0);
    add_rect_ = button(tr("+ Rect"), "addRect",
                       tr("A filled rectangle at the middle of the canvas, a quarter of its size, on top"));
    add_ellipse_ = button(tr("+ Ellipse"), "addEllipse",
                          tr("A filled ellipse at the middle of the canvas, a quarter of its size, on top"));
    add_group_ = button(tr("+ Group"), "addGroup", tr("An empty stack of its own, on top"));
    add_stroke_ = button(tr("+ Stroke"), "addStroke",
                         tr("An outline of the picked layer's shape, just above it"));
    for (auto *each : {add_rect_, add_ellipse_, add_group_, add_stroke_}) adds->addWidget(each);
    adds->addStretch(1);
    layout->addLayout(adds);
    auto *moves = new QHBoxLayout;
    moves->setContentsMargins(0, 0, 0, 0);
    delete_ = button(tr("Delete"), "delete",
                     tr("The picked layer, and the canvas nodes nothing else uses"));
    up_ = button(QStringLiteral("\xE2\x86\x91"), "up", tr("Move the picked layer up one"));
    down_ = button(QStringLiteral("\xE2\x86\x93"), "down", tr("Move the picked layer down one"));
    for (auto *each : {delete_, up_, down_}) moves->addWidget(each);
    moves->addStretch(1);
    layout->addLayout(moves);

    connect(add_rect_, &QPushButton::clicked, this, [this] {
        commit(graphCanvasAddShapeLayerOps(model_.nodes(scope_), scope_, stack_, "rect"));
    });
    connect(add_ellipse_, &QPushButton::clicked, this, [this] {
        commit(graphCanvasAddShapeLayerOps(model_.nodes(scope_), scope_, stack_, "ellipse"));
    });
    connect(add_group_, &QPushButton::clicked, this, [this] {
        commit(graphCanvasAddGroupLayerOps(model_.nodes(scope_), scope_, stack_));
    });
    connect(add_stroke_, &QPushButton::clicked, this, [this] {
        if (const auto row = pickedRow())
            commit(graphCanvasAddStrokeLayerOps(model_.nodes(scope_), scope_, stack_, row->index));
    });
    connect(delete_, &QPushButton::clicked, this, [this] {
        if (const auto row = pickedRow())
            commit(graphCanvasDeleteLayerOps(model_.nodes(scope_), scope_, stack_, row->index));
    });
    connect(up_, &QPushButton::clicked, this, [this] {
        if (const auto row = pickedRow())
            commit(graphCanvasMoveLayerOps(model_.nodes(scope_), scope_, stack_, row->index, 1));
    });
    connect(down_, &QPushButton::clicked, this, [this] {
        if (const auto row = pickedRow())
            commit(graphCanvasMoveLayerOps(model_.nodes(scope_), scope_, stack_, row->index, -1));
    });

    // The picked row's shape: in the stack's pixels, origin top left, +y down.
    shape_fields_ = new QWidget(this);
    shape_fields_->setObjectName(named("shape"));
    auto *shape_form = new QFormLayout(shape_fields_);
    shape_form->setContentsMargins(0, 4, 0, 0);
    shape_caption_ = caption({}, shape_fields_);
    shape_caption_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    shape_form->addRow(shape_caption_);
    const auto pair = [&](const char *key, QDoubleSpinBox *&x, QDoubleSpinBox *&y, double low) {
        auto *row = new QWidget(shape_fields_);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        x = numberSpin(row, named("field.") + QLatin1String{key} + QStringLiteral(".x"), low, 1.0e6);
        y = numberSpin(row, named("field.") + QLatin1String{key} + QStringLiteral(".y"), low, 1.0e6);
        row_layout->addWidget(x, 1);
        row_layout->addWidget(y, 1);
        return row;
    };
    shape_form->addRow(caption(tr("Size"), shape_fields_), pair("size", size_x_, size_y_, 0.1));
    shape_form->addRow(caption(tr("Center"), shape_fields_), pair("center", center_x_, center_y_, -1.0e6));
    corner_row_ = new QWidget(shape_fields_);
    auto *corner_layout = new QHBoxLayout(corner_row_);
    corner_layout->setContentsMargins(0, 0, 0, 0);
    corner_radius_ = numberSpin(corner_row_, named("field.corner_radius"), 0.0, 1.0e6);
    corner_layout->addWidget(corner_radius_);
    shape_form->addRow(caption(tr("Corner radius"), shape_fields_), corner_row_);
    rotation_ = numberSpin(shape_fields_, named("field.rotation"), -360.0, 360.0);
    rotation_->setSuffix(QStringLiteral("\xC2\xB0"));
    shape_form->addRow(caption(tr("Rotation"), shape_fields_), rotation_);
    layout->addWidget(shape_fields_);
    const auto commitPair = [this](const char *key, QDoubleSpinBox *x, QDoubleSpinBox *y) {
        if (loading_) return;
        const auto row = pickedRow();
        if (!row || row->shape.empty()) return;
        commitParameter(row->shape, key, Json::array({x->value(), y->value()}));
    };
    for (auto *spin : {size_x_, size_y_})
        connect(spin, &QDoubleSpinBox::valueChanged, this,
                [this, commitPair] { commitPair("size", size_x_, size_y_); });
    for (auto *spin : {center_x_, center_y_})
        connect(spin, &QDoubleSpinBox::valueChanged, this,
                [this, commitPair] { commitPair("center", center_x_, center_y_); });
    const auto commitNumber = [this](const char *key, QDoubleSpinBox *spin) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double value) {
            if (loading_) return;
            const auto row = pickedRow();
            if (row && !row->shape.empty()) commitParameter(row->shape, key, value);
        });
    };
    commitNumber("corner_radius", corner_radius_);
    commitNumber("rotation", rotation_);

    // The picked row's paint: sRGB, straight alpha.
    paint_fields_ = new QWidget(this);
    paint_fields_->setObjectName(named("paint"));
    auto *paint_form = new QFormLayout(paint_fields_);
    paint_form->setContentsMargins(0, 4, 0, 0);
    auto *color_row = new QWidget(paint_fields_);
    auto *color_layout = new QHBoxLayout(color_row);
    color_layout->setContentsMargins(0, 0, 0, 0);
    color_ = new QComboBox(color_row);
    color_->setObjectName(named("field.color"));
    color_->setToolTip(tr("The paint's colour (sRGB)"));
    for (const auto &swatch : graphCanvasSwatches())
        color_->addItem(QIcon{swatchIcon(swatch.color)}, qtext(swatch.name), qtext(swatch.name));
    color_layout->addWidget(color_, 1);
    color_pick_ = new QPushButton(tr("Pick\xE2\x80\xA6"), color_row);
    color_pick_->setObjectName(named("field.colorPick"));
    color_pick_->setToolTip(tr("Any colour, with its alpha"));
    color_layout->addWidget(color_pick_);
    paint_form->addRow(caption(tr("Color"), paint_fields_), color_row);
    stroke_row_ = new QWidget(paint_fields_);
    auto *stroke_layout = new QHBoxLayout(stroke_row_);
    stroke_layout->setContentsMargins(0, 0, 0, 0);
    width_ = numberSpin(stroke_row_, named("field.width"), 0.0, 1.0e4);
    width_->setToolTip(tr("The outline's width, in the stack's pixels"));
    stroke_layout->addWidget(width_, 1);
    align_ = new QComboBox(stroke_row_);
    align_->setObjectName(named("field.align"));
    align_->setToolTip(tr("Where the outline runs: centred on the edge, inside it or outside it"));
    for (const auto *choice : {"center", "inner", "outer"})
        align_->addItem(QLatin1String{choice}, QLatin1String{choice});
    stroke_layout->addWidget(align_, 1);
    paint_form->addRow(caption(tr("Width"), paint_fields_), stroke_row_);
    layout->addWidget(paint_fields_);
    connect(color_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        const auto name = color_->itemData(index).toString().toStdString();
        const auto &swatches = graphCanvasSwatches();
        const auto swatch = std::ranges::find(swatches, name, &GraphCanvasSwatch::name);
        const auto row = pickedRow();
        if (swatch == swatches.end() || !row || row->paint.empty()) return;
        commitParameter(row->source, "color",
                        Json::array({swatch->color[0], swatch->color[1], swatch->color[2], swatch->color[3]}));
    });
    connect(color_pick_, &QPushButton::clicked, this, [this] {
        const auto row = pickedRow();
        if (!row || row->paint.empty()) return;
        const auto nodes = model_.nodes(scope_);
        const auto *paint = findNode(nodes, row->source);
        if (paint == nullptr) return;
        const auto &parameters = paint->parameters;
        const QColor current = QColor::fromRgbF(
            static_cast<float>(number(parameters, "color", 0, 1.0)), static_cast<float>(number(parameters, "color", 1, 1.0)),
            static_cast<float>(number(parameters, "color", 2, 1.0)), static_cast<float>(number(parameters, "color", 3, 1.0)));
        const QColor chosen = QColorDialog::getColor(current, this, tr("Layer colour"),
                                                     QColorDialog::ShowAlphaChannel);
        if (!chosen.isValid()) return;
        commitParameter(row->source, "color",
                        Json::array({chosen.redF(), chosen.greenF(), chosen.blueF(), chosen.alphaF()}));
    });
    connect(width_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
        if (loading_) return;
        const auto row = pickedRow();
        if (row && row->paint == "stroke") commitParameter(row->source, "width", value);
    });
    connect(align_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        const auto row = pickedRow();
        if (row && row->paint == "stroke")
            commitParameter(row->source, "align", align_->itemData(index).toString().toStdString());
    });

    open_group_ = button(tr("Open group"), "openGroup", tr("Show this group's own layers"));
    layout->addWidget(open_group_, 0, Qt::AlignLeft);
    connect(open_group_, &QPushButton::clicked, this, [this] {
        const auto row = pickedRow();
        if (!row || row->kind != "group") return;
        stack_ = row->source;
        picked_.clear();
        reload(true);
        if (nodePicked) nodePicked(scope_, stack_);
    });

    note_ = caption({}, this);
    note_->setObjectName(named("note"));
    note_->setWordWrap(true);
    note_->hide();
    layout->addWidget(note_);
    hide();
}

void GraphCanvasLayersPanel::showSelection(const std::string &scope,
                                           const std::optional<std::string> &node) {
    if (node) {
        const auto nodes = model_.nodes(scope);
        // A node of a row of the stack on show picks that row and keeps the
        // stack -- a group's own stack included, which Open group enters.
        const auto row = scope == scope_ && !stack_.empty()
                             ? graphCanvasRowFor(nodes, stack_, *node)
                             : std::nullopt;
        if (row) {
            for (const auto &each : graphCanvasLayerRows(nodes, stack_))
                if (each.index == *row) picked_ = each.input;
        } else if (const auto stack = graphCanvasStackFor(nodes, *node); !stack.empty()) {
            if (stack != stack_ || scope != scope_) picked_.clear();
            stack_ = stack;
            scope_ = scope;
            if (const auto index = graphCanvasRowFor(nodes, stack_, *node))
                for (const auto &each : graphCanvasLayerRows(nodes, stack_))
                    if (each.index == *index) picked_ = each.input;
        }
    }
    reload(false);
}

void GraphCanvasLayersPanel::reload(bool force) {
    const auto nodes = model_.nodes(scope_);
    const auto *stack = stack_.empty() ? nullptr : findNode(nodes, stack_);
    if (stack == nullptr || stack->op != "canvas.stack") {
        stack_.clear();
        shown_rows_.clear();
        shown_stack_.clear();
        hide();
        return;
    }
    title_->setText(tr("Canvas layers \xE2\x80\x94 %1").arg(qtext(stack->heading())));
    const auto rows = graphCanvasLayerRows(nodes, stack_);
    if (force || rows != shown_rows_ || stack_ != shown_stack_) {
        shown_rows_ = rows;
        shown_stack_ = stack_;
        rebuildRows();
    }
    for (std::size_t index = 0; index < row_widgets_.size() && index < shown_rows_.size(); ++index) {
        const QSignalBlocker block{row_widgets_[index].pick};
        row_widgets_[index].pick->setChecked(shown_rows_[index].input == picked_);
    }
    empty_->setVisible(shown_rows_.empty());
    loadFields();
    followEditable();
    show();
}

void GraphCanvasLayersPanel::rebuildRows() {
    // Later: a row's own button may be what is running now.
    for (auto &widgets : row_widgets_) {
        widgets.row->hide();
        rows_layout_->removeWidget(widgets.row);
        widgets.row->deleteLater();
    }
    row_widgets_.clear();
    for (std::size_t shown = 0; shown < shown_rows_.size(); ++shown) {
        const auto &row = shown_rows_[shown];
        const auto prefix = QStringLiteral("pelican.canvasLayers.row%1").arg(shown);
        RowWidgets widgets;
        widgets.row = new QWidget(rows_);
        widgets.row->setObjectName(prefix);
        auto *line = new QHBoxLayout(widgets.row);
        line->setContentsMargins(0, 0, 0, 0);
        widgets.visible = new QCheckBox(widgets.row);
        widgets.visible->setObjectName(prefix + QStringLiteral(".visible"));
        widgets.visible->setToolTip(tr("Shown (the eye)"));
        widgets.visible->setChecked(row.visible);
        line->addWidget(widgets.visible);
        widgets.pick = new QPushButton(qtext(graphCanvasLayerKindText(row)), widgets.row);
        widgets.pick->setObjectName(prefix + QStringLiteral(".pick"));
        widgets.pick->setCheckable(true);
        widgets.pick->setFlat(true);
        widgets.pick->setToolTip(tr("Pick this layer, and its node in the Graph"));
        widgets.pick->setChecked(row.input == picked_);
        line->addWidget(widgets.pick);
        widgets.name = new QLineEdit(qtext(row.name), widgets.row);
        widgets.name->setObjectName(prefix + QStringLiteral(".name"));
        widgets.name->setToolTip(tr("The layer's name (its input is %1)").arg(qtext(row.input)));
        line->addWidget(widgets.name, 1);
        widgets.opacity = new QSpinBox(widgets.row);
        widgets.opacity->setObjectName(prefix + QStringLiteral(".opacity"));
        widgets.opacity->setRange(0, 100);
        widgets.opacity->setSuffix(QStringLiteral("%"));
        widgets.opacity->setKeyboardTracking(false);
        widgets.opacity->setValue(static_cast<int>(std::lround(row.opacity * 100.0)));
        widgets.opacity->setToolTip(row.opacity_driven ? tr("A value line drives it.")
                                                       : tr("The layer's opacity"));
        line->addWidget(widgets.opacity);
        rows_layout_->addWidget(widgets.row);
        const auto input = row.input;
        const auto index = row.index;
        connect(widgets.visible, &QCheckBox::toggled, this, [this, index](bool on) {
            if (loading_) return;
            commit(graphCanvasSetLayerOps(model_.nodes(scope_), scope_, stack_, index, "visible", on));
        });
        connect(widgets.pick, &QPushButton::clicked, this, [this, input] { pickRow(input, true); });
        auto *name = widgets.name;
        connect(name, &QLineEdit::editingFinished, this, [this, index, name] {
            if (loading_) return;
            commit(graphCanvasSetLayerOps(model_.nodes(scope_), scope_, stack_, index, "name",
                                          name->text().trimmed().toStdString()));
        });
        connect(widgets.opacity, &QSpinBox::valueChanged, this, [this, index](int percent) {
            if (loading_) return;
            commit(graphCanvasSetLayerOps(model_.nodes(scope_), scope_, stack_, index, "opacity",
                                          percent / 100.0));
        });
        row_widgets_.push_back(widgets);
    }
}

std::optional<GraphCanvasStackRow> GraphCanvasLayersPanel::pickedRow() const {
    const auto found = std::ranges::find(shown_rows_, picked_, &GraphCanvasStackRow::input);
    if (picked_.empty() || found == shown_rows_.end()) return std::nullopt;
    return *found;
}

void GraphCanvasLayersPanel::pickRow(const std::string &input, bool tell) {
    picked_ = input;
    for (std::size_t index = 0; index < row_widgets_.size() && index < shown_rows_.size(); ++index) {
        const QSignalBlocker block{row_widgets_[index].pick};
        row_widgets_[index].pick->setChecked(shown_rows_[index].input == picked_);
    }
    loadFields();
    followEditable();
    const auto row = pickedRow();
    if (tell && row && !row->source.empty() && nodePicked) nodePicked(scope_, row->source);
}

void GraphCanvasLayersPanel::loadFields() {
    loading_ = true;
    const auto row = pickedRow();
    const auto nodes = model_.nodes(scope_);
    const auto *shape = row && !row->shape.empty() ? findNode(nodes, row->shape) : nullptr;
    const auto *paint = row && !row->paint.empty() ? findNode(nodes, row->source) : nullptr;
    shape_fields_->setVisible(shape != nullptr);
    if (shape != nullptr) {
        const auto &parameters = shape->parameters;
        shape_caption_->setText(tr("%1 (%2)").arg(qtext(row->name), qtext(shape->id)));
        const auto load = [&](QDoubleSpinBox *spin, const char *key, std::size_t component,
                              const std::string &port) {
            const QSignalBlocker block{spin};
            if (!spin->hasFocus()) spin->setValue(number(parameters, key, component, 0.0));
            const bool by_line = driven(*shape, port);
            spin->setProperty("pelicanDriven", by_line);
            spin->setToolTip(by_line ? tr("A value line drives it.") : QString{});
        };
        load(size_x_, "size", 0, "size.x");
        load(size_y_, "size", 1, "size.y");
        load(center_x_, "center", 0, "center.x");
        load(center_y_, "center", 1, "center.y");
        corner_row_->setVisible(shape->op == "shape.rect");
        load(corner_radius_, "corner_radius", 0, "corner_radius");
        load(rotation_, "rotation", 0, "rotation");
    }
    paint_fields_->setVisible(paint != nullptr);
    if (paint != nullptr) {
        const auto &parameters = paint->parameters;
        const std::array<double, 4> color{number(parameters, "color", 0, 0.0), number(parameters, "color", 1, 0.0),
                                          number(parameters, "color", 2, 0.0), number(parameters, "color", 3, 1.0)};
        const QSignalBlocker block{color_};
        // A colour no swatch holds is shown as Custom, kept at the end.
        if (const int custom = color_->findData(QString{}); custom >= 0) color_->removeItem(custom);
        const auto &swatches = graphCanvasSwatches();
        const auto swatch = std::ranges::find_if(swatches, [&](const GraphCanvasSwatch &each) {
            for (std::size_t channel = 0; channel < 3; ++channel)
                if (std::abs(each.color[channel] - color[channel]) > 1e-3) return false;
            return true;
        });
        if (swatch != swatches.end()) {
            color_->setCurrentIndex(color_->findData(qtext(swatch->name)));
        } else {
            color_->addItem(QIcon{swatchIcon(color)}, tr("Custom"), QString{});
            color_->setCurrentIndex(color_->count() - 1);
        }
        stroke_row_->setVisible(paint->op == "shape.stroke");
        if (paint->op == "shape.stroke") {
            const QSignalBlocker block_width{width_};
            if (!width_->hasFocus()) width_->setValue(number(parameters, "width", 0, 0.0));
            const QSignalBlocker block_align{align_};
            const auto align = parameters.contains("align") && parameters.at("align").is_string()
                                   ? parameters.at("align").get<std::string>()
                                   : std::string{"center"};
            align_->setCurrentIndex(std::max(0, align_->findData(qtext(align))));
        }
    }
    open_group_->setVisible(row && row->kind == "group");
    loading_ = false;
}

void GraphCanvasLayersPanel::followEditable() {
    const bool editable = model_.canEdit();
    for (std::size_t index = 0; index < row_widgets_.size() && index < shown_rows_.size(); ++index) {
        auto &widgets = row_widgets_[index];
        widgets.visible->setEnabled(editable);
        widgets.name->setEnabled(editable);
        widgets.opacity->setEnabled(editable && !shown_rows_[index].opacity_driven);
    }
    const auto row = pickedRow();
    std::size_t count = 0;
    const auto nodes = model_.nodes(scope_);
    if (const auto *stack = findNode(nodes, stack_))
        if (const auto layers = stack->parameters.find("layers");
            layers != stack->parameters.end() && layers->is_array())
            count = layers->size();
    add_rect_->setEnabled(editable);
    add_ellipse_->setEnabled(editable);
    add_group_->setEnabled(editable);
    add_stroke_->setEnabled(editable && row && !row->shape.empty());
    delete_->setEnabled(editable && row.has_value());
    up_->setEnabled(editable && row && row->index + 1 < count);
    down_->setEnabled(editable && row && row->index > 0);
    for (auto *spin : {size_x_, size_y_, center_x_, center_y_, corner_radius_, rotation_})
        spin->setEnabled(editable && !spin->property("pelicanDriven").toBool());
    color_->setEnabled(editable);
    color_pick_->setEnabled(editable);
    width_->setEnabled(editable);
    align_->setEnabled(editable);
}

void GraphCanvasLayersPanel::commit(Json ops) {
    if (ops.empty()) return;
    // The layer a + adds is the one picked next: the set_input that puts it
    // on this stack names it.
    std::string added;
    if (ops.front().value("op", std::string{}) == "add_node")
        for (const auto &op : ops)
            if (op.value("op", std::string{}) == "set_input" && op.value("node", std::string{}) == stack_)
                added = op.value("port", std::string{});
    const bool sent = model_.submitEdit(std::move(ops));
    if (sent) {
        note_->hide();
        if (!added.empty()) picked_ = added;
    } else {
        // Refused before it was sent (the model's notice says why) or the
        // last edit is still out: the fields say what the document says.
        const auto &notice = model_.editNotice();
        note_->setText(notice.kind == InspectorNoticeKind::Error && !notice.message.empty()
                           ? qtext(notice.message)
                           : tr("The last change is still being saved; try again."));
        note_->show();
        reload(true);
    }
    if (submitted_) submitted_();
}

void GraphCanvasLayersPanel::commitParameter(const std::string &node, const std::string &key,
                                             const Json &value) {
    commit(graphCanvasSetParameterOps(model_.nodes(scope_), scope_, node, key, value));
}

} // namespace PelicanStudio
