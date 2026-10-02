#include "graphcanvasvaluefields.hpp"

#include "studiotheme.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

QLabel *caption(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setFont(studioSmallFont());
    return label;
}

QDoubleSpinBox *numberSpin(QWidget *parent, const QString &name, double low, double high) {
    auto *spin = new QDoubleSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(low, high);
    spin->setDecimals(3);
    spin->setSingleStep(0.1);
    spin->setKeyboardTracking(false);
    return spin;
}

// The value a curve has at `time`, drawn as the engine samples it: linear,
// a step's own value, a cubic segment through its tangents.
double drawnValue(const std::vector<GraphCanvasCurveKey> &keys, double time) {
    if (keys.empty()) return 0.0;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (std::size_t index = 1; index < keys.size(); ++index) {
        const auto &a = keys[index - 1];
        const auto &b = keys[index];
        if (time > b.time) continue;
        const double width = b.time - a.time;
        const double t = width > 0.0 ? (time - a.time) / width : 0.0;
        if (a.interpolation == "step") return a.value;
        if (a.interpolation == "cubic") {
            const double out = a.rest.is_object() ? a.rest.value("out_tangent", 0.0) : 0.0;
            const double in = b.rest.is_object() ? b.rest.value("in_tangent", 0.0) : 0.0;
            const double t2 = t * t;
            const double t3 = t2 * t;
            return (2 * t3 - 3 * t2 + 1) * a.value + (t3 - 2 * t2 + t) * width * out +
                   (-2 * t3 + 3 * t2) * b.value + (t3 - t2) * width * in;
        }
        return a.value + (b.value - a.value) * t;
    }
    return keys.back().value;
}
} // namespace

// --- The small curve.

GraphCanvasCurveGraph::GraphCanvasCurveGraph(Commit commit, QWidget *parent)
    : QWidget{parent}, commit_{std::move(commit)} {
    setObjectName(QStringLiteral("pelican.graphCanvasCurve.graph"));
    setMinimumHeight(96);
    setMouseTracking(false);
    setToolTip(tr("Drag a key to move it: Shift moves its time only, Ctrl its value only."));
}

QSize GraphCanvasCurveGraph::sizeHint() const { return QSize{240, 110}; }

void GraphCanvasCurveGraph::setKeys(std::vector<GraphCanvasCurveKey> keys, bool editable) {
    editable_ = editable;
    if (dragging_) return; // The hand holding a key is not overridden.
    keys_ = std::move(keys);
    update();
}

QRectF GraphCanvasCurveGraph::plot() const {
    return QRectF{8.0, 8.0, std::max(1.0, width() - 16.0), std::max(1.0, height() - 16.0)};
}

GraphCanvasCurveGraph::Frame
GraphCanvasCurveGraph::frameOf(const std::vector<GraphCanvasCurveKey> &keys) const {
    Frame frame;
    if (keys.empty()) return frame;
    frame.t0 = keys.front().time;
    frame.t1 = keys.back().time;
    frame.v0 = frame.v1 = keys.front().value;
    for (const auto &key : keys) {
        frame.v0 = std::min(frame.v0, key.value);
        frame.v1 = std::max(frame.v1, key.value);
    }
    if (!(frame.t1 > frame.t0)) {
        frame.t0 -= 0.5;
        frame.t1 += 0.5;
    }
    if (!(frame.v1 > frame.v0)) {
        frame.v0 -= 0.5;
        frame.v1 += 0.5;
    }
    // A margin, so a key at an end can still be grabbed and moved past it.
    const double pad_t = (frame.t1 - frame.t0) * 0.08;
    const double pad_v = (frame.v1 - frame.v0) * 0.12;
    frame.t0 -= pad_t;
    frame.t1 += pad_t;
    frame.v0 -= pad_v;
    frame.v1 += pad_v;
    return frame;
}

QPointF GraphCanvasCurveGraph::pointOf(const Frame &frame, double time, double value) const {
    const auto box = plot();
    return QPointF{box.left() + box.width() * (time - frame.t0) / (frame.t1 - frame.t0),
                   box.bottom() - box.height() * (value - frame.v0) / (frame.v1 - frame.v0)};
}

QPointF GraphCanvasCurveGraph::keyPoint(std::size_t index) const {
    if (index >= keys_.size()) return {};
    const auto frame = dragging_ ? pressed_frame_ : frameOf(keys_);
    return pointOf(frame, keys_[index].time, keys_[index].value);
}

void GraphCanvasCurveGraph::paintEvent(QPaintEvent *) {
    const StudioTheme &theme = currentStudioTheme();
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), theme.canvas.node_body);
    painter.setPen(QPen{theme.canvas.node_border, 1.0});
    painter.drawRect(rect().adjusted(0, 0, -1, -1));
    if (keys_.empty()) {
        painter.setPen(QPen{theme.canvas.dim_text});
        painter.drawText(rect(), Qt::AlignCenter, tr("No keys"));
        return;
    }
    const auto frame = dragging_ ? pressed_frame_ : frameOf(keys_);
    const auto box = plot();
    // The zero line, when it is inside.
    if (frame.v0 < 0.0 && frame.v1 > 0.0) {
        painter.setPen(QPen{theme.canvas.node_border, 1.0, Qt::DotLine});
        const auto y = pointOf(frame, frame.t0, 0.0).y();
        painter.drawLine(QPointF{box.left(), y}, QPointF{box.right(), y});
    }
    QPainterPath path;
    constexpr int steps = 96;
    for (int step = 0; step <= steps; ++step) {
        const double time = frame.t0 + (frame.t1 - frame.t0) * step / steps;
        const auto at = pointOf(frame, time, drawnValue(keys_, time));
        if (step == 0) path.moveTo(at);
        else path.lineTo(at);
    }
    painter.setPen(QPen{theme.domainTint(QStringLiteral("values")), 1.8});
    painter.drawPath(path);
    for (std::size_t index = 0; index < keys_.size(); ++index) {
        const auto at = pointOf(frame, keys_[index].time, keys_[index].value);
        painter.setPen(QPen{theme.canvas.node_border, 1.0});
        painter.setBrush(dragging_ == index ? theme.domainTint(QStringLiteral("rendering"))
                                            : theme.domainTint(QStringLiteral("values")));
        painter.drawEllipse(at, 4.5, 4.5);
    }
}

void GraphCanvasCurveGraph::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton || !editable_ || keys_.empty()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const auto frame = frameOf(keys_);
    std::optional<std::size_t> nearest;
    double best = 10.0;
    for (std::size_t index = 0; index < keys_.size(); ++index) {
        const auto at = pointOf(frame, keys_[index].time, keys_[index].value);
        const double distance = std::hypot(at.x() - event->position().x(),
                                           at.y() - event->position().y());
        if (distance <= best) {
            best = distance;
            nearest = index;
        }
    }
    if (!nearest) return;
    dragging_ = nearest;
    pressed_keys_ = keys_;
    pressed_frame_ = frame;
    event->accept();
    update();
}

void GraphCanvasCurveGraph::mouseMoveEvent(QMouseEvent *event) {
    if (!dragging_) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const auto box = plot();
    const auto &frame = pressed_frame_;
    double time = frame.t0 + (frame.t1 - frame.t0) * (event->position().x() - box.left()) / box.width();
    double value = frame.v0 + (frame.v1 - frame.v0) * (box.bottom() - event->position().y()) / box.height();
    const auto &held = pressed_keys_[*dragging_];
    if (event->modifiers().testFlag(Qt::ShiftModifier)) value = held.value;
    if (event->modifiers().testFlag(Qt::ControlModifier)) time = held.time;
    keys_ = GraphCanvasModel::curveKeysWithMoved(pressed_keys_, *dragging_, time, value);
    event->accept();
    update();
}

void GraphCanvasCurveGraph::mouseReleaseEvent(QMouseEvent *event) {
    if (!dragging_) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    dragging_.reset();
    event->accept();
    update();
    // One drag is one edit; a press that moved nothing is none.
    if (keys_ != pressed_keys_ && commit_) commit_(keys_);
}

// --- The fields.

GraphCanvasValueFields::GraphCanvasValueFields(GraphCanvasModel &model,
                                               std::function<void()> submitted, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasValueFields"));
    held_ = std::make_unique<GraphCanvasHeldEdits>(model_, this);
    held_->setSent([this] {
        if (submitted_) submitted_();
    });
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);

    // Decision 4: Keys.
    curve_ = new QWidget(this);
    curve_->setObjectName(QStringLiteral("pelican.graphCanvasCurve"));
    auto *curve_layout = new QVBoxLayout(curve_);
    curve_layout->setContentsMargins(0, 0, 0, 0);
    auto *keys_caption = caption(tr("Keys"), curve_);
    keys_caption->setStyleSheet(QStringLiteral("font-weight: bold;"));
    curve_layout->addWidget(keys_caption);
    graph_ = new GraphCanvasCurveGraph(
        [this](const std::vector<GraphCanvasCurveKey> &keys) { commitKeys(keys); }, curve_);
    curve_layout->addWidget(graph_);
    keys_ = new QTableWidget(curve_);
    keys_->setObjectName(QStringLiteral("pelican.graphCanvasCurve.keys"));
    keys_->setColumnCount(4);
    keys_->setHorizontalHeaderLabels({tr("Time"), tr("Value"), tr("Interpolation"), QString{}});
    keys_->verticalHeader()->setVisible(false);
    keys_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    keys_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    keys_->setSelectionMode(QAbstractItemView::SingleSelection);
    keys_->setSelectionBehavior(QAbstractItemView::SelectRows);
    keys_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    curve_layout->addWidget(keys_);
    add_key_ = new QPushButton(tr("+ Key"), curve_);
    add_key_->setObjectName(QStringLiteral("pelican.graphCanvasCurve.add"));
    add_key_->setToolTip(tr("A key after the picked row (the last when none is): halfway to "
                            "the next, or a second past the last."));
    curve_layout->addWidget(add_key_, 0, Qt::AlignLeft);
    connect(add_key_, &QPushButton::clicked, this, [this] {
        const int row = keys_->currentRow();
        const auto after = row >= 0 ? static_cast<std::size_t>(row)
                                    : (shown_keys_.empty() ? 0 : shown_keys_.size() - 1);
        commitKeys(GraphCanvasModel::curveKeysWithAdded(shown_keys_, after));
    });
    layout->addWidget(curve_);
    curve_->hide();

    // Decision 5: a playback's time map.
    playback_ = new QWidget(this);
    playback_->setObjectName(QStringLiteral("pelican.graphCanvasPlayback"));
    auto *playback_form = new QFormLayout(playback_);
    playback_form->setContentsMargins(0, 0, 0, 0);
    wrap_ = new QComboBox(playback_);
    wrap_->setObjectName(QStringLiteral("pelican.graphCanvasPlayback.wrap"));
    for (const auto &wrap : GraphCanvasModel::playbackWraps()) wrap_->addItem(qtext(wrap), qtext(wrap));
    wrap_->setToolTip(tr("Past the end: stop there (once), start again (repeat), or run back "
                         "(ping_pong)."));
    playback_form->addRow(caption(tr("Wrap"), playback_), wrap_);
    connect(wrap_, &QComboBox::activated, this, [this](int index) {
        if (loading_ || index < 0) return;
        afterCommit(send([&model = model_, scope = scope_, node = node_,
                          wrap = wrap_->itemData(index).toString().toStdString()] {
            return model.setPlaybackField(scope, node, "wrap", wrap);
        }));
    });
    const std::map<std::string, QString> labels{{"duration", tr("Duration")},
                                                {"source_begin", tr("Source begin")},
                                                {"source_end", tr("Source end")},
                                                {"speed", tr("Speed")},
                                                {"delay", tr("Delay")}};
    for (const auto &[key, fallback] : GraphCanvasModel::playbackNumbers()) {
        (void)fallback;
        auto *spin = numberSpin(playback_,
                                QStringLiteral("pelican.graphCanvasPlayback.") + qtext(key),
                                key == "duration" ? 0.001 : -1.0e6, 1.0e6);
        playback_form->addRow(caption(labels.at(key), playback_), spin);
        playback_numbers_.emplace_back(key, spin);
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this, key, spin] {
            if (loading_) return;
            if (std::abs(spin->value() - model_.playbackNumber(scope_, node_, key)) < 1e-9) return;
            afterCommit(send([&model = model_, scope = scope_, node = node_, key,
                              value = spin->value()] {
                return model.setPlaybackField(scope, node, key, value);
            }));
        });
    }
    layout->addWidget(playback_);
    playback_->hide();

    // Decision 12 (b): render.scalar.
    scalar_ = new QWidget(this);
    scalar_->setObjectName(QStringLiteral("pelican.graphCanvasScalar"));
    auto *scalar_form = new QFormLayout(scalar_);
    scalar_form->setContentsMargins(0, 0, 0, 0);
    scalar_target_ = new QComboBox(scalar_);
    scalar_target_->setObjectName(QStringLiteral("pelican.graphCanvasScalar.target"));
    scalar_target_->setToolTip(tr("The buffer the value is written into: the graph's, or the "
                                  "frame plan's (a feature's, as bloom.intensity)."));
    scalar_form->addRow(caption(tr("Buffer"), scalar_), scalar_target_);
    connect(scalar_target_, &QComboBox::activated, this, [this](int index) {
        if (loading_ || index < 0) return;
        afterCommit(send([&model = model_, scope = scope_, node = node_,
                          target = scalar_target_->itemData(index).toString().toStdString()] {
            return model.setScalarTarget(scope, node, target);
        }));
    });
    scalar_components_ = new QComboBox(scalar_);
    scalar_components_->setObjectName(QStringLiteral("pelican.graphCanvasScalar.components"));
    scalar_components_->addItem(tr("1 (on the value line)"), 1);
    scalar_components_->addItem(QStringLiteral("vec2"), 2);
    scalar_components_->addItem(QStringLiteral("vec3"), 3);
    scalar_components_->addItem(QStringLiteral("vec4"), 4);
    scalar_components_->setToolTip(tr("One number on the value line, or two to four written "
                                      "here."));
    scalar_form->addRow(caption(tr("Value"), scalar_), scalar_components_);
    connect(scalar_components_, &QComboBox::activated, this, [this](int) {
        if (!loading_) commitScalar();
    });
    auto *values_row = new QWidget(scalar_);
    auto *values_layout = new QHBoxLayout(values_row);
    values_layout->setContentsMargins(0, 0, 0, 0);
    for (int component = 0; component < 4; ++component) {
        auto *spin = numberSpin(values_row,
                                QStringLiteral("pelican.graphCanvasScalar.value%1").arg(component),
                                -1.0e9, 1.0e9);
        values_layout->addWidget(spin, 1);
        scalar_values_.push_back(spin);
        connect(spin, &QDoubleSpinBox::editingFinished, this, [this] {
            if (!loading_) commitScalar();
        });
    }
    scalar_form->addRow(QString{}, values_row);
    scalar_offset_ = caption({}, scalar_);
    scalar_offset_->setObjectName(QStringLiteral("pelican.graphCanvasScalar.offset"));
    scalar_offset_->setToolTip(tr("Where in the buffer it lands. Studio packs the sinks on one "
                                  "buffer from byte 0, without a gap."));
    scalar_form->addRow(caption(tr("Offset"), scalar_), scalar_offset_);
    layout->addWidget(scalar_);
    scalar_->hide();

    // Decision 2: Buffers.
    buffers_ = new QWidget(this);
    buffers_->setObjectName(QStringLiteral("pelican.graphCanvasBuffers"));
    auto *buffers_layout = new QVBoxLayout(buffers_);
    buffers_layout->setContentsMargins(0, 0, 0, 0);
    auto *buffers_caption = caption(tr("Buffers"), buffers_);
    buffers_caption->setStyleSheet(QStringLiteral("font-weight: bold;"));
    buffers_layout->addWidget(buffers_caption);
    buffer_list_ = caption({}, buffers_);
    buffer_list_->setObjectName(QStringLiteral("pelican.graphCanvasBuffers.list"));
    buffer_list_->setWordWrap(true);
    buffer_list_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    buffers_layout->addWidget(buffer_list_);
    buffer_add_ = new QPushButton(tr("+ Buffer port"), buffers_);
    buffer_add_->setObjectName(QStringLiteral("pelican.graphCanvasBuffers.add"));
    buffer_add_->setToolTip(tr("A buffer the shader reads through pelican_resource_ports.glsl, "
                               "which its reflection does not show until the port is there."));
    buffers_layout->addWidget(buffer_add_, 0, Qt::AlignLeft);
    buffer_row_ = new QWidget(buffers_);
    auto *buffer_row_layout = new QHBoxLayout(buffer_row_);
    buffer_row_layout->setContentsMargins(0, 0, 0, 0);
    buffer_name_ = new QComboBox(buffer_row_);
    buffer_name_->setObjectName(QStringLiteral("pelican.graphCanvasBuffers.name"));
    buffer_name_->setEditable(true);
    buffer_name_->setInsertPolicy(QComboBox::NoInsert);
    buffer_name_->setToolTip(tr("The port's name: what the shader reads it as "
                                "(pelican_load_<name>)."));
    buffer_row_layout->addWidget(buffer_name_, 1);
    buffer_row_layout->addWidget(caption(QStringLiteral("←"), buffer_row_));
    buffer_source_ = new QComboBox(buffer_row_);
    buffer_source_->setObjectName(QStringLiteral("pelican.graphCanvasBuffers.source"));
    buffer_source_->setToolTip(tr("The buffer it reads: a render.buffer or render.attribute"));
    buffer_row_layout->addWidget(buffer_source_, 1);
    buffer_apply_ = new QPushButton(tr("Add"), buffer_row_);
    buffer_apply_->setObjectName(QStringLiteral("pelican.graphCanvasBuffers.apply"));
    buffer_row_layout->addWidget(buffer_apply_);
    buffers_layout->addWidget(buffer_row_);
    buffer_row_->hide();
    connect(buffer_add_, &QPushButton::clicked, this, [this] {
        buffer_row_->setVisible(!buffer_row_->isVisible());
        loadBuffers();
    });
    connect(buffer_apply_, &QPushButton::clicked, this, [this] { addBufferPort(); });
    connect(buffer_name_->lineEdit(), &QLineEdit::returnPressed, this, [this] { addBufferPort(); });
    layout->addWidget(buffers_);
    buffers_->hide();

    note_ = caption({}, this);
    note_->setObjectName(QStringLiteral("pelican.graphCanvasValueFields.note"));
    note_->setWordWrap(true);
    note_->hide();
    layout->addWidget(note_);
    hide();
}

bool GraphCanvasValueFields::send(std::function<bool()> edit) {
    if (node_.empty()) return false;
    return held_->sendOrHold(std::move(edit));
}

void GraphCanvasValueFields::release() {
    if (node_.empty()) return;
    QWidget *focus = QApplication::focusWidget();
    if (focus == nullptr || !isAncestorOf(focus)) return;
    // Each of these spins sends its value when it is finished with.
    auto *spin = qobject_cast<QDoubleSpinBox *>(focus);
    if (spin == nullptr) return;
    spin->interpretText();
    Q_EMIT spin->editingFinished();
}

void GraphCanvasValueFields::afterCommit(bool sent) {
    if (sent) {
        note_->hide();
        if (submitted_) submitted_();
        return;
    }
    // Refused here (the model's notice says why) or nothing changed: the
    // fields say what the document says.
    const auto &notice = model_.editNotice();
    if (notice.kind == InspectorNoticeKind::Error && !notice.message.empty()) {
        note_->setText(qtext(notice.message));
        note_->show();
    }
    if (op_ == "value.curve") loadCurve(true);
    if (GraphCanvasModel::isPlayback(op_)) loadPlayback();
    if (op_ == "render.scalar") loadScalar();
    if (submitted_) submitted_();
}

void GraphCanvasValueFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    // UX-9, decision 2: what is typed for the node being let go of goes to it.
    if (node == nullptr || node->id != node_ || scope != scope_) release();
    const bool shows = node != nullptr &&
                       (node->op == "value.curve" || GraphCanvasModel::isPlayback(node->op) ||
                        node->op == "render.scalar" || node->op == "render.fullscreen" ||
                        node->op == "render.raster");
    if (!shows) {
        node_.clear();
        scope_.clear();
        op_.clear();
        hide();
        return;
    }
    const bool same = node_ == node->id && scope_ == scope;
    if (!same) note_->hide();
    node_ = node->id;
    scope_ = scope;
    op_ = node->op;
    curve_->setVisible(op_ == "value.curve");
    playback_->setVisible(GraphCanvasModel::isPlayback(op_));
    scalar_->setVisible(op_ == "render.scalar");
    buffers_->setVisible(op_ == "render.fullscreen" || op_ == "render.raster");
    if (!same) buffer_row_->hide();
    if (op_ == "value.curve") loadCurve(!same);
    // Another node's numbers replace what a focused field held: what was
    // typed there went to the node it was typed for (release()).
    if (GraphCanvasModel::isPlayback(op_)) loadPlayback(!same);
    if (op_ == "render.scalar") loadScalar(!same);
    if (buffers_->isVisible()) loadBuffers();
    show();
}

void GraphCanvasValueFields::followEditable() {
    const bool editable = model_.canEdit();
    if (curve_->isVisible()) {
        graph_->setKeys(graph_->keys(), editable);
        add_key_->setEnabled(editable);
        for (int row = 0; row < keys_->rowCount(); ++row)
            for (int column = 0; column < keys_->columnCount(); ++column)
                if (auto *cell = keys_->cellWidget(row, column)) cell->setEnabled(editable);
    }
    playback_->setEnabled(editable);
    scalar_->setEnabled(editable);
    held_->follow();
    buffer_add_->setEnabled(editable);
    buffer_apply_->setEnabled(editable && buffer_source_->count() > 0 &&
                              !buffer_name_->currentText().trimmed().isEmpty());
}

void GraphCanvasValueFields::loadCurve(bool force) {
    const auto keys = model_.curveKeys(scope_, node_);
    const bool editable = model_.canEdit();
    graph_->setKeys(keys, editable);
    // A row being typed into keeps what it holds until it is finished.
    if (!force && keys == shown_keys_) return;
    if (!force)
        for (int row = 0; row < keys_->rowCount(); ++row)
            for (int column = 0; column < 2; ++column)
                if (auto *cell = keys_->cellWidget(row, column); cell != nullptr && cell->hasFocus())
                    return;
    shown_keys_ = keys;
    const QSignalBlocker block{keys_};
    const int current = keys_->currentRow();
    keys_->setRowCount(static_cast<int>(keys.size()));
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const int row = static_cast<int>(index);
        const auto name = [&](const char *field) {
            return QStringLiteral("pelican.graphCanvasCurve.key%1.%2").arg(row).arg(QLatin1String{field});
        };
        auto *time = numberSpin(keys_, name("time"), -1.0e6, 1.0e6);
        auto *value = numberSpin(keys_, name("value"), -1.0e9, 1.0e9);
        {
            const QSignalBlocker block_time{time};
            const QSignalBlocker block_value{value};
            time->setValue(keys[index].time);
            value->setValue(keys[index].value);
        }
        auto *interpolation = new QComboBox(keys_);
        interpolation->setObjectName(name("interpolation"));
        for (const auto &kind : GraphCanvasModel::curveInterpolations())
            interpolation->addItem(qtext(kind), qtext(kind));
        interpolation->setCurrentIndex(
            std::max(0, interpolation->findData(qtext(keys[index].interpolation))));
        interpolation->setToolTip(tr("How the curve runs from this key to the next"));
        auto *remove = new QToolButton(keys_);
        remove->setObjectName(name("remove"));
        remove->setText(QStringLiteral("×"));
        remove->setToolTip(tr("Take this key out"));
        remove->setEnabled(editable && keys.size() > 1);
        for (QWidget *cell : std::initializer_list<QWidget *>{time, value, interpolation})
            cell->setEnabled(editable);
        keys_->setCellWidget(row, 0, time);
        keys_->setCellWidget(row, 1, value);
        keys_->setCellWidget(row, 2, interpolation);
        keys_->setCellWidget(row, 3, remove);
        connect(time, &QDoubleSpinBox::editingFinished, this, [this, row] { commitRow(row); });
        connect(value, &QDoubleSpinBox::editingFinished, this, [this, row] { commitRow(row); });
        connect(interpolation, &QComboBox::activated, this, [this, row](int) { commitRow(row); });
        connect(remove, &QToolButton::clicked, this, [this, index] {
            if (index >= shown_keys_.size() || shown_keys_.size() <= 1) return;
            auto keys = shown_keys_;
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(index));
            commitKeys(keys);
        });
    }
    if (current >= 0 && current < keys_->rowCount()) keys_->setCurrentCell(current, 0);
    keys_->setFixedHeight(keys_->horizontalHeader()->height() +
                          std::max(1, keys_->rowCount()) * keys_->verticalHeader()->defaultSectionSize() +
                          4);
    add_key_->setEnabled(editable);
}

void GraphCanvasValueFields::commitRow(int row) {
    if (loading_ || row < 0 || static_cast<std::size_t>(row) >= shown_keys_.size()) return;
    auto *time = qobject_cast<QDoubleSpinBox *>(keys_->cellWidget(row, 0));
    auto *value = qobject_cast<QDoubleSpinBox *>(keys_->cellWidget(row, 1));
    auto *interpolation = qobject_cast<QComboBox *>(keys_->cellWidget(row, 2));
    if (time == nullptr || value == nullptr || interpolation == nullptr) return;
    auto keys = shown_keys_;
    auto &key = keys[static_cast<std::size_t>(row)];
    key.time = time->value();
    key.value = value->value();
    key.interpolation = interpolation->currentData().toString().toStdString();
    if (keys == shown_keys_) return;
    commitKeys(keys);
}

void GraphCanvasValueFields::commitKeys(const std::vector<GraphCanvasCurveKey> &keys) {
    if (node_.empty()) return;
    const bool sent = send([&model = model_, scope = scope_, node = node_, keys] {
        return model.setCurveKeys(scope, node, keys);
    });
    // What is shown now is what was sent, until the document says otherwise.
    if (sent) {
        shown_keys_ = keys;
        graph_->setKeys(keys, model_.canEdit());
    }
    afterCommit(sent);
}

void GraphCanvasValueFields::loadPlayback(bool force) {
    loading_ = true;
    const auto parameters = model_.nodeParameters(scope_, node_);
    const auto wrap = parameters.is_object() && parameters.contains("wrap") &&
                              parameters.at("wrap").is_string()
                          ? parameters.at("wrap").get<std::string>()
                          : GraphCanvasModel::playbackWraps().front();
    {
        const QSignalBlocker block{wrap_};
        wrap_->setCurrentIndex(std::max(0, wrap_->findData(qtext(wrap))));
    }
    for (const auto &[key, spin] : playback_numbers_) {
        if (!force && spin->hasFocus()) continue;
        const QSignalBlocker block{spin};
        spin->setValue(model_.playbackNumber(scope_, node_, key));
        const bool driven = model_.playbackDriven(scope_, node_, key);
        spin->setEnabled(model_.canEdit() && !driven);
        spin->setToolTip(driven ? tr("An edge drives it.") : QString{});
    }
    loading_ = false;
}

void GraphCanvasValueFields::loadScalar(bool force) {
    loading_ = true;
    const auto parameters = model_.nodeParameters(scope_, node_);
    const auto target = parameters.is_object() && parameters.contains("target") &&
                                parameters.at("target").is_string()
                            ? parameters.at("target").get<std::string>()
                            : std::string{};
    {
        const QSignalBlocker block{scalar_target_};
        scalar_target_->clear();
        // UX-13b, decision 2 (engine CT-5): the buffers it can write once the
        // edit that picks one makes it so, each saying what that changes.
        for (const auto &choice : model_.scalarTargetChoices(scope_, node_, model_.scalarValue(scope_, node_).size()))
            scalar_target_->addItem(qtext(GraphCanvasModel::scalarTargetLabel(choice)), qtext(choice.buffer));
        if (!target.empty() && scalar_target_->findData(qtext(target)) < 0)
            scalar_target_->addItem(qtext(target), qtext(target));
        scalar_target_->setCurrentIndex(std::max(0, scalar_target_->findData(qtext(target))));
    }
    const auto value = model_.scalarValue(scope_, node_);
    const bool written = parameters.is_object() && parameters.contains("value");
    const int components = written ? static_cast<int>(value.size()) : 1;
    {
        const QSignalBlocker block{scalar_components_};
        scalar_components_->setCurrentIndex(std::max(0, scalar_components_->findData(components)));
    }
    for (int index = 0; index < 4; ++index) {
        auto *spin = scalar_values_[static_cast<std::size_t>(index)];
        const QSignalBlocker block{spin};
        spin->setVisible(written && index < components);
        if (index < static_cast<int>(value.size()) && (force || !spin->hasFocus()))
            spin->setValue(value[static_cast<std::size_t>(index)]);
    }
    std::int64_t offset = 0;
    if (parameters.is_object() && parameters.contains("offset") &&
        parameters.at("offset").is_number_integer())
        offset = parameters.at("offset").get<std::int64_t>();
    scalar_offset_->setText(tr("%1 bytes").arg(offset));
    loading_ = false;
}

void GraphCanvasValueFields::commitScalar() {
    const int components = scalar_components_->currentData().toInt();
    std::vector<double> value;
    if (components <= 1) {
        value = {0.0};
    } else {
        const auto now = model_.scalarValue(scope_, node_);
        const auto parameters = model_.nodeParameters(scope_, node_);
        const bool written = parameters.is_object() && parameters.contains("value");
        for (int index = 0; index < components; ++index) {
            auto *spin = scalar_values_[static_cast<std::size_t>(index)];
            // A component the field did not show yet starts at 0.
            const bool shown = written && index < static_cast<int>(now.size());
            value.push_back(shown || spin->isVisible() ? spin->value() : 0.0);
        }
    }
    afterCommit(send([&model = model_, scope = scope_, node = node_, value] {
        return model.setScalarValue(scope, node, value);
    }));
    loadScalar();
}

void GraphCanvasValueFields::loadBuffers() {
    QStringList lines;
    const auto *declared = model_.passInputs(scope_, node_);
    const auto drawn = model_.nodes(scope_);
    const auto found = std::ranges::find(drawn, node_, &GraphCanvasNode::id);
    std::set<std::string> shown;
    if (found != drawn.end())
        for (const auto &socket : found->input_sockets) {
            if (!socket.buffer) continue;
            if (!socket.port.empty()) shown.insert(socket.port);
            lines << (socket.connected()
                          ? tr("binding %1 %2 ← %3.%4 (port %5)")
                                .arg(socket.binding)
                                .arg(qtext(socket.declaration), qtext(socket.source_node),
                                     qtext(socket.source_port), qtext(socket.port))
                          : tr("binding %1 %2: drag a render.buffer's resource onto its socket")
                                .arg(socket.binding)
                                .arg(qtext(socket.declaration)));
        }
    // UX-13a, decision 1: a port the reflection does not show yet is bound
    // where the engine binds a buffer read -- after the images, in the order
    // of the buffers the reads name (one binding per buffer).
    const auto reads = model_.bufferPortReads(scope_, node_);
    std::vector<std::string> resources;
    for (const auto &read : reads)
        if (!read.resource.empty() && std::ranges::find(resources, read.resource) == resources.end())
            resources.push_back(read.resource);
    const auto ports = model_.bufferPorts(scope_, node_);
    for (const auto &read : reads) {
        const auto &port = read.port;
        if (shown.contains(port)) continue;
        std::string source;
        if (found != drawn.end())
            for (const auto &input : found->inputs)
                if (input.port == port && input.connected)
                    source = input.source_node + "." + input.source_port;
        const auto place = std::ranges::find(resources, read.resource);
        if (source.empty())
            lines << tr("%1 (no line)").arg(qtext(port));
        else if (declared != nullptr && declared->declared() && place != resources.end())
            lines << tr("binding %1 pelican_resource_%2 ← %3")
                         .arg(static_cast<qulonglong>(declared->buffer_binding_base +
                                                      static_cast<std::size_t>(place - resources.begin())))
                         .arg(qtext(port), qtext(source));
        else
            lines << tr("%1 ← %2").arg(qtext(port), qtext(source));
    }
    if (lines.isEmpty())
        lines << (declared == nullptr
                      ? tr("The shader's buffer bindings show once the pass is built.")
                      : tr("No buffer: the shader declares none, and the pass has no buffer port."));
    buffer_list_->setText(lines.join(QLatin1Char{'\n'}));
    if (!buffer_row_->isVisible()) return;
    // The names the shader reads and the buffers to read from.
    const QSignalBlocker block_name{buffer_name_};
    const auto kept = buffer_name_->currentText();
    buffer_name_->clear();
    const auto parameters = model_.nodeParameters(scope_, node_);
    std::vector<std::string> names;
    if (parameters.is_object() && parameters.contains("shader") && parameters.at("shader").is_object())
        for (const auto &[stage, reference] : parameters.at("shader").items()) {
            (void)stage;
            if (!reference.is_string()) continue;
            for (auto &name : shaderResourcePortNames(project_root_, reference.get<std::string>()))
                if (std::ranges::find(names, name) == names.end() &&
                    std::ranges::find(ports, name) == ports.end())
                    names.push_back(std::move(name));
        }
    for (const auto &name : names) buffer_name_->addItem(qtext(name));
    if (!kept.isEmpty() && buffer_name_->findText(kept) < 0) buffer_name_->setEditText(kept);
    else if (buffer_name_->count() == 0) buffer_name_->setEditText(QStringLiteral("value"));
    const QSignalBlocker block_source{buffer_source_};
    const auto kept_source = buffer_source_->currentData().toString();
    buffer_source_->clear();
    for (const auto &source : model_.bufferSources(scope_))
        buffer_source_->addItem(qtext(source), qtext(source));
    buffer_source_->setCurrentIndex(std::max(0, buffer_source_->findData(kept_source)));
    followEditable();
}

void GraphCanvasValueFields::addBufferPort() {
    const auto name = GraphCanvasModel::bufferPortName(buffer_name_->currentText().trimmed().toStdString());
    const auto source = buffer_source_->currentData().toString().toStdString();
    if (source.empty()) return;
    const bool sent = send([&model = model_, scope = scope_, node = node_, name, source] {
        return model.addBufferPort(scope, node, name, source, "resource");
    });
    if (sent) buffer_row_->hide();
    afterCommit(sent);
}

std::vector<std::string> shaderResourcePortNames(const std::filesystem::path &project_root,
                                                 const std::string &reference) {
    std::vector<std::string> names;
    constexpr std::string_view prefix = "project://";
    if (project_root.empty() || !reference.starts_with(prefix)) return names;
    const std::filesystem::path relative{reference.substr(prefix.size())};
    std::vector<std::filesystem::path> candidates{project_root / relative};
    for (const char *extension : {".frag", ".vert", ".glsl"})
        candidates.push_back(project_root / std::filesystem::path{relative.string() + extension});
    for (const auto &candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error)) continue;
        std::ifstream stream{candidate, std::ios::binary};
        std::stringstream text;
        text << stream.rdbuf();
        const auto source = QString::fromStdString(text.str());
        // UX-13a: the variable itself too (pelican_resource_<port>), but not
        // the include's own name nor pelican_sample_table, an engine function.
        static const QRegularExpression pattern{QStringLiteral(
            "(?:PELICAN_RESOURCE_PORT_|pelican_load_|pelican_sample_(?!table\\b)|"
            "pelican_resource_(?!ports\\b))([A-Za-z_][A-Za-z0-9_]*)")};
        for (auto match = pattern.globalMatch(source); match.hasNext();) {
            const auto name = match.next().captured(1).toStdString();
            if (std::ranges::find(names, name) == names.end()) names.push_back(name);
        }
    }
    return names;
}

} // namespace PelicanStudio
