#include "graphcanvaslayerspanel.hpp"

#include "../model/graphcanvasimport.hpp"
#include "../model/graphcanvasmodel.hpp"
#include "studiotheme.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <tuple>

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

std::filesystem::path localPath(const QString &path) { return std::filesystem::path{path.toStdWString()}; }

std::string utf8Stem(const std::filesystem::path &path) {
    const auto text = path.stem().u8string();
    return {text.begin(), text.end()};
}

// The pictures among dropped or picked files.
std::vector<std::filesystem::path> pictures(const QMimeData *data) {
    std::vector<std::filesystem::path> files;
    if (data == nullptr || !data->hasUrls()) return files;
    for (const auto &url : data->urls())
        if (url.isLocalFile() && isCanvasImageFile(localPath(url.toLocalFile())))
            files.push_back(localPath(url.toLocalFile()));
    return files;
}

// A picture's pixels, or nothing when it cannot be read as one.
std::optional<std::array<double, 2>> pixelSize(const std::filesystem::path &file) {
    QImageReader reader{QString::fromStdWString(file.wstring())};
    const auto size = reader.size();
    if (!size.isValid() || size.isEmpty()) return std::nullopt;
    return std::array<double, 2>{static_cast<double>(size.width()), static_cast<double>(size.height())};
}

// What an op list does to the nodes as the canvas reads them, so a second
// gesture can be built on the first in the same list (several pictures, a
// Canvas and the import that goes on it).
void applyLocally(std::vector<GraphCanvasNode> &nodes, const Json &ops) {
    const auto connect = [](GraphCanvasNode &node, const std::string &port, const Json &source) {
        auto found = std::ranges::find(node.inputs, port, &GraphCanvasPort::port);
        if (found == node.inputs.end()) {
            GraphCanvasPort made;
            made.port = port;
            node.inputs.push_back(made);
            found = std::prev(node.inputs.end());
        }
        found->connected = source.is_object();
        found->source_node = source.is_object() ? source.value("node", std::string{}) : std::string{};
        found->source_port = source.is_object() ? source.value("port", std::string{}) : std::string{};
    };
    const auto restack = [](GraphCanvasNode &node) {
        if (node.op != "canvas.stack") return;
        auto ports = graphCanvasStackPorts(node.parameters);
        for (auto &port : ports)
            if (const auto old = std::ranges::find(node.inputs, port.port, &GraphCanvasPort::port);
                old != node.inputs.end() && old->connected) {
                port.connected = true;
                port.source_node = old->source_node;
                port.source_port = old->source_port;
            }
        node.inputs = std::move(ports);
    };
    for (const auto &op : ops) {
        const auto kind = op.value("op", std::string{});
        if (kind == "add_node") {
            const auto &json = op.at("node");
            GraphCanvasNode node;
            node.id = json.value("id", std::string{});
            node.op = json.value("op", std::string{});
            node.definition = json.value("definition", std::string{});
            node.parameters = json.value("parameters", Json::object());
            restack(node);
            // items() borrows its container, so the object outlives the loop.
            const auto inputs = json.value("inputs", Json::object());
            for (const auto &[port, source] : inputs.items()) connect(node, port, source);
            nodes.push_back(std::move(node));
        } else if (kind == "set_node_parameters") {
            const auto id = op.value("node", std::string{});
            for (auto &node : nodes)
                if (node.id == id) {
                    node.parameters = op.at("parameters");
                    restack(node);
                }
        } else if (kind == "set_input") {
            const auto id = op.value("node", std::string{});
            for (auto &node : nodes)
                if (node.id == id) connect(node, op.value("port", std::string{}), op.at("source"));
        } else if (kind == "remove_node") {
            std::erase_if(nodes, [&](const GraphCanvasNode &node) { return node.id == op.value("node", std::string{}); });
        }
    }
}

// The stack of a definition's nodes no other node reads: its root.
std::string rootStack(const std::vector<GraphCanvasNode> &nodes) {
    for (const auto &node : nodes) {
        if (node.op != "canvas.stack") continue;
        const bool read = std::ranges::any_of(nodes, [&](const GraphCanvasNode &reader) {
            return std::ranges::any_of(reader.inputs, [&](const GraphCanvasPort &port) {
                return port.connected && port.source_node == node.id;
            });
        });
        if (!read) return node.id;
    }
    return {};
}

} // namespace

GraphCanvasLayersPanel::GraphCanvasLayersPanel(GraphCanvasModel &model, std::function<void()> submitted,
                                               QWidget *parent)
    : QWidget(parent), model_(model), submitted_(std::move(submitted)) {
    setObjectName(QStringLiteral("pelican.canvasLayers"));
    setAcceptDrops(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    const auto button = [](QWidget *parent, const QString &text, const char *name, const QString &tip) {
        auto *pressed = new QPushButton(text, parent);
        pressed->setObjectName(named(name));
        pressed->setToolTip(tip);
        return pressed;
    };
    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    title_ = new QLabel(this);
    title_->setObjectName(named("title"));
    title_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    header->addWidget(title_, 1);
    back_ = button(this, tr("\xE2\x86\x90 Back"), "back", tr("Back to the stack this group is in"));
    header->addWidget(back_);
    layout->addLayout(header);
    stacks_ = new QComboBox(this);
    stacks_->setObjectName(named("stacks"));
    stacks_->setToolTip(tr("The canvas whose layers are shown (each canvas.output's stack)"));
    layout->addWidget(stacks_);
    add_canvas_ = button(this, tr("+ Canvas"), "addCanvas",
                         tr("A 1280 x 720 canvas (canvas.output + canvas.stack), put on the screen's present"));
    layout->addWidget(add_canvas_, 0, Qt::AlignLeft);

    body_ = new QWidget(this);
    auto *body = new QVBoxLayout(body_);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(4);
    layout->addWidget(body_);
    rows_ = new QWidget(body_);
    rows_layout_ = new QVBoxLayout(rows_);
    rows_layout_->setContentsMargins(0, 0, 0, 0);
    rows_layout_->setSpacing(2);
    body->addWidget(rows_);
    empty_ = caption(tr("No layer yet: + Rect, + Ellipse or + Image adds one on top."), body_);
    empty_->setObjectName(named("empty"));
    body->addWidget(empty_);

    auto *adds = new QHBoxLayout;
    adds->setContentsMargins(0, 0, 0, 0);
    add_rect_ = button(body_, tr("+ Rect"), "addRect",
                       tr("A filled rectangle at the middle of the canvas, a quarter of its size, on top"));
    add_ellipse_ = button(body_, tr("+ Ellipse"), "addEllipse",
                          tr("A filled ellipse at the middle of the canvas, a quarter of its size, on top"));
    add_group_ = button(body_, tr("+ Group"), "addGroup", tr("An empty stack of its own, on top"));
    add_stroke_ = button(body_, tr("+ Stroke"), "addStroke",
                         tr("An outline of the picked layer's shape, just above it"));
    for (auto *each : {add_rect_, add_ellipse_, add_group_, add_stroke_}) adds->addWidget(each);
    adds->addStretch(1);
    body->addLayout(adds);
    auto *files = new QHBoxLayout;
    files->setContentsMargins(0, 0, 0, 0);
    add_image_ = button(body_, tr("+ Image\xE2\x80\xA6"), "addImage",
                        tr("Pictures (PNG, JPG) copied into the project's assets/canvas, each a layer on top at "
                           "the middle of the canvas (a name already there gets _2). Dropping them here does "
                           "the same."));
    import_output_ = button(body_, tr("Import PSD output\xE2\x80\xA6"), "importPsdOutput",
                            tr("A psd-canvas conversion (its canvas.graph.json): copied into the project's "
                               "imports/, and its layers as one group on top"));
    import_psd_ = button(body_, tr("Import PSD\xE2\x80\xA6"), "importPsd",
                         tr("A .psd converted with pelican-import-tools psd-canvas, then imported"));
    import_psd_->setVisible(qEnvironmentVariableIsSet("PELICAN_IMPORT_TOOLS"));
    for (auto *each : {add_image_, import_output_, import_psd_}) files->addWidget(each);
    files->addStretch(1);
    body->addLayout(files);
    // SG-3: text and adjustment layers.
    auto *more = new QHBoxLayout;
    more->setContentsMargins(0, 0, 0, 0);
    add_text_ = button(body_, tr("+ Text\xE2\x80\xA6"), "addText",
                       tr("Text in a font of the project's, white, at the middle of the canvas, on top"));
    more->addWidget(add_text_);
    add_adjustment_ = new QToolButton(body_);
    add_adjustment_->setObjectName(named("addAdjustment"));
    add_adjustment_->setText(tr("+ Adjustment"));
    add_adjustment_->setToolTip(tr("A colour filter over everything under it (a canvas.backdrop and the filter), "
                                   "just above the picked layer"));
    add_adjustment_->setPopupMode(QToolButton::InstantPopup);
    add_adjustment_->setMenu(filterMenu("addAdjustment", graphCanvasAdjustmentChoices(), [this](const std::string &op) {
        const auto row = pickedRow();
        commit(graphCanvasAddAdjustmentLayerOps(model_.nodes(scope_), scope_, stack_,
                                                row ? std::optional<std::size_t>{row->index} : std::nullopt, op));
    }));
    more->addWidget(add_adjustment_);
    more->addStretch(1);
    body->addLayout(more);
    connect(add_text_, &QPushButton::clicked, this, [this] { openTextDialog(); });
    auto *moves = new QHBoxLayout;
    moves->setContentsMargins(0, 0, 0, 0);
    delete_ = button(body_, tr("Delete"), "delete",
                     tr("The picked layer, and the canvas nodes nothing else uses"));
    up_ = button(body_, QStringLiteral("\xE2\x86\x91"), "up", tr("Move the picked layer up one"));
    down_ = button(body_, QStringLiteral("\xE2\x86\x93"), "down", tr("Move the picked layer down one"));
    for (auto *each : {delete_, up_, down_}) moves->addWidget(each);
    moves->addStretch(1);
    body->addLayout(moves);

    connect(add_canvas_, &QPushButton::clicked, this, [this] {
        auto ops = graphCanvasCompositionOps(model_.nodes({}), {});
        if (ops.empty()) return;
        const auto output = ops.back()["node"].value("id", std::string{});
        trail_.clear();
        read_only_ = false;
        if (addCanvas) {
            if (addCanvas(std::move(ops), output)) note_->hide();
            if (submitted_) submitted_();
        } else {
            commit(std::move(ops));
        }
    });
    connect(stacks_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        trail_.clear();
        read_only_ = false;
        scope_.clear();
        stack_ = stacks_->itemData(index).toString().toStdString();
        picked_.clear();
        reload(true);
    });
    connect(back_, &QPushButton::clicked, this, [this] {
        if (trail_.empty()) return;
        const auto place = trail_.back();
        trail_.pop_back();
        scope_ = place.scope;
        stack_ = place.stack;
        read_only_ = place.read_only;
        picked_.clear();
        note_->hide();
        reload(true);
    });
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
    connect(add_image_, &QPushButton::clicked, this, [this] { chooseImages(false); });
    connect(import_output_, &QPushButton::clicked, this, [this] {
        chooseFile(tr("Import PSD output"), QStringLiteral("pelican.canvasPsdOutputDialog"),
                   tr("psd-canvas output (canvas.graph.json *.json)"), [this](const QStringList &picked) {
                       if (!picked.isEmpty()) importPsdOutput(localPath(picked.front()));
                   });
    });
    connect(import_psd_, &QPushButton::clicked, this, [this] {
        chooseFile(tr("Import PSD"), QStringLiteral("pelican.canvasPsdDialog"), tr("Photoshop (*.psd *.PSD)"),
                   [this](const QStringList &picked) {
                       if (!picked.isEmpty()) importPsd(localPath(picked.front()));
                   });
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

    const auto pair = [&](QWidget *parent, const QString &key, QDoubleSpinBox *&x, QDoubleSpinBox *&y,
                          double low) {
        auto *row = new QWidget(parent);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        x = numberSpin(row, named("field.") + key + QStringLiteral(".x"), low, 1.0e6);
        y = numberSpin(row, named("field.") + key + QStringLiteral(".y"), low, 1.0e6);
        row_layout->addWidget(x, 1);
        row_layout->addWidget(y, 1);
        return row;
    };

    // The picked row's shape: in the stack's pixels, origin top left, +y down.
    shape_fields_ = new QWidget(body_);
    shape_fields_->setObjectName(named("shape"));
    auto *shape_form = new QFormLayout(shape_fields_);
    shape_form->setContentsMargins(0, 4, 0, 0);
    shape_caption_ = caption({}, shape_fields_);
    shape_caption_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    shape_form->addRow(shape_caption_);
    shape_form->addRow(caption(tr("Size"), shape_fields_),
                       pair(shape_fields_, QStringLiteral("size"), size_x_, size_y_, 0.1));
    shape_form->addRow(caption(tr("Center"), shape_fields_),
                       pair(shape_fields_, QStringLiteral("center"), center_x_, center_y_, -1.0e6));
    corner_row_ = new QWidget(shape_fields_);
    auto *corner_layout = new QHBoxLayout(corner_row_);
    corner_layout->setContentsMargins(0, 0, 0, 0);
    corner_radius_ = numberSpin(corner_row_, named("field.corner_radius"), 0.0, 1.0e6);
    corner_layout->addWidget(corner_radius_);
    shape_form->addRow(caption(tr("Corner radius"), shape_fields_), corner_row_);
    rotation_ = numberSpin(shape_fields_, named("field.rotation"), -360.0, 360.0);
    rotation_->setSuffix(QStringLiteral("\xC2\xB0"));
    shape_form->addRow(caption(tr("Rotation"), shape_fields_), rotation_);
    body->addWidget(shape_fields_);
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
    paint_fields_ = new QWidget(body_);
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
    body->addWidget(paint_fields_);
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

    // SG-2: the picked image layer's place, size (its picture's pixels to
    // begin with) and sampler.
    image_fields_ = new QWidget(body_);
    image_fields_->setObjectName(named("image"));
    auto *image_form = new QFormLayout(image_fields_);
    image_form->setContentsMargins(0, 4, 0, 0);
    image_form->addRow(caption(tr("Offset"), image_fields_),
                       pair(image_fields_, QStringLiteral("offset"), offset_x_, offset_y_, -1.0e6));
    auto *image_size = pair(image_fields_, QStringLiteral("imageSize"), image_w_, image_h_, 0.1);
    size_lock_ = new QToolButton(image_size);
    size_lock_->setObjectName(named("field.sizeLock"));
    size_lock_->setText(QStringLiteral("\xE2\x9B\x93"));
    size_lock_->setCheckable(true);
    size_lock_->setChecked(true);
    size_lock_->setToolTip(tr("Keep the width and the height in proportion"));
    image_size->layout()->addWidget(size_lock_);
    image_form->addRow(caption(tr("Size"), image_fields_), image_size);
    sampler_ = new QComboBox(image_fields_);
    sampler_->setObjectName(named("field.sampler"));
    sampler_->setToolTip(tr("How the picture is read between its pixels: smoothed or the nearest"));
    for (const auto *choice : {"linear", "nearest"}) sampler_->addItem(QLatin1String{choice}, QLatin1String{choice});
    image_form->addRow(caption(tr("Sampler"), image_fields_), sampler_);
    body->addWidget(image_fields_);
    for (auto *spin : {offset_x_, offset_y_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
            if (loading_) return;
            const auto row = pickedRow();
            if (row && row->kind == "image")
                commitParameter(row->source, "offset", Json::array({offset_x_->value(), offset_y_->value()}));
        });
    const auto commitImageSize = [this](bool width_moved) {
        if (loading_) return;
        const auto row = pickedRow();
        if (!row || row->kind != "image") return;
        double w = image_w_->value();
        double h = image_h_->value();
        if (size_lock_->isChecked() && size_ratio_ > 0.0) {
            if (width_moved) h = w / size_ratio_;
            else w = h * size_ratio_;
        }
        commitParameter(row->source, "size", Json::array({w, h}));
    };
    connect(image_w_, &QDoubleSpinBox::valueChanged, this, [commitImageSize] { commitImageSize(true); });
    connect(image_h_, &QDoubleSpinBox::valueChanged, this, [commitImageSize] { commitImageSize(false); });
    connect(sampler_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        const auto row = pickedRow();
        if (row && row->kind == "image")
            commitParameter(row->source, "sampler", sampler_->itemData(index).toString().toStdString());
    });

    // SG-2: what every layer has -- the blend, the mask, the Transform.
    layer_fields_ = new QWidget(body_);
    layer_fields_->setObjectName(named("layer"));
    auto *layer_form = new QFormLayout(layer_fields_);
    layer_form->setContentsMargins(0, 4, 0, 0);
    blend_ = new QComboBox(layer_fields_);
    blend_->setObjectName(named("field.blend"));
    // SG-3 (CV-2b): every blend the engine draws, in Photoshop's groups.
    {
        int group = -1;
        for (const auto &choice : graphCanvasBlendChoices()) {
            if (group >= 0 && choice.group != group) blend_->insertSeparator(blend_->count());
            group = choice.group;
            blend_->addItem(qtext(choice.label), qtext(choice.name));
        }
    }
    blend_->setMaxVisibleItems(32);
    blend_->setToolTip(tr("How the layer mixes with what is under it"));
    layer_form->addRow(caption(tr("Blend"), layer_fields_), blend_);
    auto *mask_row = new QWidget(layer_fields_);
    auto *mask_layout = new QHBoxLayout(mask_row);
    mask_layout->setContentsMargins(0, 0, 0, 0);
    mask_caption_ = caption({}, mask_row);
    mask_caption_->setObjectName(named("field.maskName"));
    mask_layout->addWidget(mask_caption_, 1);
    add_mask_ = new QPushButton(tr("Add mask image\xE2\x80\xA6"), mask_row);
    add_mask_->setObjectName(named("field.addMask"));
    add_mask_->setToolTip(tr("A picture whose brightness is where the layer shows (white) and where it does "
                             "not (black), copied into assets/canvas"));
    mask_layout->addWidget(add_mask_);
    remove_mask_ = new QPushButton(tr("Remove mask"), mask_row);
    remove_mask_->setObjectName(named("field.removeMask"));
    mask_layout->addWidget(remove_mask_);
    layer_form->addRow(caption(tr("Mask"), layer_fields_), mask_row);
    transform_toggle_ = new QPushButton(tr("Transform"), layer_fields_);
    transform_toggle_->setObjectName(named("field.transformToggle"));
    transform_toggle_->setCheckable(true);
    transform_toggle_->setFlat(true);
    transform_toggle_->setStyleSheet(QStringLiteral("text-align: left; font-weight: bold;"));
    transform_toggle_->setToolTip(tr("Move, turn and scale the whole layer (a canvas.transform)"));
    layer_form->addRow(transform_toggle_);
    transform_fields_ = new QWidget(layer_fields_);
    auto *transform_form = new QFormLayout(transform_fields_);
    transform_form->setContentsMargins(0, 0, 0, 0);
    transform_form->addRow(caption(tr("Translate"), transform_fields_),
                           pair(transform_fields_, QStringLiteral("translate"), translate_x_, translate_y_, -1.0e6));
    rotate_ = numberSpin(transform_fields_, named("field.rotate"), -360.0, 360.0);
    rotate_->setSuffix(QStringLiteral("\xC2\xB0"));
    transform_form->addRow(caption(tr("Rotate"), transform_fields_), rotate_);
    transform_form->addRow(caption(tr("Scale"), transform_fields_),
                           pair(transform_fields_, QStringLiteral("scale"), scale_x_, scale_y_, -1.0e3));
    for (auto *spin : {scale_x_, scale_y_}) {
        spin->setDecimals(3);
        spin->setSingleStep(0.1);
    }
    layer_form->addRow(transform_fields_);
    body->addWidget(layer_fields_);
    connect(blend_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        if (const auto row = pickedRow())
            commit(graphCanvasSetLayerFlagOps(model_.nodes(scope_), scope_, stack_, row->index, "blend",
                                              blend_->itemData(index).toString().toStdString()));
    });
    connect(add_mask_, &QPushButton::clicked, this, [this] { chooseImages(true); });
    connect(remove_mask_, &QPushButton::clicked, this, [this] {
        if (const auto row = pickedRow())
            commit(graphCanvasRemoveMaskOps(model_.nodes(scope_), scope_, stack_, row->index));
    });
    connect(transform_toggle_, &QPushButton::toggled, this, [this](bool open) {
        transform_open_ = open;
        transform_fields_->setVisible(open);
    });
    for (auto *spin : {translate_x_, translate_y_, rotate_, scale_x_, scale_y_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] { commitTransform(); });

    // SG-2: no row picked, the stack's own.
    stack_fields_ = new QWidget(body_);
    stack_fields_->setObjectName(named("stackFields"));
    auto *stack_form = new QFormLayout(stack_fields_);
    stack_form->setContentsMargins(0, 4, 0, 0);
    stack_form->addRow(caption(tr("Canvas size"), stack_fields_),
                       pair(stack_fields_, QStringLiteral("stackSize"), stack_w_, stack_h_, 1.0));
    blend_space_ = new QComboBox(stack_fields_);
    blend_space_->setObjectName(named("field.blendSpace"));
    blend_space_->setToolTip(tr("Where the layers mix: linear light, or sRGB values as a paint program does"));
    for (const auto *choice : {"linear", "srgb"}) blend_space_->addItem(QLatin1String{choice}, QLatin1String{choice});
    stack_form->addRow(caption(tr("Blend space"), stack_fields_), blend_space_);
    body->addWidget(stack_fields_);
    buildSg3Fields(body);
    for (auto *spin : {stack_w_, stack_h_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this] {
            if (loading_) return;
            commit(graphCanvasSetStackOps(model_.nodes(scope_), scope_, stack_, "size",
                                          Json::array({stack_w_->value(), stack_h_->value()})));
        });
    connect(blend_space_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        commit(graphCanvasSetStackOps(model_.nodes(scope_), scope_, stack_, "blend_space",
                                      blend_space_->itemData(index).toString().toStdString()));
    });

    open_group_ = button(body_, tr("Open group"), "openGroup", tr("Show this group's own layers"));
    body->addWidget(open_group_, 0, Qt::AlignLeft);
    connect(open_group_, &QPushButton::clicked, this, [this] {
        const auto row = pickedRow();
        if (!row) return;
        if (row->kind == "group") {
            trail_.push_back({scope_, stack_, read_only_});
            stack_ = row->source;
            picked_.clear();
            reload(true);
            if (nodePicked && !read_only_) nodePicked(scope_, stack_);
        } else if (row->kind == "import" && !row->definition.empty()) {
            // An imported PSD's layers are its definition's: read, not edited.
            const auto inside = rootStack(model_.nodes(row->definition));
            if (inside.empty()) {
                say(tr("%1 holds no canvas.stack to show.").arg(qtext(row->definition)));
                return;
            }
            trail_.push_back({scope_, stack_, read_only_});
            scope_ = row->definition;
            stack_ = inside;
            read_only_ = true;
            picked_.clear();
            reload(true);
        }
    });

    note_ = caption({}, this);
    note_->setObjectName(named("note"));
    note_->setWordWrap(true);
    note_->hide();
    layout->addWidget(note_);
    refusal_ = caption({}, this);
    refusal_->setObjectName(named("refusal"));
    refusal_->setWordWrap(true);
    refusal_->setStyleSheet(QStringLiteral("color: #c0392b;"));
    refusal_->hide();
    layout->addWidget(refusal_);
    imports_ = new QPushButton(this);
    imports_->setObjectName(named("imports"));
    imports_->setCheckable(true);
    imports_->setFlat(true);
    imports_->setStyleSheet(QStringLiteral("text-align: left;"));
    imports_->setToolTip(tr("What the PSD conversion noted (canvas.report.json)"));
    imports_->hide();
    layout->addWidget(imports_);
    import_list_ = caption({}, this);
    import_list_->setObjectName(named("importList"));
    import_list_->setWordWrap(true);
    import_list_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    import_list_->hide();
    layout->addWidget(import_list_);
    connect(imports_, &QPushButton::toggled, import_list_, &QWidget::setVisible);
    layout->addStretch(1);
    reload(true);
}

void GraphCanvasLayersPanel::showSelection(const std::string &scope,
                                           const std::optional<std::string> &node) {
    // Only a new pick moves the dock: the same one, read again with every
    // redraw, leaves an opened group open.
    const auto key = node ? scope + "\n" + *node : std::string{};
    const bool fresh = key != property("pelicanLastPick").toString().toStdString();
    setProperty("pelicanLastPick", qtext(key));
    if (node && fresh) {
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
            if (stack != stack_ || scope != scope_) {
                picked_.clear();
                trail_.clear();
                read_only_ = false;
            }
            stack_ = stack;
            scope_ = scope;
            if (const auto index = graphCanvasRowFor(nodes, stack_, *node))
                for (const auto &each : graphCanvasLayerRows(nodes, stack_))
                    if (each.index == *index) picked_ = each.input;
            // SG-3: a canvas.place shows its stack's fields, its own among them.
            if (const auto *picked = findNode(nodes, *node); picked != nullptr && picked->op == "canvas.place") {
                place_ = *node;
                picked_.clear();
            }
        }
    }
    reload(false);
}

void GraphCanvasLayersPanel::settleStack() {
    const auto nodes = model_.nodes(scope_);
    if (const auto *stack = stack_.empty() ? nullptr : findNode(nodes, stack_);
        stack != nullptr && stack->op == "canvas.stack")
        return;
    // Gone (or never set): the first canvas of the root.
    trail_.clear();
    read_only_ = false;
    scope_.clear();
    stack_.clear();
    picked_.clear();
    const auto outputs = graphCanvasOutputStacks(model_.nodes({}));
    if (!outputs.empty()) stack_ = outputs.front().stack;
}

void GraphCanvasLayersPanel::reload(bool force) {
    settleStack();
    loading_ = true;
    const auto outputs = graphCanvasOutputStacks(model_.nodes({}));
    if (outputs != shown_outputs_ || force) {
        shown_outputs_ = outputs;
        const QSignalBlocker block{stacks_};
        stacks_->clear();
        for (const auto &each : outputs)
            stacks_->addItem(qtext(each.output + "  \xE2\x86\x90  " + each.stack), qtext(each.stack));
    }
    {
        const QSignalBlocker block{stacks_};
        stacks_->setCurrentIndex(scope_.empty() ? stacks_->findData(qtext(stack_)) : -1);
    }
    loading_ = false;
    stacks_->setVisible(outputs.size() > 1 && trail_.empty());
    back_->setVisible(!trail_.empty());
    const auto nodes = model_.nodes(scope_);
    const auto *stack = stack_.empty() ? nullptr : findNode(nodes, stack_);
    add_canvas_->setVisible(stack == nullptr);
    body_->setVisible(stack != nullptr);
    if (stack == nullptr) {
        title_->setText(tr("Layers \xE2\x80\x94 no canvas yet"));
        shown_rows_.clear();
        shown_stack_.clear();
        rebuildRows();
        followEditable();
        return;
    }
    title_->setText(read_only_ ? tr("Canvas layers \xE2\x80\x94 %1 (imported, read only)").arg(qtext(stack->heading()))
                               : tr("Canvas layers \xE2\x80\x94 %1").arg(qtext(stack->heading())));
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
}

void GraphCanvasLayersPanel::rebuildRows() {
    // Later: a row's own button may be what is running now.
    for (auto &widgets : row_widgets_) {
        widgets.row->hide();
        rows_layout_->removeWidget(widgets.row);
        widgets.row->deleteLater();
    }
    row_widgets_.clear();
    const auto nodes = model_.nodes(scope_);
    for (std::size_t shown = 0; shown < shown_rows_.size(); ++shown) {
        const auto &row = shown_rows_[shown];
        const auto prefix = QStringLiteral("pelican.canvasLayers.row%1").arg(shown);
        RowWidgets widgets;
        widgets.row = new QWidget(rows_);
        widgets.row->setObjectName(prefix);
        // An import's layers are its definition's: greyed.
        if (read_only_) widgets.row->setStyleSheet(QStringLiteral("color: #8a8f98;"));
        auto *line = new QHBoxLayout(widgets.row);
        line->setContentsMargins(0, 0, 0, 0);
        widgets.visible = new QCheckBox(widgets.row);
        widgets.visible->setObjectName(prefix + QStringLiteral(".visible"));
        widgets.visible->setToolTip(tr("Shown (the eye)"));
        widgets.visible->setChecked(row.visible);
        line->addWidget(widgets.visible);
        widgets.clip = new QToolButton(widgets.row);
        widgets.clip->setObjectName(prefix + QStringLiteral(".clip"));
        widgets.clip->setText(QStringLiteral("\xE2\x86\xB3"));
        widgets.clip->setCheckable(true);
        widgets.clip->setChecked(row.clip);
        widgets.clip->setAutoRaise(true);
        widgets.clip->setProperty("pelicanClipAllowed", row.clip || graphCanvasClipAllowed(nodes, stack_, row.index));
        widgets.clip->setToolTip(widgets.clip->property("pelicanClipAllowed").toBool()
                                     ? tr("Clipped to the layer below: drawn only where it is")
                                     : tr("No unclipped layer below to clip to"));
        line->addWidget(widgets.clip);
        widgets.pick = new QPushButton(qtext(graphCanvasLayerKindText(row)), widgets.row);
        widgets.pick->setObjectName(prefix + QStringLiteral(".pick"));
        widgets.pick->setCheckable(true);
        widgets.pick->setFlat(true);
        widgets.pick->setToolTip(tr("Pick this layer, and its node in the Graph"));
        widgets.pick->setChecked(row.input == picked_);
        line->addWidget(widgets.pick);
        widgets.mask = new QLabel(row.mask.empty() ? QString{} : QStringLiteral("\xE2\x97\x90"), widgets.row);
        widgets.mask->setObjectName(prefix + QStringLiteral(".mask"));
        widgets.mask->setToolTip(row.mask.empty() ? QString{} : tr("Masked by %1").arg(qtext(row.mask_source)));
        line->addWidget(widgets.mask);
        widgets.name = new QLineEdit(qtext(row.name), widgets.row);
        widgets.name->setObjectName(prefix + QStringLiteral(".name"));
        widgets.name->setToolTip(tr("The layer's name (its input is %1)").arg(qtext(row.input)));
        line->addWidget(widgets.name, 1);
        // SG-3: a blend other than normal, faintly; `f` for a filtered layer.
        widgets.blend = new QLabel(widgets.row);
        widgets.blend->setObjectName(prefix + QStringLiteral(".blend"));
        widgets.blend->setFont(studioSmallFont());
        widgets.blend->setStyleSheet(QStringLiteral("color: #8a8f98;"));
        if (row.blend != "normal") {
            const auto &choices = graphCanvasBlendChoices();
            const auto choice = std::ranges::find(choices, row.blend, &GraphCanvasBlendChoice::name);
            widgets.blend->setText(choice != choices.end() ? qtext(choice->label) : qtext(row.blend));
        }
        widgets.blend->setVisible(row.blend != "normal");
        line->addWidget(widgets.blend);
        widgets.filters = new QLabel(widgets.row);
        widgets.filters->setObjectName(prefix + QStringLiteral(".filters"));
        widgets.filters->setStyleSheet(QStringLiteral("font-style: italic; font-weight: bold;"));
        const bool filtered = row.kind != "adjustment" && !row.filters.empty();
        if (filtered) {
            QStringList names;
            for (const auto &filter : row.filters)
                if (const auto *node = findNode(nodes, filter)) names.push_back(qtext(graphCanvasFilterLabel(node->op)));
            widgets.filters->setText(QStringLiteral("f"));
            widgets.filters->setToolTip(tr("Filtered: %1").arg(names.join(QStringLiteral(", "))));
        }
        widgets.filters->setVisible(filtered);
        line->addWidget(widgets.filters);
        if (row.kind == "group" || row.kind == "import") {
            widgets.pass_through = new QToolButton(widgets.row);
            widgets.pass_through->setObjectName(prefix + QStringLiteral(".passThrough"));
            widgets.pass_through->setText(tr("PT"));
            widgets.pass_through->setCheckable(true);
            widgets.pass_through->setChecked(row.pass_through);
            widgets.pass_through->setAutoRaise(true);
            widgets.pass_through->setToolTip(
                tr("Pass through: the group's layers mix with what is under the group, not first among "
                   "themselves"));
            line->addWidget(widgets.pass_through);
        }
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
        connect(widgets.clip, &QToolButton::toggled, this, [this, index](bool on) {
            if (loading_) return;
            commit(graphCanvasSetLayerFlagOps(model_.nodes(scope_), scope_, stack_, index, "clip", on));
        });
        if (widgets.pass_through != nullptr)
            connect(widgets.pass_through, &QToolButton::toggled, this, [this, index](bool on) {
                if (loading_) return;
                commit(graphCanvasSetLayerFlagOps(model_.nodes(scope_), scope_, stack_, index, "pass_through", on));
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
    if (tell && !read_only_ && row && !row->source.empty() && nodePicked) nodePicked(scope_, row->source);
}

void GraphCanvasLayersPanel::loadFields() {
    loading_ = true;
    const auto row = pickedRow();
    const auto nodes = model_.nodes(scope_);
    const auto *shape = row && !row->shape.empty() && row->kind != "text" ? findNode(nodes, row->shape) : nullptr;
    const auto *paint = row && !row->paint.empty() ? findNode(nodes, row->source) : nullptr;
    const auto *image = row && row->kind == "image" ? findNode(nodes, row->source) : nullptr;
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
    image_fields_->setVisible(image != nullptr);
    if (image != nullptr) {
        const auto &parameters = image->parameters;
        for (auto [spin, key, component] : {std::tuple{offset_x_, "offset", 0}, std::tuple{offset_y_, "offset", 1},
                                            std::tuple{image_w_, "size", 0}, std::tuple{image_h_, "size", 1}}) {
            const QSignalBlocker block{spin};
            if (!spin->hasFocus())
                spin->setValue(number(parameters, key, static_cast<std::size_t>(component), 0.0));
        }
        const auto h = number(parameters, "size", 1, 0.0);
        size_ratio_ = h > 0.0 ? number(parameters, "size", 0, 0.0) / h : 1.0;
        const QSignalBlocker block{sampler_};
        const auto sampler = parameters.value("sampler", Json("linear"));
        sampler_->setCurrentIndex(std::max(0, sampler_->findData(qtext(sampler.is_string() ? sampler.get<std::string>()
                                                                                           : std::string{"linear"}))));
    }
    layer_fields_->setVisible(row.has_value());
    if (row) {
        const QSignalBlocker block{blend_};
        const int blend = blend_->findData(qtext(row->blend));
        if (blend >= 0) blend_->setCurrentIndex(blend);
        mask_caption_->setText(row->mask.empty() ? tr("none") : qtext(row->mask_source.empty() ? row->mask
                                                                                                : row->mask_source));
        // Folded for a shape, whose own centre and rotation are the fields
        // to reach for first; open for the rest, and wherever one is set.
        if (transform_row_ != row->input) {
            transform_row_ = row->input;
            transform_open_ = !row->transform.empty() || (row->kind != "rect" && row->kind != "ellipse");
        }
        const QSignalBlocker block_toggle{transform_toggle_};
        transform_toggle_->setChecked(transform_open_);
        // SG-3: an adjustment covers the whole canvas; nothing to move.
        transform_toggle_->setVisible(row->kind != "adjustment");
        transform_fields_->setVisible(transform_open_ && row->kind != "adjustment");
        const auto values = graphCanvasLayerTransform(nodes, *row);
        for (auto [spin, value] : {std::pair{translate_x_, values.translate[0]}, std::pair{translate_y_, values.translate[1]},
                                   std::pair{rotate_, values.rotate}, std::pair{scale_x_, values.scale[0]},
                                   std::pair{scale_y_, values.scale[1]}}) {
            const QSignalBlocker block_spin{spin};
            if (!spin->hasFocus()) spin->setValue(value);
        }
    }
    const auto *stack = findNode(nodes, stack_);
    stack_fields_->setVisible(!row && stack != nullptr);
    if (!row && stack != nullptr) {
        for (auto [spin, component] : {std::pair{stack_w_, 0}, std::pair{stack_h_, 1}}) {
            const QSignalBlocker block{spin};
            if (!spin->hasFocus())
                spin->setValue(number(stack->parameters, "size", static_cast<std::size_t>(component), 0.0));
        }
        const QSignalBlocker block{blend_space_};
        const auto space = stack->parameters.value("blend_space", Json("linear"));
        blend_space_->setCurrentIndex(
            std::max(0, blend_space_->findData(qtext(space.is_string() ? space.get<std::string>() : "linear"))));
    }
    open_group_->setVisible(row && (row->kind == "group" || row->kind == "import"));
    loadSg3Fields(row);
    loading_ = false;
}

void GraphCanvasLayersPanel::followEditable() {
    const bool editable = model_.canEdit();
    for (std::size_t index = 0; index < row_widgets_.size() && index < shown_rows_.size(); ++index) {
        auto &widgets = row_widgets_[index];
        widgets.visible->setEnabled(editable);
        widgets.clip->setEnabled(editable && widgets.clip->property("pelicanClipAllowed").toBool());
        widgets.name->setEnabled(editable);
        widgets.opacity->setEnabled(editable && !shown_rows_[index].opacity_driven);
        if (widgets.pass_through != nullptr) widgets.pass_through->setEnabled(editable);
    }
    const auto row = pickedRow();
    std::size_t count = 0;
    const auto nodes = model_.nodes(scope_);
    if (const auto *stack = findNode(nodes, stack_))
        if (const auto layers = stack->parameters.find("layers");
            layers != stack->parameters.end() && layers->is_array())
            count = layers->size();
    add_canvas_->setEnabled(editable);
    add_rect_->setEnabled(editable);
    add_ellipse_->setEnabled(editable);
    add_group_->setEnabled(editable);
    add_image_->setEnabled(editable);
    import_output_->setEnabled(editable);
    import_psd_->setEnabled(editable);
    add_stroke_->setEnabled(editable && row && !row->shape.empty());
    delete_->setEnabled(editable && row.has_value());
    up_->setEnabled(editable && row && row->index + 1 < count);
    down_->setEnabled(editable && row && row->index > 0);
    for (auto *spin : {size_x_, size_y_, center_x_, center_y_, corner_radius_, rotation_})
        spin->setEnabled(editable && !spin->property("pelicanDriven").toBool());
    for (QWidget *field : std::initializer_list<QWidget *>{
             color_, color_pick_, width_, align_, offset_x_, offset_y_, image_w_, image_h_, sampler_, blend_,
             translate_x_, translate_y_, rotate_, scale_x_, scale_y_, stack_w_, stack_h_, blend_space_})
        field->setEnabled(editable);
    add_mask_->setEnabled(editable && row && row->mask.empty());
    remove_mask_->setEnabled(editable && row && !row->mask.empty());
    // SG-3.
    add_text_->setEnabled(editable);
    add_adjustment_->setEnabled(editable);
    for (QWidget *field : std::initializer_list<QWidget *>{
             text_, font_, import_font_, text_size_, text_align_, line_height_, tracking_, box_width_, text_stroke_,
             filter_, add_filter_, brightness_, contrast_, input_floor_, input_ceiling_, gamma_, output_floor_,
             output_ceiling_, channels_, hue_, saturation_, lightness_, colorize_, stops_, reversed_, overlay_pick_,
             place_target_, place_scale_, pixels_per_unit_, depth_test_, depth_write_, double_sided_,
             // CV-3a.
             blur_radius_, blur_max_radius_, shadow_offset_[0], shadow_offset_[1], shadow_radius_, shadow_max_radius_,
             shadow_spread_, shadow_opacity_, shadow_color_, glow_radius_, glow_max_radius_, glow_spread_,
             glow_opacity_, glow_color_})
        field->setEnabled(editable);
    for (auto *spin : {position_[0], position_[1], position_[2], place_rotation_[0], place_rotation_[1],
                       place_rotation_[2], anchor_[0], anchor_[1]})
        spin->setEnabled(editable);
    const auto filter = pickedFilter();
    remove_filter_->setEnabled(editable && row && !filter.empty() &&
                               !(row->kind == "adjustment" && row->filters.size() == 1));
    const bool scene = !graphCanvasScenePasses(nodes).empty();
    place_in_scene_->setEnabled(editable && scene);
    place_in_scene_->setToolTip(scene ? tr("Draw this canvas in the 3D scene as well: a canvas.place on the "
                                           "pass's colour, tested against its depth, two units wide at the "
                                           "origin (the 2D output stays)")
                                      : tr("Needs a 3D pass: the document has no render.material_pass to draw "
                                           "the canvas into"));
    // CV-2a's refusals, as the sentence the dock has for them.
    const auto &notice = model_.editNotice();
    const auto sentence = notice.kind == InspectorNoticeKind::Error ? graphCanvasRefusalSentence(notice.message)
                                                                    : std::string{};
    refusal_->setText(qtext(sentence));
    refusal_->setVisible(!sentence.empty());
}

void GraphCanvasLayersPanel::say(const QString &line) {
    note_->setText(line);
    note_->setVisible(!line.isEmpty());
}

void GraphCanvasLayersPanel::commit(Json ops) {
    if (ops.empty()) return;
    if (read_only_) {
        say(tr("These are an imported PSD's layers: they are read only here. Change the PSD and import it "
               "again."));
        reload(true);
        return;
    }
    // The layer a gesture puts on this stack is the one picked next: the
    // input its new `layers` holds that the stack's did not.
    std::string added;
    const auto before = graphCanvasLayerRows(model_.nodes(scope_), stack_);
    for (const auto &op : ops)
        if (op.value("op", std::string{}) == "set_node_parameters" && op.value("node", std::string{}) == stack_)
            for (const auto &element : op["parameters"].value("layers", Json::array()))
                if (const auto input = element.value("input", std::string{});
                    !input.empty() && std::ranges::find(before, input, &GraphCanvasStackRow::input) == before.end())
                    added = input;
    const bool sent = model_.submitEdit(std::move(ops));
    if (sent) {
        note_->hide();
        if (!added.empty()) picked_ = added;
    } else {
        // Refused before it was sent (the model's notice says why) or the
        // last edit is still out: the fields say what the document says.
        const auto &notice = model_.editNotice();
        say(notice.kind == InspectorNoticeKind::Error && !notice.message.empty()
                ? qtext(notice.message)
                : tr("The last change is still being saved; try again."));
        reload(true);
    }
    if (submitted_) submitted_();
}

void GraphCanvasLayersPanel::commitParameter(const std::string &node, const std::string &key,
                                             const Json &value) {
    commit(graphCanvasSetParameterOps(model_.nodes(scope_), scope_, node, key, value));
}

void GraphCanvasLayersPanel::commitTransform() {
    if (loading_) return;
    const auto row = pickedRow();
    if (!row) return;
    GraphCanvasTransformValues values;
    values.translate = {translate_x_->value(), translate_y_->value()};
    values.rotate = rotate_->value();
    values.scale = {scale_x_->value(), scale_y_->value()};
    commit(graphCanvasSetTransformOps(model_.nodes(scope_), scope_, stack_, row->index, values));
}

void GraphCanvasLayersPanel::chooseFile(const QString &title, const QString &name, const QString &filter,
                                        std::function<void(const QStringList &)> picked) {
    // Qt's own dialog, as the sound import's: a scenario can name its file
    // field, and it is answered when it closes, with no event loop under it.
    auto *dialog = new QFileDialog{this, title};
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(name);
    dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    dialog->setFileMode(name == QLatin1String{"pelican.canvasImageDialog"} ? QFileDialog::ExistingFiles
                                                                            : QFileDialog::ExistingFile);
    dialog->setNameFilter(filter);
    if (auto *field = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        field->setObjectName(name + QStringLiteral(".fileName"));
    connect(dialog, &QFileDialog::filesSelected, this, std::move(picked));
    dialog->open();
}

void GraphCanvasLayersPanel::chooseImages(bool mask) {
    if (!projectRoot || projectRoot().empty()) {
        say(tr("Open a project to add pictures to."));
        return;
    }
    chooseFile(mask ? tr("Mask image") : tr("Add images"), QStringLiteral("pelican.canvasImageDialog"),
               tr("Pictures (*.png *.jpg *.jpeg *.PNG *.JPG *.JPEG)"), [this, mask](const QStringList &picked) {
                   std::vector<std::filesystem::path> files;
                   for (const auto &file : picked) files.push_back(localPath(file));
                   if (mask) {
                       if (!files.empty()) addMaskImage(files.front());
                   } else {
                       addImages(files);
                   }
               });
}

void GraphCanvasLayersPanel::addImages(const std::vector<std::filesystem::path> &files) {
    if (files.empty() || stack_.empty()) return;
    const auto root = projectRoot ? projectRoot() : std::filesystem::path{};
    const auto imported = importCanvasImages(root, files);
    // One edit for them all, each on top of the one before.
    auto nodes = model_.nodes(scope_);
    Json ops = Json::array();
    std::string error = imported.error;
    for (std::size_t at = 0; at < imported.copies.size(); ++at) {
        const auto size = pixelSize(imported.copies[at]);
        if (!size) {
            error = imported.references[at] + " cannot be read as a picture.";
            continue;
        }
        auto more = graphCanvasAddImageLayerOps(nodes, scope_, stack_, imported.references[at], *size,
                                                utf8Stem(files.size() == imported.copies.size() ? files[at]
                                                                                                : imported.copies[at]));
        applyLocally(nodes, more);
        for (auto &op : more) ops.push_back(std::move(op));
    }
    commit(std::move(ops));
    if (!error.empty()) say(qtext(error));
    else if (!imported.references.empty())
        say(tr("Copied %1 into the project.").arg(qtext(imported.references.front()) +
                                                  (imported.references.size() > 1
                                                       ? tr(" and %1 more").arg(imported.references.size() - 1)
                                                       : QString{})));
}

void GraphCanvasLayersPanel::addMaskImage(const std::filesystem::path &file) {
    const auto row = pickedRow();
    if (!row) return;
    const auto imported = importCanvasImages(projectRoot ? projectRoot() : std::filesystem::path{}, {file});
    if (imported.copies.empty()) {
        say(qtext(imported.error));
        return;
    }
    const auto size = pixelSize(imported.copies.front());
    if (!size) {
        say(tr("%1 cannot be read as a picture.").arg(qtext(imported.references.front())));
        return;
    }
    commit(graphCanvasAddMaskOps(model_.nodes(scope_), scope_, stack_, row->index, imported.references.front(),
                                 *size));
}

void GraphCanvasLayersPanel::importPsdOutput(const std::filesystem::path &graph_json) {
    const auto root = projectRoot ? projectRoot() : std::filesystem::path{};
    if (root.empty()) {
        say(tr("Open a project to import into."));
        return;
    }
    const auto output = readCanvasPsdOutput(graph_json);
    if (!output.error.empty()) {
        say(qtext(output.error));
        return;
    }
    bool replace = false;
    if (std::filesystem::exists(root / "imports" / std::filesystem::u8path(output.directory))) {
        const auto answer = QMessageBox::question(
            this, tr("Import PSD output"),
            tr("imports/%1 is already in the project. Replace its files with these?").arg(qtext(output.directory)));
        if (answer != QMessageBox::Yes) return;
        replace = true;
    }
    if (const auto error = copyCanvasPsdOutput(root, output, replace); !error.empty()) {
        say(qtext(error));
        return;
    }
    // A definition of that name already in the document: the next free one.
    const auto scopes = model_.scopes();
    const auto taken = [&](const std::string &name) {
        return std::ranges::any_of(scopes, [&](const GraphCanvasScope &scope) { return scope.id == name; });
    };
    auto name = output.definition_name;
    for (int suffix = 2; taken(name) && suffix < 10000; ++suffix)
        name = output.definition_name + "_" + std::to_string(suffix);
    const auto definition = graphCanvasRebaseImportAssets(output.definition, output.directory);
    auto label = output.definition_name;
    if (label.starts_with("psd_")) label = label.substr(4);
    if (stack_.empty() || read_only_) {
        // No canvas to put it on: a new one, with the import on it.
        auto nodes = model_.nodes({});
        auto ops = graphCanvasCompositionOps(nodes, {});
        if (ops.empty()) return;
        const auto output_id = ops.back()["node"].value("id", std::string{});
        const auto new_stack = ops.front()["node"].value("id", std::string{});
        applyLocally(nodes, ops);
        for (auto &op : graphCanvasImportDefinitionOps(nodes, {}, new_stack, name, definition, label))
            ops.push_back(std::move(op));
        trail_.clear();
        read_only_ = false;
        scope_.clear();
        stack_ = new_stack;
        if (addCanvas) addCanvas(std::move(ops), output_id);
        else commit(std::move(ops));
        if (submitted_) submitted_();
    } else {
        commit(graphCanvasImportDefinitionOps(model_.nodes(scope_), scope_, stack_, name, definition, label));
    }
    imports_->setText(tr("imported %1 layers, %2 notes").arg(output.layers).arg(output.diagnostics.size()));
    QStringList lines;
    for (const auto &line : output.diagnostics) lines.push_back(qtext(line));
    import_list_->setText(lines.isEmpty() ? tr("No notes.") : lines.join(QLatin1Char('\n')));
    imports_->setChecked(false);
    import_list_->hide();
    imports_->show();
}

void GraphCanvasLayersPanel::importPsd(const std::filesystem::path &psd) {
    // pelican-import-tools psd-canvas into a directory of its own, then the
    // same road as Import PSD output.
    const auto tools = qEnvironmentVariable("PELICAN_IMPORT_TOOLS");
    if (tools.isEmpty()) return;
    auto stem = utf8Stem(psd);
    for (auto &c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
    const auto out = QDir{QDir::tempPath()}.filePath(
        QStringLiteral("pelican_psd_%1_%2").arg(qtext(stem)).arg(QDateTime::currentMSecsSinceEpoch()));
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("uv"));
    process->setArguments({QStringLiteral("run"), QStringLiteral("--project"), tools, QStringLiteral("pelican-import-tools"),
                           QStringLiteral("psd-canvas"), QString::fromStdWString(psd.wstring()), out,
                           QStringLiteral("--asset-prefix"), QStringLiteral("project://imports/") + qtext(stem)});
    say(tr("Converting %1\xE2\x80\xA6").arg(QString::fromStdWString(psd.filename().wstring())));
    connect(process, &QProcess::finished, this, [this, process, out](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            const auto error = QString::fromUtf8(process->readAllStandardError()).trimmed();
            say(tr("psd-canvas failed: %1").arg(error.section(QLatin1Char('\n'), -1)));
            return;
        }
        importPsdOutput(localPath(QDir{out}.filePath(QStringLiteral("canvas.graph.json"))));
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        say(tr("psd-canvas could not be started (uv is not on PATH?)."));
    });
    process->start();
}

void GraphCanvasLayersPanel::dragEnterEvent(QDragEnterEvent *event) {
    if (!stack_.empty() && !pictures(event->mimeData()).empty()) event->acceptProposedAction();
    else QWidget::dragEnterEvent(event);
}

void GraphCanvasLayersPanel::dropEvent(QDropEvent *event) {
    const auto files = pictures(event->mimeData());
    if (files.empty()) return QWidget::dropEvent(event);
    event->acceptProposedAction();
    addImages(files);
}

// --- SG-3: text, adjustment layers and filters, 3D.

namespace {

std::string scopeToken(const std::string &scope) { return scope.empty() ? std::string{"root"} : scope; }

// The words a size combo shows for a size: a whole number without a point.
QString sizeText(double size) {
    return std::abs(size - std::round(size)) < 1e-9 ? QString::number(static_cast<long long>(std::llround(size)))
                                                    : QString::number(size, 'g', 6);
}

QComboBox *sizeCombo(QWidget *parent, const QString &name) {
    auto *combo = new QComboBox(parent);
    combo->setObjectName(name);
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    for (const auto size : graphCanvasTextSizes()) combo->addItem(sizeText(size));
    combo->setToolTip(QObject::tr("The text's size: one em, in the canvas's pixels"));
    return combo;
}

std::optional<double> sizeOf(const QComboBox &combo) {
    bool ok = false;
    const auto value = combo.currentText().trimmed().toDouble(&ok);
    if (!ok || !(value > 0.0)) return std::nullopt;
    return value;
}

// A font reference as the combos show it: its path in the project.
QString fontText(const std::string &reference) {
    constexpr std::string_view prefix = "project://";
    return QString::fromStdString(reference.starts_with(prefix) ? reference.substr(prefix.size()) : reference);
}

QDoubleSpinBox *placeSpin(QWidget *parent, const QString &name, double low, double high, int decimals, double step) {
    auto *spin = numberSpin(parent, name, low, high);
    spin->setDecimals(decimals);
    spin->setSingleStep(step);
    return spin;
}

} // namespace

QMenu *GraphCanvasLayersPanel::filterMenu(const char *prefix, const std::vector<GraphCanvasFilterChoice> &choices,
                                          std::function<void(const std::string &op)> chosen) {
    auto *menu = new QMenu(this);
    menu->setObjectName(named(prefix) + QStringLiteral(".menu"));
    for (const auto &choice : choices) {
        auto *action = menu->addAction(qtext(choice.label));
        action->setObjectName(named(prefix) + QLatin1Char('.') + qtext(choice.op.substr(choice.op.find('.') + 1)));
        connect(action, &QAction::triggered, this, [chosen, op = choice.op] { chosen(op); });
    }
    return menu;
}

void GraphCanvasLayersPanel::buildSg3Fields(QVBoxLayout *body) {
    // The picked text row's text and type; its colour is the paint's field.
    text_fields_ = new QWidget(body_);
    text_fields_->setObjectName(named("text"));
    auto *text_form = new QFormLayout(text_fields_);
    text_form->setContentsMargins(0, 4, 0, 0);
    text_ = new QPlainTextEdit(text_fields_);
    text_->setObjectName(named("field.text"));
    text_->setToolTip(tr("The text; a new line starts a line. Sent when the field loses the keyboard."));
    text_->setTabChangesFocus(true);
    text_->setMaximumHeight(64);
    text_->installEventFilter(this);
    text_form->addRow(caption(tr("Text"), text_fields_), text_);
    auto *font_row = new QWidget(text_fields_);
    auto *font_layout = new QHBoxLayout(font_row);
    font_layout->setContentsMargins(0, 0, 0, 0);
    font_ = new QComboBox(font_row);
    font_->setObjectName(named("field.font"));
    font_->setToolTip(tr("A font file of the project's (.ttf, .otf)"));
    font_layout->addWidget(font_, 1);
    import_font_ = new QPushButton(tr("Import font\xE2\x80\xA6"), font_row);
    import_font_->setObjectName(named("field.importFont"));
    import_font_->setToolTip(tr("A .ttf or .otf copied into the project's assets/fonts"));
    font_layout->addWidget(import_font_);
    text_form->addRow(caption(tr("Font"), text_fields_), font_row);
    text_size_ = sizeCombo(text_fields_, named("field.textSize"));
    text_form->addRow(caption(tr("Size"), text_fields_), text_size_);
    text_align_ = new QComboBox(text_fields_);
    text_align_->setObjectName(named("field.textAlign"));
    text_align_->setToolTip(tr("Each line against the origin (or within the box when it wraps)"));
    for (const auto *choice : {"left", "center", "right"}) text_align_->addItem(QLatin1String{choice}, QLatin1String{choice});
    text_form->addRow(caption(tr("Align"), text_fields_), text_align_);
    line_height_ = placeSpin(text_fields_, named("field.lineHeight"), 0.1, 10.0, 2, 0.1);
    line_height_->setToolTip(tr("From one line to the next, in em"));
    text_form->addRow(caption(tr("Line height"), text_fields_), line_height_);
    tracking_ = placeSpin(text_fields_, named("field.tracking"), -1000.0, 1000.0, 0, 10.0);
    tracking_->setSuffix(tr(" /1000 em"));
    tracking_->setToolTip(tr("Added after each character but the last of a line"));
    text_form->addRow(caption(tr("Tracking"), text_fields_), tracking_);
    box_width_ = placeSpin(text_fields_, named("field.boxWidth"), 0.0, 1.0e6, 0, 10.0);
    box_width_->setSpecialValueText(tr("no wrap"));
    box_width_->setToolTip(tr("Wrap the lines at this width (the canvas's pixels); 0 does not wrap"));
    text_form->addRow(caption(tr("Box width"), text_fields_), box_width_);
    text_stroke_ = new QCheckBox(tr("Stroke"), text_fields_);
    text_stroke_->setObjectName(named("field.textStroke"));
    text_stroke_->setToolTip(tr("An outline of the text, as a layer just above it"));
    text_form->addRow(text_stroke_);
    body->insertWidget(body->indexOf(paint_fields_), text_fields_);

    const auto textRow = [this]() -> std::optional<GraphCanvasStackRow> {
        const auto row = pickedRow();
        return row && row->kind == "text" ? row : std::nullopt;
    };
    const auto commitText = [this, textRow](const char *key, const Json &value) {
        if (loading_) return;
        if (const auto row = textRow())
            commit(graphCanvasSetTextParameterOps(model_.nodes(scope_), scope_, row->shape, key, value));
    };
    connect(font_, &QComboBox::currentIndexChanged, this, [this, commitText](int index) {
        if (index >= 0) commitText("font", font_->itemData(index).toString().toStdString());
    });
    connect(import_font_, &QPushButton::clicked, this, [this, commitText] {
        chooseFont(this, [commitText](const std::string &reference) { commitText("font", reference); });
    });
    const auto commitSize = [this, commitText] {
        if (const auto size = sizeOf(*text_size_)) commitText("size", *size);
    };
    connect(text_size_, &QComboBox::currentIndexChanged, this, commitSize);
    connect(text_size_->lineEdit(), &QLineEdit::editingFinished, this, commitSize);
    connect(text_align_, &QComboBox::currentIndexChanged, this, [this, commitText](int index) {
        if (index >= 0) commitText("align", text_align_->itemData(index).toString().toStdString());
    });
    connect(line_height_, &QDoubleSpinBox::valueChanged, this, [commitText](double value) { commitText("line_height", value); });
    connect(tracking_, &QDoubleSpinBox::valueChanged, this, [commitText](double value) { commitText("tracking", value); });
    connect(box_width_, &QDoubleSpinBox::valueChanged, this, [commitText](double value) { commitText("box_width", value); });
    connect(text_stroke_, &QCheckBox::toggled, this, [this, textRow](bool on) {
        if (loading_) return;
        const auto row = textRow();
        if (!row) return;
        const auto nodes = model_.nodes(scope_);
        const auto stroke = graphCanvasStrokeRowOf(nodes, stack_, row->index);
        // The text row stays picked: the box is one of its fields.
        const auto keep = picked_;
        if (on && !stroke) commit(graphCanvasAddStrokeLayerOps(nodes, scope_, stack_, row->index));
        else if (!on && stroke) commit(graphCanvasDeleteLayerOps(nodes, scope_, stack_, *stroke));
        picked_ = keep;
    });

    // Every picked row's filters, and the picked filter's parameters.
    filter_fields_ = new QWidget(body_);
    filter_fields_->setObjectName(named("filterFields"));
    auto *filter_form = new QFormLayout(filter_fields_);
    filter_form->setContentsMargins(0, 4, 0, 0);
    auto *filter_row = new QWidget(filter_fields_);
    auto *filter_layout = new QHBoxLayout(filter_row);
    filter_layout->setContentsMargins(0, 0, 0, 0);
    filter_ = new QComboBox(filter_row);
    filter_->setObjectName(named("field.filter"));
    filter_->setToolTip(tr("The layer's filters, in the order they apply"));
    filter_layout->addWidget(filter_, 1);
    add_filter_ = new QToolButton(filter_row);
    add_filter_->setObjectName(named("field.addFilter"));
    add_filter_->setText(tr("+ Filter"));
    add_filter_->setToolTip(tr("A filter on this layer alone, over the ones it has"));
    add_filter_->setPopupMode(QToolButton::InstantPopup);
    add_filter_->setMenu(filterMenu("addFilter", graphCanvasFilterChoices(), [this](const std::string &op) {
        const auto row = pickedRow();
        if (!row) return;
        auto ops = graphCanvasAddFilterOps(model_.nodes(scope_), scope_, stack_, row->index, op);
        if (ops.empty()) return;
        filter_row_ = ops.front()["node"].value("id", std::string{});
        commit(std::move(ops));
    }));
    filter_layout->addWidget(add_filter_);
    remove_filter_ = new QPushButton(tr("Remove filter"), filter_row);
    remove_filter_->setObjectName(named("field.removeFilter"));
    remove_filter_->setToolTip(tr("Take the shown filter off the layer"));
    filter_layout->addWidget(remove_filter_);
    filter_form->addRow(caption(tr("Filters"), filter_fields_), filter_row);
    filter_pages_ = new QStackedWidget(filter_fields_);
    const auto page = [this](QFormLayout *&form) {
        auto *made = new QWidget(filter_pages_);
        form = new QFormLayout(made);
        form->setContentsMargins(0, 0, 0, 0);
        filter_pages_->addWidget(made);
        return made;
    };
    const auto spin = [](QWidget *parent, const char *key, double low, double high, int decimals, double step) {
        return placeSpin(parent, named("field.filter.") + QLatin1String{key}, low, high, decimals, step);
    };
    QFormLayout *form = nullptr;
    auto *none = page(form);
    form->addRow(caption(tr("Nothing to set."), none));
    auto *bc = page(form);
    brightness_ = spin(bc, "brightness", -150.0, 150.0, 0, 1.0);
    contrast_ = spin(bc, "contrast", -50.0, 100.0, 0, 1.0);
    form->addRow(caption(tr("Brightness"), bc), brightness_);
    form->addRow(caption(tr("Contrast"), bc), contrast_);
    auto *levels = page(form);
    input_floor_ = spin(levels, "input_floor", 0.0, 255.0, 0, 1.0);
    input_ceiling_ = spin(levels, "input_ceiling", 0.0, 255.0, 0, 1.0);
    gamma_ = spin(levels, "gamma", 0.01, 9.99, 2, 0.05);
    output_floor_ = spin(levels, "output_floor", 0.0, 255.0, 0, 1.0);
    output_ceiling_ = spin(levels, "output_ceiling", 0.0, 255.0, 0, 1.0);
    form->addRow(caption(tr("Input black"), levels), input_floor_);
    form->addRow(caption(tr("Input white"), levels), input_ceiling_);
    form->addRow(caption(tr("Gamma"), levels), gamma_);
    form->addRow(caption(tr("Output black"), levels), output_floor_);
    form->addRow(caption(tr("Output white"), levels), output_ceiling_);
    channels_ = new QLineEdit(levels);
    channels_->setObjectName(named("field.filter.channels"));
    channels_->setPlaceholderText(QStringLiteral("{\"red\": {\"gamma\": 1.2}}"));
    channels_->setToolTip(tr("Per channel (red, green, blue), the same five numbers, as JSON; empty for none"));
    form->addRow(caption(tr("Channels"), levels), channels_);
    auto *hs = page(form);
    hue_ = spin(hs, "hue", -180.0, 180.0, 0, 1.0);
    saturation_ = spin(hs, "saturation", -100.0, 100.0, 0, 1.0);
    lightness_ = spin(hs, "lightness", -100.0, 100.0, 0, 1.0);
    form->addRow(caption(tr("Hue"), hs), hue_);
    form->addRow(caption(tr("Saturation"), hs), saturation_);
    form->addRow(caption(tr("Lightness"), hs), lightness_);
    colorize_ = new QLineEdit(hs);
    colorize_->setObjectName(named("field.filter.colorize"));
    colorize_->setPlaceholderText(QStringLiteral("{\"hue\": 200, \"saturation\": 50, \"lightness\": 0}"));
    colorize_->setToolTip(tr("Colorize instead (hue, saturation, lightness), as JSON; empty for none"));
    form->addRow(caption(tr("Colorize"), hs), colorize_);
    auto *map = page(form);
    stops_ = new QLineEdit(map);
    stops_->setObjectName(named("field.filter.stops"));
    stops_->setToolTip(tr("The gradient: [{\"position\": 0..1, \"color\": [r, g, b]}, ...] (sRGB), up to 16"));
    form->addRow(caption(tr("Stops"), map), stops_);
    reversed_ = new QCheckBox(tr("Reversed"), map);
    reversed_->setObjectName(named("field.filter.reversed"));
    form->addRow(reversed_);
    auto *overlay = page(form);
    overlay_pick_ = new QPushButton(tr("Pick\xE2\x80\xA6"), overlay);
    overlay_pick_->setObjectName(named("field.filter.colorPick"));
    overlay_pick_->setToolTip(tr("The colour laid over the layer; its alpha is how much"));
    form->addRow(caption(tr("Color"), overlay), overlay_pick_);
    // CV-3a: Gaussian Blur, Drop Shadow and Outer Glow, in + Filter's order.
    const auto kindSpin = [](QWidget *parent, const char *kind, const char *key, double low, double high, int decimals,
                             double step) {
        return placeSpin(parent, named("field.filter.") + QLatin1String{kind} + QLatin1Char('.') + QLatin1String{key},
                         low, high, decimals, step);
    };
    const auto maxSpin = [&](QWidget *parent, const char *kind) {
        auto *spin = kindSpin(parent, kind, "maxRadius", 0.0, 250.0, 1, 1.0);
        spin->setSpecialValueText(tr("None"));
        spin->setToolTip(tr("The largest radius a line into Radius gives; a radius a line drives needs one"));
        return spin;
    };
    const auto percentSpin = [&](QWidget *parent, const char *kind, const char *key) {
        auto *spin = kindSpin(parent, kind, key, 0.0, 100.0, 0, 1.0);
        spin->setSuffix(QStringLiteral(" %"));
        return spin;
    };
    const auto colorButton = [](QWidget *parent, const char *kind, const QString &tip) {
        auto *button = new QPushButton(tr("Pick\xE2\x80\xA6"), parent);
        button->setObjectName(named("field.filter.") + QLatin1String{kind} + QStringLiteral(".colorPick"));
        button->setToolTip(tip);
        return button;
    };
    auto *blur = page(form);
    blur_radius_ = kindSpin(blur, "blur", "radius", 0.0, 250.0, 1, 0.5);
    blur_radius_->setToolTip(tr("Photoshop's Gaussian Blur radius, in the stack's pixels"));
    blur_max_radius_ = maxSpin(blur, "blur");
    form->addRow(caption(tr("Radius"), blur), blur_radius_);
    form->addRow(caption(tr("Max radius"), blur), blur_max_radius_);
    auto *shadow = page(form);
    auto *offset_row = new QWidget(shadow);
    auto *offset_layout = new QHBoxLayout(offset_row);
    offset_layout->setContentsMargins(0, 0, 0, 0);
    for (int axis = 0; axis < 2; ++axis) {
        shadow_offset_[axis] = kindSpin(offset_row, "drop_shadow", axis == 0 ? "offset.x" : "offset.y", -1000.0, 1000.0,
                                        1, 1.0);
        offset_layout->addWidget(shadow_offset_[axis]);
    }
    offset_row->setToolTip(tr("Where the shadow falls, in the stack's pixels (+y down)"));
    form->addRow(caption(tr("Offset"), shadow), offset_row);
    shadow_radius_ = kindSpin(shadow, "drop_shadow", "radius", 0.0, 250.0, 1, 1.0);
    shadow_radius_->setToolTip(tr("Photoshop's Size: how far the shadow reaches, in the stack's pixels"));
    shadow_max_radius_ = maxSpin(shadow, "drop_shadow");
    shadow_spread_ = percentSpin(shadow, "drop_shadow", "spread");
    shadow_spread_->setToolTip(tr("Photoshop's Spread: how much of the size is solid before it fades"));
    shadow_opacity_ = percentSpin(shadow, "drop_shadow", "opacity");
    shadow_color_ = colorButton(shadow, "drop_shadow", tr("The shadow's colour"));
    form->addRow(caption(tr("Size"), shadow), shadow_radius_);
    form->addRow(caption(tr("Max size"), shadow), shadow_max_radius_);
    form->addRow(caption(tr("Spread"), shadow), shadow_spread_);
    form->addRow(caption(tr("Opacity"), shadow), shadow_opacity_);
    form->addRow(caption(tr("Color"), shadow), shadow_color_);
    auto *glow = page(form);
    glow_radius_ = kindSpin(glow, "outer_glow", "radius", 0.0, 250.0, 1, 1.0);
    glow_radius_->setToolTip(tr("Photoshop's Size: how far the glow reaches, in the stack's pixels"));
    glow_max_radius_ = maxSpin(glow, "outer_glow");
    glow_spread_ = percentSpin(glow, "outer_glow", "spread");
    glow_spread_->setToolTip(tr("Photoshop's Spread: how much of the size is solid before it fades"));
    glow_opacity_ = percentSpin(glow, "outer_glow", "opacity");
    glow_color_ = colorButton(glow, "outer_glow", tr("The glow's colour"));
    form->addRow(caption(tr("Size"), glow), glow_radius_);
    form->addRow(caption(tr("Max size"), glow), glow_max_radius_);
    form->addRow(caption(tr("Spread"), glow), glow_spread_);
    form->addRow(caption(tr("Opacity"), glow), glow_opacity_);
    form->addRow(caption(tr("Color"), glow), glow_color_);
    filter_form->addRow(filter_pages_);
    body->insertWidget(body->indexOf(layer_fields_) + 1, filter_fields_);

    connect(filter_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (loading_ || index < 0) return;
        filter_row_ = filter_->itemData(index).toString().toStdString();
        loadFields();
        followEditable();
    });
    connect(remove_filter_, &QPushButton::clicked, this, [this] {
        const auto row = pickedRow();
        const auto filter = pickedFilter();
        if (row && !filter.empty())
            commit(graphCanvasRemoveFilterOps(model_.nodes(scope_), scope_, stack_, row->index, filter));
    });
    for (auto [field, key] : {std::pair{brightness_, "brightness"}, std::pair{contrast_, "contrast"},
                              std::pair{input_floor_, "input_floor"}, std::pair{input_ceiling_, "input_ceiling"},
                              std::pair{gamma_, "gamma"}, std::pair{output_floor_, "output_floor"},
                              std::pair{output_ceiling_, "output_ceiling"}, std::pair{hue_, "hue"},
                              std::pair{saturation_, "saturation"}, std::pair{lightness_, "lightness"}})
        connect(field, &QDoubleSpinBox::valueChanged, this,
                [this, key](double value) { commitFilterNumber(key, value); });
    connect(channels_, &QLineEdit::editingFinished, this, [this] { commitFilterJson("channels", channels_); });
    connect(colorize_, &QLineEdit::editingFinished, this, [this] { commitFilterJson("colorize", colorize_); });
    connect(stops_, &QLineEdit::editingFinished, this, [this] { commitFilterJson("stops", stops_); });
    connect(reversed_, &QCheckBox::toggled, this, [this](bool on) {
        if (loading_) return;
        const auto nodes = model_.nodes(scope_);
        const auto *filter = findNode(nodes, pickedFilter());
        if (filter == nullptr) return;
        auto parameters = filter->parameters;
        if (on) parameters["reversed"] = true;
        else parameters.erase("reversed");
        if (parameters != filter->parameters)
            commit(Json::array({Json{{"op", "set_node_parameters"}, {"scope", scopeToken(scope_)},
                                     {"node", filter->id}, {"parameters", std::move(parameters)}}}));
    });
    connect(overlay_pick_, &QPushButton::clicked, this, [this] {
        const auto nodes = model_.nodes(scope_);
        const auto *filter = findNode(nodes, pickedFilter());
        if (filter == nullptr || filter->op != "filter.color_overlay") return;
        const auto &parameters = filter->parameters;
        const QColor current = QColor::fromRgbF(
            static_cast<float>(number(parameters, "color", 0, 1.0)), static_cast<float>(number(parameters, "color", 1, 1.0)),
            static_cast<float>(number(parameters, "color", 2, 1.0)), static_cast<float>(number(parameters, "color", 3, 1.0)));
        const QColor chosen = QColorDialog::getColor(current, this, tr("Overlay colour"), QColorDialog::ShowAlphaChannel);
        if (!chosen.isValid()) return;
        commit(graphCanvasSetParameterOps(nodes, scope_, filter->id, "color",
                                          Json::array({chosen.redF(), chosen.greenF(), chosen.blueF(), chosen.alphaF()})));
    });
    // CV-3a.
    for (auto *field : {blur_radius_, shadow_radius_, glow_radius_})
        connect(field, &QDoubleSpinBox::valueChanged, this, [this](double value) { commitFilterNumber("radius", value); });
    for (auto *field : {blur_max_radius_, shadow_max_radius_, glow_max_radius_})
        connect(field, &QDoubleSpinBox::valueChanged, this, [this](double value) { commitFilterLimit(value); });
    for (auto [field, key] : {std::pair{shadow_spread_, "spread"}, std::pair{shadow_opacity_, "opacity"},
                              std::pair{glow_spread_, "spread"}, std::pair{glow_opacity_, "opacity"}})
        connect(field, &QDoubleSpinBox::valueChanged, this,
                [this, key](double value) { commitFilterNumber(key, value / 100.0); });
    for (auto *field : shadow_offset_)
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] {
            if (loading_) return;
            const auto filter = pickedFilter();
            if (filter.empty()) return;
            commit(graphCanvasSetParameterOps(model_.nodes(scope_), scope_, filter, "offset",
                                              Json::array({shadow_offset_[0]->value(), shadow_offset_[1]->value()})));
        });
    connect(shadow_color_, &QPushButton::clicked, this, [this] { pickFilterColor("Shadow colour"); });
    connect(glow_color_, &QPushButton::clicked, this, [this] { pickFilterColor("Glow colour"); });

    // No row picked: the stack in the 3D scene.
    auto *stack_form = qobject_cast<QFormLayout *>(stack_fields_->layout());
    place_row_ = new QWidget(stack_fields_);
    auto *place_layout = new QHBoxLayout(place_row_);
    place_layout->setContentsMargins(0, 0, 0, 0);
    place_target_ = new QComboBox(place_row_);
    place_target_->setObjectName(named("placeTarget"));
    place_target_->setToolTip(tr("The 3D pass, and its colour attachment, the canvas is drawn onto"));
    place_layout->addWidget(place_target_, 1);
    place_in_scene_ = new QPushButton(tr("Place in 3D scene\xE2\x80\xA6"), place_row_);
    place_in_scene_->setObjectName(named("placeInScene"));
    place_layout->addWidget(place_in_scene_);
    stack_form->addRow(caption(tr("3D"), stack_fields_), place_row_);
    place_fields_ = new QWidget(stack_fields_);
    place_fields_->setObjectName(named("place"));
    auto *place_form = new QFormLayout(place_fields_);
    place_form->setContentsMargins(0, 0, 0, 0);
    place_caption_ = caption({}, place_fields_);
    place_caption_->setStyleSheet(QStringLiteral("font-weight: bold;"));
    place_form->addRow(place_caption_);
    const auto triple = [&](const char *key, QDoubleSpinBox *(&spins)[3], double low, double high, int decimals,
                            double step) {
        auto *row = new QWidget(place_fields_);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        const char *components[] = {".x", ".y", ".z"};
        for (int axis = 0; axis < 3; ++axis) {
            spins[axis] = placeSpin(row, named("field.place.") + QLatin1String{key} + QLatin1String{components[axis]},
                                    low, high, decimals, step);
            row_layout->addWidget(spins[axis], 1);
        }
        return row;
    };
    place_form->addRow(caption(tr("Position"), place_fields_), triple("position", position_, -1.0e5, 1.0e5, 3, 0.1));
    place_form->addRow(caption(tr("Rotation"), place_fields_),
                       triple("rotation", place_rotation_, -360.0, 360.0, 1, 5.0));
    for (auto *each : place_rotation_) each->setSuffix(QStringLiteral("\xC2\xB0"));
    place_scale_ = placeSpin(place_fields_, named("field.place.scale"), -1.0e3, 1.0e3, 3, 0.1);
    place_form->addRow(caption(tr("Scale"), place_fields_), place_scale_);
    pixels_per_unit_ = placeSpin(place_fields_, named("field.place.pixels_per_unit"), 0.001, 1.0e6, 1, 10.0);
    pixels_per_unit_->setToolTip(tr("The canvas's pixels in one world unit"));
    place_form->addRow(caption(tr("Pixels per unit"), place_fields_), pixels_per_unit_);
    auto *anchor = new QWidget(place_fields_);
    auto *anchor_layout = new QHBoxLayout(anchor);
    anchor_layout->setContentsMargins(0, 0, 0, 0);
    for (int axis = 0; axis < 2; ++axis) {
        anchor_[axis] = placeSpin(anchor, named("field.place.anchor") + QLatin1String{axis == 0 ? ".x" : ".y"}, -10.0,
                                  10.0, 3, 0.05);
        anchor_layout->addWidget(anchor_[axis], 1);
    }
    anchor->setToolTip(tr("The point of the canvas at the position: 0 0 its top left, 0.5 0.5 its middle"));
    place_form->addRow(caption(tr("Anchor"), place_fields_), anchor);
    depth_test_ = new QCheckBox(tr("Depth test"), place_fields_);
    depth_test_->setObjectName(named("field.place.depth_test"));
    depth_write_ = new QCheckBox(tr("Depth write"), place_fields_);
    depth_write_->setObjectName(named("field.place.depth_write"));
    double_sided_ = new QCheckBox(tr("Double sided"), place_fields_);
    double_sided_->setObjectName(named("field.place.double_sided"));
    auto *flags = new QWidget(place_fields_);
    auto *flags_layout = new QHBoxLayout(flags);
    flags_layout->setContentsMargins(0, 0, 0, 0);
    for (auto *each : {depth_test_, depth_write_, double_sided_}) flags_layout->addWidget(each);
    place_form->addRow(flags);
    stack_form->addRow(place_fields_);

    connect(place_in_scene_, &QPushButton::clicked, this, [this] {
        const auto nodes = model_.nodes(scope_);
        const auto passes = graphCanvasScenePasses(nodes);
        const int at = place_target_->currentIndex();
        if (at < 0 || static_cast<std::size_t>(at) >= passes.size()) return;
        auto ops = graphCanvasPlaceOps(nodes, scope_, stack_, passes[static_cast<std::size_t>(at)]);
        if (ops.empty()) return;
        place_ = ops.front()["node"].value("id", std::string{});
        commit(std::move(ops));
    });
    for (auto *spins : {position_, place_rotation_})
        for (int axis = 0; axis < 3; ++axis)
            connect(spins[axis], &QDoubleSpinBox::valueChanged, this, [this, spins] {
                commitPlace(spins == position_ ? "position" : "rotation",
                            Json::array({spins[0]->value(), spins[1]->value(), spins[2]->value()}));
            });
    connect(place_scale_, &QDoubleSpinBox::valueChanged, this, [this](double value) { commitPlace("scale", value); });
    connect(pixels_per_unit_, &QDoubleSpinBox::valueChanged, this,
            [this](double value) { commitPlace("pixels_per_unit", value); });
    for (auto *component : anchor_)
        connect(component, &QDoubleSpinBox::valueChanged, this,
                [this] { commitPlace("anchor", Json::array({anchor_[0]->value(), anchor_[1]->value()})); });
    connect(depth_test_, &QCheckBox::toggled, this, [this](bool on) { commitPlace("depth_test", on); });
    connect(depth_write_, &QCheckBox::toggled, this, [this](bool on) { commitPlace("depth_write", on); });
    connect(double_sided_, &QCheckBox::toggled, this, [this](bool on) { commitPlace("double_sided", on); });
}

std::string GraphCanvasLayersPanel::pickedFilter() const {
    const auto row = pickedRow();
    if (!row || row->filters.empty()) return {};
    if (std::ranges::find(row->filters, filter_row_) != row->filters.end()) return filter_row_;
    return row->filters.back();
}

std::string GraphCanvasLayersPanel::shownPlace() const {
    const auto places = graphCanvasPlacesOf(model_.nodes(scope_), stack_);
    if (std::ranges::find(places, place_) != places.end()) return place_;
    return places.empty() ? std::string{} : places.front();
}

void GraphCanvasLayersPanel::loadSg3Fields(const std::optional<GraphCanvasStackRow> &row) {
    const auto nodes = model_.nodes(scope_);
    // Text.
    const auto *text = row && row->kind == "text" ? findNode(nodes, row->shape) : nullptr;
    text_fields_->setVisible(text != nullptr);
    if (text != nullptr) {
        const auto &parameters = text->parameters;
        if (!text_->hasFocus()) {
            const QSignalBlocker block{text_};
            const auto current = qtext(parameters.value("text", std::string{}));
            if (text_->toPlainText() != current) text_->setPlainText(current);
        }
        // The project's fonts, read again when another text row is picked.
        if (font_->property("pelicanFontsFor").toString() != qtext(row->input)) {
            font_->setProperty("pelicanFontsFor", qtext(row->input));
            reloadFonts();
        }
        fillFontCombo(font_, parameters.value("font", std::string{}));
        {
            const QSignalBlocker block{text_size_};
            if (!text_size_->lineEdit()->hasFocus()) {
                const auto size = number(parameters, "size", 0, graphCanvasDefaultTextSize);
                const int at = text_size_->findText(sizeText(size));
                if (at >= 0) text_size_->setCurrentIndex(at);
                text_size_->setEditText(sizeText(size));
            }
        }
        {
            const QSignalBlocker block{text_align_};
            const auto align = parameters.value("align", Json("left"));
            text_align_->setCurrentIndex(
                std::max(0, text_align_->findData(qtext(align.is_string() ? align.get<std::string>() : "left"))));
        }
        for (auto [spin, key, fallback] : {std::tuple{line_height_, "line_height", 1.2},
                                           std::tuple{tracking_, "tracking", 0.0},
                                           std::tuple{box_width_, "box_width", 0.0}}) {
            const QSignalBlocker block{spin};
            if (!spin->hasFocus()) spin->setValue(number(parameters, key, 0, fallback));
        }
        const QSignalBlocker block{text_stroke_};
        text_stroke_->setChecked(graphCanvasStrokeRowOf(nodes, stack_, row->index).has_value());
    }

    // Filters.
    filter_fields_->setVisible(row.has_value());
    if (row) {
        const auto picked = pickedFilter();
        {
            const QSignalBlocker block{filter_};
            filter_->clear();
            for (const auto &id : row->filters) {
                const auto *node = findNode(nodes, id);
                filter_->addItem(qtext(node != nullptr ? graphCanvasFilterLabel(node->op) + "  (" + id + ")" : id),
                                 qtext(id));
            }
            filter_->setCurrentIndex(filter_->findData(qtext(picked)));
        }
        filter_->setVisible(!row->filters.empty());
        remove_filter_->setVisible(!row->filters.empty());
        const auto *filter = picked.empty() ? nullptr : findNode(nodes, picked);
        filter_pages_->setVisible(filter != nullptr);
        if (filter != nullptr) {
            const auto &parameters = filter->parameters;
            const auto &choices = graphCanvasFilterChoices();
            const auto at = std::ranges::find(choices, filter->op, &GraphCanvasFilterChoice::op);
            filter_pages_->setCurrentIndex(at == choices.end() ? 0 : static_cast<int>(at - choices.begin()));
            const auto load = [&](QDoubleSpinBox *spin, const char *key, double fallback) {
                const QSignalBlocker block{spin};
                if (!spin->hasFocus()) spin->setValue(number(parameters, key, 0, fallback));
            };
            load(brightness_, "brightness", 0.0);
            load(contrast_, "contrast", 0.0);
            load(input_floor_, "input_floor", 0.0);
            load(input_ceiling_, "input_ceiling", 255.0);
            load(gamma_, "gamma", 1.0);
            load(output_floor_, "output_floor", 0.0);
            load(output_ceiling_, "output_ceiling", 255.0);
            load(hue_, "hue", 0.0);
            load(saturation_, "saturation", 0.0);
            load(lightness_, "lightness", 0.0);
            for (auto [field, key] : {std::pair{channels_, "channels"}, std::pair{colorize_, "colorize"},
                                      std::pair{stops_, "stops"}}) {
                if (field->hasFocus()) continue;
                const auto found = parameters.find(key);
                field->setText(found == parameters.end() ? QString{} : qtext(found->dump()));
            }
            // CV-3a: a blur's, a shadow's and a glow's (their defaults the engine's).
            const auto defaults = graphCanvasFilterDefaults(filter->op);
            const auto fallback = defaults.is_object() ? defaults : Json::object();
            const auto known = [&](const char *key, std::size_t index, double otherwise) {
                return number(parameters, key, index, number(fallback, key, index, otherwise));
            };
            for (auto *spin : {blur_radius_, shadow_radius_, glow_radius_}) {
                const QSignalBlocker block{spin};
                if (!spin->hasFocus()) spin->setValue(known("radius", 0, 0.0));
            }
            for (auto *spin : {blur_max_radius_, shadow_max_radius_, glow_max_radius_}) {
                const QSignalBlocker block{spin};
                if (!spin->hasFocus()) spin->setValue(number(parameters, "max_radius", 0, 0.0));
            }
            for (auto [spin, key] : {std::pair{shadow_spread_, "spread"}, std::pair{shadow_opacity_, "opacity"},
                                     std::pair{glow_spread_, "spread"}, std::pair{glow_opacity_, "opacity"}}) {
                const QSignalBlocker block{spin};
                if (!spin->hasFocus()) spin->setValue(100.0 * known(key, 0, 0.0));
            }
            for (std::size_t axis = 0; axis < 2; ++axis) {
                const QSignalBlocker block{shadow_offset_[axis]};
                if (!shadow_offset_[axis]->hasFocus()) shadow_offset_[axis]->setValue(known("offset", axis, 0.0));
            }
            const QIcon effect_color{swatchIcon({known("color", 0, 0.0), known("color", 1, 0.0), known("color", 2, 0.0),
                                                 1.0})};
            shadow_color_->setIcon(effect_color);
            glow_color_->setIcon(effect_color);
            const QSignalBlocker block{reversed_};
            reversed_->setChecked(parameters.value("reversed", false) == true);
            overlay_pick_->setIcon(QIcon{swatchIcon({number(parameters, "color", 0, 1.0), number(parameters, "color", 1, 1.0),
                                                     number(parameters, "color", 2, 1.0), 1.0})});
        }
    }

    // 3D.
    if (!row) {
        const auto passes = graphCanvasScenePasses(nodes);
        {
            const QSignalBlocker block{place_target_};
            const int keep = place_target_->currentIndex();
            place_target_->clear();
            for (const auto &pass : passes) place_target_->addItem(qtext(pass.node + " \xC2\xB7 " + pass.color));
            place_target_->setCurrentIndex(keep >= 0 && keep < place_target_->count() ? keep : 0);
        }
        place_target_->setVisible(passes.size() > 1);
        const auto place = shownPlace();
        const auto *placed = place.empty() ? nullptr : findNode(nodes, place);
        place_fields_->setVisible(placed != nullptr);
        if (placed != nullptr) {
            const auto &parameters = placed->parameters;
            place_caption_->setText(tr("Placed in 3D: %1").arg(qtext(placed->heading())));
            for (int axis = 0; axis < 3; ++axis)
                for (auto [spins, key, fallback] : {std::tuple{position_, "position", 0.0},
                                                    std::tuple{place_rotation_, "rotation", 0.0}}) {
                    const QSignalBlocker block{spins[axis]};
                    if (!spins[axis]->hasFocus())
                        spins[axis]->setValue(number(parameters, key, static_cast<std::size_t>(axis), fallback));
                }
            for (int axis = 0; axis < 2; ++axis) {
                const QSignalBlocker block{anchor_[axis]};
                if (!anchor_[axis]->hasFocus())
                    anchor_[axis]->setValue(number(parameters, "anchor", static_cast<std::size_t>(axis), 0.5));
            }
            for (auto [spin, key, fallback] : {std::tuple{place_scale_, "scale", 1.0},
                                               std::tuple{pixels_per_unit_, "pixels_per_unit", 100.0}}) {
                const QSignalBlocker block{spin};
                if (!spin->hasFocus()) spin->setValue(number(parameters, key, 0, fallback));
            }
            for (auto [box, key, fallback] : {std::tuple{depth_test_, "depth_test", true},
                                              std::tuple{depth_write_, "depth_write", false},
                                              std::tuple{double_sided_, "double_sided", true}}) {
                const QSignalBlocker block{box};
                const auto found = parameters.find(key);
                box->setChecked(found != parameters.end() && found->is_boolean() ? found->get<bool>() : fallback);
            }
        }
    }
}

void GraphCanvasLayersPanel::commitFilterNumber(const char *key, double value) {
    if (loading_) return;
    const auto filter = pickedFilter();
    if (!filter.empty()) commit(graphCanvasSetParameterOps(model_.nodes(scope_), scope_, filter, key, value));
}

void GraphCanvasLayersPanel::commitFilterLimit(double value) {
    if (loading_) return;
    const auto nodes = model_.nodes(scope_);
    const auto *filter = findNode(nodes, pickedFilter());
    if (filter == nullptr) return;
    auto parameters = filter->parameters;
    if (value > 0.0) parameters["max_radius"] = value;
    else parameters.erase("max_radius");
    if (parameters == filter->parameters) return;
    commit(Json::array({Json{{"op", "set_node_parameters"}, {"scope", scopeToken(scope_)}, {"node", filter->id},
                             {"parameters", std::move(parameters)}}}));
}

void GraphCanvasLayersPanel::pickFilterColor(const char *title) {
    const auto nodes = model_.nodes(scope_);
    const auto *filter = findNode(nodes, pickedFilter());
    if (filter == nullptr) return;
    const auto defaults = graphCanvasFilterDefaults(filter->op);
    const auto fallback = defaults.is_object() ? defaults : Json::object();
    const auto component = [&](std::size_t index) {
        return number(filter->parameters, "color", index, number(fallback, "color", index, index == 3 ? 1.0 : 0.0));
    };
    const QColor current = QColor::fromRgbF(static_cast<float>(component(0)), static_cast<float>(component(1)),
                                            static_cast<float>(component(2)), static_cast<float>(component(3)));
    const QColor chosen = QColorDialog::getColor(current, this, tr(title), QColorDialog::ShowAlphaChannel);
    if (!chosen.isValid()) return;
    commit(graphCanvasSetParameterOps(nodes, scope_, filter->id, "color",
                                      Json::array({chosen.redF(), chosen.greenF(), chosen.blueF(), chosen.alphaF()})));
}

void GraphCanvasLayersPanel::commitFilterJson(const char *key, QLineEdit *field) {
    if (loading_) return;
    const auto nodes = model_.nodes(scope_);
    const auto *filter = findNode(nodes, pickedFilter());
    if (filter == nullptr) return;
    auto parameters = filter->parameters;
    const auto text = field->text().trimmed().toStdString();
    if (text.empty()) {
        parameters.erase(key);
    } else {
        try {
            parameters[key] = Json::parse(text);
        } catch (const std::exception &failure) {
            say(tr("%1 is not JSON: %2").arg(QLatin1String{key}, QString::fromUtf8(failure.what())));
            return;
        }
    }
    if (parameters == filter->parameters) return;
    commit(Json::array({Json{{"op", "set_node_parameters"}, {"scope", scopeToken(scope_)}, {"node", filter->id},
                             {"parameters", std::move(parameters)}}}));
}

void GraphCanvasLayersPanel::commitPlace(const char *key, const Json &value) {
    if (loading_) return;
    const auto place = shownPlace();
    if (!place.empty()) commit(graphCanvasSetPlaceParameterOps(model_.nodes(scope_), scope_, place, key, value));
}

void GraphCanvasLayersPanel::reloadFonts() {
    const auto read = readCanvasProjectFonts(projectRoot ? projectRoot() : std::filesystem::path{});
    fonts_ = read.fonts;
}

void GraphCanvasLayersPanel::fillFontCombo(QComboBox *combo, const std::string &current) {
    const QSignalBlocker block{combo};
    combo->clear();
    for (const auto &font : fonts_) combo->addItem(fontText(font), qtext(font));
    if (!current.empty() && std::ranges::find(fonts_, current) == fonts_.end())
        combo->addItem(fontText(current), qtext(current));
    combo->setCurrentIndex(current.empty() ? (combo->count() > 0 ? 0 : -1) : combo->findData(qtext(current)));
}

void GraphCanvasLayersPanel::chooseFont(QWidget *over, std::function<void(const std::string &reference)> imported) {
    if (!projectRoot || projectRoot().empty()) {
        say(tr("Open a project to add fonts to."));
        return;
    }
    // Qt's own dialog, as the pictures' (chooseFile), over the window that asked.
    auto *dialog = new QFileDialog{over, tr("Import font")};
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("pelican.canvasFontDialog"));
    dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    dialog->setFileMode(QFileDialog::ExistingFile);
    dialog->setNameFilter(tr("Fonts (*.ttf *.otf *.TTF *.OTF)"));
    if (auto *field = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        field->setObjectName(QStringLiteral("pelican.canvasFontDialog.fileName"));
    connect(dialog, &QFileDialog::filesSelected, this, [this, imported = std::move(imported)](const QStringList &picked) {
        if (picked.isEmpty()) return;
        const auto copied = importCanvasFont(projectRoot(), localPath(picked.front()));
        if (!copied.error.empty()) {
            say(qtext(copied.error));
            return;
        }
        reloadFonts();
        say(tr("Copied %1 into the project.").arg(qtext(copied.reference)));
        imported(copied.reference);
    });
    dialog->open();
}

void GraphCanvasLayersPanel::openTextDialog() {
    if (stack_.empty()) return;
    if (!projectRoot || projectRoot().empty()) {
        say(tr("Open a project to add text to."));
        return;
    }
    reloadFonts();
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("pelican.canvasTextDialog"));
    dialog->setWindowTitle(tr("Add text"));
    const auto name = [](const char *suffix) { return QStringLiteral("pelican.canvasTextDialog.") + QLatin1String{suffix}; };
    auto *form = new QFormLayout(dialog);
    auto *text = new QLineEdit(dialog);
    text->setObjectName(name("text"));
    text->setPlaceholderText(tr("The text"));
    form->addRow(tr("Text"), text);
    auto *font_row = new QWidget(dialog);
    auto *font_layout = new QHBoxLayout(font_row);
    font_layout->setContentsMargins(0, 0, 0, 0);
    auto *font = new QComboBox(font_row);
    font->setObjectName(name("font"));
    font->setToolTip(tr("A font file of the project's (.ttf, .otf)"));
    font_layout->addWidget(font, 1);
    auto *import = new QPushButton(tr("Import font\xE2\x80\xA6"), font_row);
    import->setObjectName(name("importFont"));
    import->setToolTip(tr("A .ttf or .otf copied into the project's assets/fonts"));
    font_layout->addWidget(import);
    form->addRow(tr("Font"), font_row);
    auto *size = sizeCombo(dialog, name("size"));
    size->setCurrentIndex(size->findText(sizeText(graphCanvasDefaultTextSize)));
    form->addRow(tr("Size"), size);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto *ok = buttons->button(QDialogButtonBox::Ok);
    ok->setObjectName(name("ok"));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(name("cancel"));
    form->addRow(buttons);
    fillFontCombo(font, {});
    const auto settle = [text, font, size, ok] {
        ok->setEnabled(!text->text().isEmpty() && font->currentIndex() >= 0 && sizeOf(*size).has_value());
    };
    settle();
    connect(text, &QLineEdit::textChanged, dialog, settle);
    connect(font, &QComboBox::currentIndexChanged, dialog, settle);
    connect(size, &QComboBox::currentTextChanged, dialog, settle);
    connect(import, &QPushButton::clicked, dialog, [this, dialog, font, settle] {
        chooseFont(dialog, [this, font, settle](const std::string &reference) {
            fillFontCombo(font, reference);
            settle();
        });
    });
    connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this, text, font, size] {
        const auto chosen = sizeOf(*size);
        if (!chosen || font->currentIndex() < 0) return;
        commit(graphCanvasAddTextLayerOps(model_.nodes(scope_), scope_, stack_, text->text().toStdString(),
                                          font->currentData().toString().toStdString(), *chosen));
    });
    dialog->open();
}

bool GraphCanvasLayersPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == text_ && event->type() == QEvent::FocusOut && !loading_) {
        const auto row = pickedRow();
        if (row && row->kind == "text")
            commit(graphCanvasSetTextParameterOps(model_.nodes(scope_), scope_, row->shape, "text",
                                                  text_->toPlainText().toStdString()));
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace PelicanStudio
