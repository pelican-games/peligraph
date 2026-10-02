#include "graphcanvaslayerspanel.hpp"

#include "../model/graphcanvasimport.hpp"
#include "../model/graphcanvasmodel.hpp"
#include "studiotheme.hpp"

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardItemModel>
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

constexpr const char *kBlends[] = {"normal", "multiply", "screen", "overlay", "add"};

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
    for (const auto *choice : kBlends) blend_->addItem(QLatin1String{choice}, QLatin1String{choice});
    // Listed, not chosen: the engine refuses them (canvas.blend_unsupported@1).
    if (auto *items = qobject_cast<QStandardItemModel *>(blend_->model()))
        for (int index = 1; index < items->rowCount(); ++index) {
            items->item(index)->setEnabled(false);
            items->item(index)->setToolTip(tr("The engine does not draw this blend yet"));
        }
    blend_->setToolTip(tr("How the layer mixes with what is under it (only normal is drawn yet)"));
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
    const auto *shape = row && !row->shape.empty() ? findNode(nodes, row->shape) : nullptr;
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
        transform_fields_->setVisible(transform_open_);
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

} // namespace PelicanStudio
