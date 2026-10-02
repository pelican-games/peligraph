#include "graphcanvasrenderfields.hpp"

#include "fullscreenpasssettings.hpp"
#include "fullscreenshadertemplate.hpp"
#include "passforms/attachmentopsform.hpp"
#include "passforms/rasterstateform.hpp"
#include "passforms/shaderdefinesform.hpp"
#include "studiotheme.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QActionGroup>
#include <QApplication>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QMenu>
#include <QRadioButton>
#include <QWidgetAction>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <limits>
#include <tuple>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString qtext(const std::string &value) { return QString::fromStdString(value); }

QString imageFieldName(const QString &field) {
    return QStringLiteral("pelican.graphCanvasImage.") + field;
}

QString passFieldName(const QString &field) {
    return QStringLiteral("pelican.graphCanvasPass.") + field;
}

QString placeFieldName(const QString &field) {
    return QStringLiteral("pelican.graphCanvasPlace.") + field;
}

QString socketFieldName(const QString &field) {
    return QStringLiteral("pelican.graphCanvasSocket.") + field;
}

std::string stringOf(const Json &object, const char *key) {
    if (!object.is_object()) return {};
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

QLabel *caption(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setFont(studioSmallFont());
    return label;
}

// The combo shows `current` even when its list does not have it: what the
// document holds is never hidden behind a value it does not hold.
void selectOrAdd(QComboBox &combo, const QString &current, const QString &label = {}) {
    int index = combo.findData(current);
    if (index < 0 && !current.isEmpty()) {
        combo.addItem(label.isEmpty() ? current : label, current);
        index = combo.count() - 1;
    }
    combo.setCurrentIndex(index);
}

// A field a load replaces. It may be the one whose signal is being handled,
// so it goes with the event loop; until then it is out of sight and answers
// to no name, so nothing finds it in place of the new one.
void retire(QWidget *widget) {
    widget->hide();
    widget->setObjectName(QString{});
    widget->deleteLater();
}

} // namespace

// --- Decision 1: render.image.

GraphCanvasImageFields::GraphCanvasImageFields(GraphCanvasModel &model,
                                               std::function<void()> submitted, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasImage"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    heading_ = new QLabel(this);
    heading_->setObjectName(imageFieldName(QStringLiteral("heading")));
    heading_->setFont(studioSmallFont());
    heading_->setWordWrap(true);
    layout->addWidget(heading_);

    fields_ = new QWidget(this);
    auto *form = new QFormLayout(fields_);
    form->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(fields_);

    format_ = new QComboBox(fields_);
    format_->setObjectName(imageFieldName(QStringLiteral("format")));
    format_->setInsertPolicy(QComboBox::NoInsert);
    format_->setToolTip(tr("The image's format: the ones the engine's render targets accept."));
    form->addRow(caption(tr("Format"), fields_), format_);
    // A pick is an index change (a person's and a scenario's select alike);
    // a load changes the index with the signal blocked.
    connect(format_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) return;
        const auto value = format_->itemData(index).toString();
        commitFormat(value.isEmpty() ? format_->itemText(index) : value);
    });

    semantic_ = new QComboBox(fields_);
    semantic_->setObjectName(imageFieldName(QStringLiteral("semantic")));
    semantic_->setToolTip(tr("What the image holds: the semantic type its readers check."));
    form->addRow(caption(tr("Type"), fields_), semantic_);
    connect(semantic_, &QComboBox::currentIndexChanged, this, [this] { commitSemantic(); });

    arguments_ = new QWidget(fields_);
    arguments_form_ = new QFormLayout(arguments_);
    arguments_form_->setContentsMargins(12, 0, 0, 0);
    form->addRow(arguments_);

    usage_ = new QWidget(fields_);
    usage_->setObjectName(imageFieldName(QStringLiteral("usage")));
    auto *usage_layout = new QGridLayout(usage_);
    usage_layout->setContentsMargins(0, 0, 0, 0);
    usage_layout->setHorizontalSpacing(8);
    usage_layout->setVerticalSpacing(0);
    form->addRow(caption(tr("Usage"), fields_), usage_);
    usage_text_ = new QLineEdit(fields_);
    usage_text_->setObjectName(imageFieldName(QStringLiteral("usageText")));
    usage_text_->setPlaceholderText(QStringLiteral("COLOR_ATTACHMENT, SAMPLED"));
    usage_text_->setToolTip(tr("The engine did not list its usage flags: the flags, separated by "
                               "commas."));
    form->addRow(caption(tr("Usage"), fields_), usage_text_);
    form->setRowVisible(usage_text_, false);
    connect(usage_text_, &QLineEdit::editingFinished, this, [this] { commitUsage(); });

    auto *size_row = new QWidget(fields_);
    auto *size_layout = new QHBoxLayout(size_row);
    size_layout->setContentsMargins(0, 0, 0, 0);
    size_scale_ = new QRadioButton(tr("Screen ×"), size_row);
    size_scale_->setObjectName(imageFieldName(QStringLiteral("sizeScale")));
    size_scale_->setToolTip(tr("As large as the screen, times this."));
    extent_scale_ = new QDoubleSpinBox(size_row);
    extent_scale_->setObjectName(imageFieldName(QStringLiteral("extentScale")));
    extent_scale_->setRange(0.01, 16.0);
    extent_scale_->setDecimals(3);
    extent_scale_->setSingleStep(0.25);
    extent_scale_->setKeyboardTracking(false);
    size_layout->addWidget(size_scale_);
    size_layout->addWidget(extent_scale_, 1);
    // UX-5d, decision 1: as the loop's Count, the size can be an expression
    // the engine evaluates when it composes the pipeline -- in a loop's body,
    // over `element`.
    extent_expression_ = new QLineEdit(size_row);
    extent_expression_->setObjectName(imageFieldName(QStringLiteral("extentScaleExpression")));
    extent_expression_->setPlaceholderText(QStringLiteral("1 / pow2(element + 1)"));
    extent_expression_->setToolTip(
        tr("The screen's size times this expression, evaluated when the pipeline is composed "
           "(in a loop's body, `element` is the copy's number)."));
    extent_expression_->hide();
    size_layout->addWidget(extent_expression_, 1);
    size_expression_toggle_ = new QToolButton(size_row);
    size_expression_toggle_->setObjectName(imageFieldName(QStringLiteral("sizeExpressionToggle")));
    size_expression_toggle_->setText(QStringLiteral("ƒ"));
    size_expression_toggle_->setCheckable(true);
    size_expression_toggle_->setToolTip(tr("Write the size as an expression"));
    size_layout->addWidget(size_expression_toggle_);
    form->addRow(caption(tr("Size"), fields_), size_row);

    auto *fixed_row = new QWidget(fields_);
    auto *fixed_layout = new QHBoxLayout(fixed_row);
    fixed_layout->setContentsMargins(0, 0, 0, 0);
    size_fixed_ = new QRadioButton(tr("Fixed"), fixed_row);
    size_fixed_->setObjectName(imageFieldName(QStringLiteral("sizeFixed")));
    size_fixed_->setToolTip(tr("A width and height of its own, whatever the screen."));
    width_ = new QSpinBox(fixed_row);
    width_->setObjectName(imageFieldName(QStringLiteral("width")));
    height_ = new QSpinBox(fixed_row);
    height_->setObjectName(imageFieldName(QStringLiteral("height")));
    for (QSpinBox *spin : {width_, height_}) {
        spin->setRange(1, 16384);
        spin->setValue(256);
        spin->setKeyboardTracking(false);
    }
    width_expression_ = new QLineEdit(fixed_row);
    width_expression_->setObjectName(imageFieldName(QStringLiteral("widthExpression")));
    width_expression_->setPlaceholderText(QStringLiteral("256 / pow2(element)"));
    height_expression_ = new QLineEdit(fixed_row);
    height_expression_->setObjectName(imageFieldName(QStringLiteral("heightExpression")));
    height_expression_->setPlaceholderText(QStringLiteral("256 / pow2(element)"));
    for (QLineEdit *line : {width_expression_, height_expression_}) {
        line->setToolTip(tr("An expression for the size in pixels, evaluated when the pipeline "
                            "is composed."));
        line->hide();
    }
    fixed_layout->addWidget(size_fixed_);
    fixed_layout->addWidget(width_, 1);
    fixed_layout->addWidget(width_expression_, 1);
    fixed_layout->addWidget(new QLabel(QStringLiteral("×"), fixed_row));
    fixed_layout->addWidget(height_, 1);
    fixed_layout->addWidget(height_expression_, 1);
    form->addRow(QString{}, fixed_row);
    // The forms a body's image is usually given, one press each.
    size_expression_row_ = new QWidget(fields_);
    auto *forms_layout = new QHBoxLayout(size_expression_row_);
    forms_layout->setContentsMargins(0, 0, 0, 0);
    half_each_ = new QPushButton(tr("Half each element"), size_expression_row_);
    half_each_->setObjectName(imageFieldName(QStringLiteral("sizeHalfEachElement")));
    half_each_->setToolTip(tr("Each copy half the size of the one before: the screen times "
                              "1 / pow2(element + 1) (a fixed size: divided by pow2(element))."));
    double_each_ = new QPushButton(tr("Double each element"), size_expression_row_);
    double_each_->setObjectName(imageFieldName(QStringLiteral("sizeDoubleEachElement")));
    double_each_->setToolTip(tr("Each copy twice the size of the one before: times "
                                "pow2(element)."));
    forms_layout->addWidget(half_each_);
    forms_layout->addWidget(double_each_);
    forms_layout->addStretch(1);
    form->addRow(QString{}, size_expression_row_);
    form->setRowVisible(size_expression_row_, false);
    size_form_ = form;
    auto *group = new QButtonGroup(this);
    group->addButton(size_scale_);
    group->addButton(size_fixed_);

    layers_ = new QSpinBox(fields_);
    layers_->setObjectName(imageFieldName(QStringLiteral("layers")));
    layers_->setRange(1, 2048);
    layers_->setKeyboardTracking(false);
    layers_->setToolTip(tr("Array layers (six for a cube)."));
    form->addRow(caption(tr("Layers"), fields_), layers_);

    // UX-7, decision 6: the engine keeps the frame before as a second
    // image (a render target's `history: true`), which a port reads with
    // its Previous frame -- TAA's way in.
    history_ = new QCheckBox(tr("Keep previous frame (history)"), fields_);
    history_->setObjectName(imageFieldName(QStringLiteral("history")));
    history_->setToolTip(tr("The engine keeps the frame before as a second image, which a "
                            "pass's input reads with Previous frame (history: true)."));
    form->addRow(QString{}, history_);
    connect(history_, &QCheckBox::clicked, this, [this](bool checked) {
        auto changed = parameters();
        if (!changed.is_object()) return;
        if (checked) changed["history"] = true;
        else changed.erase("history");
        if (changed == parameters()) return;
        commit(changed);
    });

    connect(size_scale_, &QRadioButton::clicked, this, [this] { commitSize(); });
    connect(size_fixed_, &QRadioButton::clicked, this, [this] { commitSize(); });
    for (QAbstractSpinBox *spin : std::initializer_list<QAbstractSpinBox *>{extent_scale_, width_,
                                                                           height_, layers_})
        connect(spin, &QAbstractSpinBox::editingFinished, this, [this] { commitSize(); });
    connect(size_expression_toggle_, &QToolButton::toggled, this, [this](bool expression) {
        showSizeExpression(expression);
        // Back to a number: the number the field shows.
        if (!expression) commitSize();
    });
    for (QLineEdit *line : {extent_expression_, width_expression_, height_expression_})
        connect(line, &QLineEdit::returnPressed, this, [this] { commitSize(); });
    connect(half_each_, &QPushButton::clicked, this, [this] { putSizeExpression(true); });
    connect(double_each_, &QPushButton::clicked, this, [this] { putSizeExpression(false); });

    note_ = new QLabel(this);
    note_->setObjectName(imageFieldName(QStringLiteral("note")));
    note_->setFont(studioSmallFont());
    note_->setWordWrap(true);
    note_->hide();
    layout->addWidget(note_);
    hide();
}

nlohmann::json GraphCanvasImageFields::parameters() const {
    return model_.nodeParameters(scope_, node_);
}

void GraphCanvasImageFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || node->op != "render.image") {
        node_.clear();
        scope_.clear();
        shown_.clear();
        hide();
        return;
    }
    node_ = node->id;
    scope_ = scope;
    // The formats come with the authoring context, which a plan without a
    // pass of the rendering section never asks for.
    model_.askRenderTargetVocabulary();
    const auto current = parameters();
    // Built again only when what the fields show changed, so a report
    // arriving four times a second takes no field from under the cursor.
    QStringList parts{qtext(scope_), qtext(node_), qtext(current.dump())};
    for (const auto &format : model_.renderTargetFormats()) parts << qtext(format);
    parts << QStringLiteral("|");
    // UX-5b, decision 4: a material pass's image offers its socket's formats.
    for (const auto &format : model_.imageFormatChoicesFor(scope_, node_)) parts << qtext(format);
    parts << QStringLiteral("|");
    for (const auto &usage : model_.renderTargetUsages()) parts << qtext(usage);
    parts << QStringLiteral("|");
    for (const auto &type : model_.imageTypes()) parts << qtext(type.semantic);
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown != shown_) {
        shown_ = shown;
        heading_->setText(tr("Image %1").arg(qtext(node_)));
        load(current);
    }
    followEditable();
    show();
}

void GraphCanvasImageFields::followEditable() { fields_->setEnabled(model_.canEdit()); }

void GraphCanvasImageFields::load(const Json &parameters) {
    QStringList missing;
    const auto format = qtext(stringOf(parameters, "format"));
    {
        const QSignalBlocker block{format_};
        format_->clear();
        const auto formats = model_.imageFormatChoicesFor(scope_, node_);
        // Without the engine's list the format is typed, as it was.
        format_->setEditable(formats.empty());
        for (const auto &choice : formats) format_->addItem(qtext(choice), qtext(choice));
        selectOrAdd(*format_, format);
        if (formats.empty()) {
            format_->setEditText(format);
            missing << tr("its formats");
        }
    }
    if (format_->isEditable() && format_->lineEdit() != format_line_) {
        // Enter in the line sends what was typed. The combo makes its line
        // again each time it becomes editable, so it is connected once each.
        format_line_ = format_->lineEdit();
        connect(format_line_, &QLineEdit::returnPressed, this,
                [this] { commitFormat(format_->currentText().trimmed()); });
    }

    const auto type = parameters.is_object() ? parameters.value("type", Json::object()) : Json::object();
    const auto semantic = qtext(stringOf(type, "semantic"));
    const auto types = model_.imageTypes();
    {
        const QSignalBlocker block{semantic_};
        semantic_->clear();
        for (const auto &choice : types) {
            if (choice.legacy && qtext(choice.semantic) != semantic) continue;
            semantic_->addItem(qtext(choice.label), qtext(choice.semantic));
            semantic_->setItemData(semantic_->count() - 1, qtext(choice.semantic), Qt::ToolTipRole);
        }
        selectOrAdd(*semantic_, semantic);
        if (types.empty()) missing << tr("its image types (the catalog)");
    }
    loadArguments(parameters);
    loadUsage(parameters);
    if (model_.renderTargetUsages().empty()) missing << tr("its usage flags");

    const bool fixed = parameters.is_object() && parameters.contains("width") &&
                       parameters.contains("height");
    {
        const QSignalBlocker block_scale{size_scale_};
        const QSignalBlocker block_fixed{size_fixed_};
        size_scale_->setChecked(!fixed);
        size_fixed_->setChecked(fixed);
    }
    const auto number = [&](const char *key, double fallback) {
        const auto found = parameters.is_object() ? parameters.find(key) : parameters.end();
        return parameters.is_object() && found != parameters.end() && found->is_number()
                   ? found->get<double>()
                   : fallback;
    };
    {
        const QSignalBlocker block{extent_scale_};
        extent_scale_->setValue(number("extent_scale", 1.0));
    }
    for (auto [spin, key] : {std::pair{width_, "width"}, std::pair{height_, "height"},
                             std::pair{layers_, "layers"}}) {
        const QSignalBlocker block{spin};
        spin->setValue(static_cast<int>(number(key, spin == layers_ ? 1.0 : 256.0)));
    }
    // UX-5d, decision 1: a size the document writes as {"$expr": ...}.
    const auto expression = [&](const char *key) -> std::optional<QString> {
        if (!parameters.is_object()) return std::nullopt;
        const auto found = parameters.find(key);
        if (found == parameters.end() || !found->is_object()) return std::nullopt;
        const auto text = found->find("$expr");
        if (text == found->end() || !text->is_string()) return std::nullopt;
        return qtext(text->get<std::string>());
    };
    const auto extent_text = fixed ? std::nullopt : expression("extent_scale");
    const auto width_text = fixed ? expression("width") : std::nullopt;
    const auto height_text = fixed ? expression("height") : std::nullopt;
    const bool written = extent_text || width_text || height_text;
    extent_expression_->setText(extent_text.value_or(QString{}));
    width_expression_->setText(width_text.value_or(QString::number(width_->value())));
    height_expression_->setText(height_text.value_or(QString::number(height_->value())));
    {
        const QSignalBlocker block{size_expression_toggle_};
        size_expression_toggle_->setChecked(written);
    }
    showSizeExpression(written);
    {
        const QSignalBlocker block{history_};
        const auto history = parameters.is_object() ? parameters.find("history") : parameters.end();
        history_->setChecked(parameters.is_object() && history != parameters.end() &&
                             history->is_boolean() && history->get<bool>());
    }

    note_->setVisible(!missing.isEmpty());
    if (!missing.isEmpty())
        note_->setText(tr("The engine did not list %1: those fields take what is typed, or "
                          "show the document's value.")
                           .arg(missing.join(QStringLiteral(", "))));
}

void GraphCanvasImageFields::loadArguments(const Json &parameters) {
    // A load can come from inside one of these combos' own signal (a refused
    // pick), so they go with the event loop rather than here.
    for (QComboBox *combo : argument_boxes_) {
        const auto row = arguments_form_->takeRow(combo);
        for (QLayoutItem *item : {row.labelItem, row.fieldItem}) {
            if (item == nullptr) continue;
            if (QWidget *widget = item->widget()) retire(widget);
            delete item;
        }
    }
    argument_boxes_.clear();
    const auto type = parameters.is_object() ? parameters.value("type", Json::object()) : Json::object();
    const auto semantic = stringOf(type, "semantic");
    const auto arguments = type.is_object() ? type.value("arguments", Json::object()) : Json::object();
    const auto schema = semantic.empty() ? std::nullopt : model_.imageType(semantic);
    // The arguments the schema declares, in its order; without a schema the
    // ones the document holds, each offering only its own value.
    std::vector<GraphCanvasTypeArgument> rows;
    if (schema) rows = schema->arguments;
    else if (arguments.is_object())
        for (const auto &[name, value] : arguments.items())
            rows.push_back({name, value.is_string() ? std::vector{value.get<std::string>()}
                                                    : std::vector<std::string>{},
                            {}});
    for (const auto &row : rows) {
        auto *combo = new QComboBox(arguments_);
        combo->setObjectName(imageFieldName(QStringLiteral("argument.") + qtext(row.name)));
        if (!row.default_value.empty())
            combo->addItem(tr("(default: %1)").arg(qtext(row.default_value)), QString{});
        for (const auto &value : row.allowed) combo->addItem(qtext(value), qtext(value));
        // An argument left out is its default; one with none shows nothing
        // rather than a value the document does not hold.
        selectOrAdd(*combo, qtext(stringOf(arguments, row.name.c_str())));
        arguments_form_->addRow(caption(qtext(row.name), arguments_), combo);
        argument_boxes_.push_back(combo);
        connect(combo, &QComboBox::currentIndexChanged, this,
                [this, name = row.name, combo] { commitArgument(name, combo); });
    }
    arguments_->setVisible(!rows.empty());
}

void GraphCanvasImageFields::loadUsage(const Json &parameters) {
    auto *grid = static_cast<QGridLayout *>(usage_->layout());
    for (auto &[flag, box] : usage_boxes_) {
        grid->removeWidget(box);
        retire(box);
    }
    usage_boxes_.clear();
    std::vector<std::string> current;
    if (parameters.is_object())
        for (const auto &flag : parameters.value("usage", Json::array()))
            if (flag.is_string()) current.push_back(flag.get<std::string>());
    auto flags = model_.renderTargetUsages();
    auto *form = static_cast<QFormLayout *>(fields_->layout());
    const bool listed = !flags.empty();
    form->setRowVisible(usage_, listed);
    form->setRowVisible(usage_text_, !listed);
    if (!listed) {
        QStringList text;
        for (const auto &flag : current) text << qtext(flag);
        const QSignalBlocker block{usage_text_};
        usage_text_->setText(text.join(QStringLiteral(", ")));
        return;
    }
    // A flag the document holds that the engine does not list stays visible.
    for (const auto &flag : current)
        if (std::ranges::find(flags, flag) == flags.end()) flags.push_back(flag);
    int index = 0;
    for (const auto &flag : flags) {
        auto *box = new QCheckBox(qtext(flag), usage_);
        box->setObjectName(imageFieldName(QStringLiteral("usage.") + qtext(flag)));
        box->setFont(studioSmallFont());
        box->setChecked(std::ranges::find(current, flag) != current.end());
        // UX-6a, decision 4: what the flag costs.
        if (flag == "TRANSFER_SRC")
            box->setToolTip(tr("Lets the image be copied or read back from outside the frame "
                               "plan. With it the image is not aliased."));
        grid->addWidget(box, index / 2, index % 2);
        ++index;
        usage_boxes_.emplace_back(flag, box);
        connect(box, &QCheckBox::clicked, this, [this] { commitUsage(); });
    }
}

void GraphCanvasImageFields::commit(const Json &parameters) {
    if (node_.empty()) return;
    if (model_.setNodeParameters(scope_, node_, parameters)) {
        if (submitted_) submitted_();
        return;
    }
    // Refused (an edit in flight, or nothing changed): the fields say what
    // the document says, not the choice that was not sent.
    load(this->parameters());
}

void GraphCanvasImageFields::commitFormat(const QString &format) {
    if (format.isEmpty() || node_.empty()) return;
    // UX-5b, decision 4: one edit, which for a material pass's image moves
    // its other sockets' images to the combination the format belongs to.
    if (model_.setImageFormat(scope_, node_, format.toStdString())) {
        if (submitted_) submitted_();
        return;
    }
    load(parameters());
}

void GraphCanvasImageFields::commitSemantic() {
    const auto semantic = semantic_->currentData().toString().toStdString();
    if (semantic.empty()) return;
    const auto current = parameters();
    const auto type = current.value("type", Json::object());
    if (stringOf(type, "semantic") == semantic) return;
    commit(model_.imageParametersWithSemantic(current, semantic));
}

void GraphCanvasImageFields::commitArgument(const std::string &name, QComboBox *combo) {
    auto changed = parameters();
    auto type = changed.value("type", Json::object());
    if (!type.is_object()) return;
    auto arguments = type.value("arguments", Json::object());
    if (!arguments.is_object()) arguments = Json::object();
    const auto value = combo->currentData().toString().toStdString();
    // The default item leaves the argument out.
    if (value.empty()) arguments.erase(name);
    else arguments[name] = value;
    type["arguments"] = std::move(arguments);
    changed["type"] = std::move(type);
    commit(changed);
}

void GraphCanvasImageFields::commitUsage() {
    auto changed = parameters();
    auto usage = Json::array();
    if (!usage_boxes_.empty()) {
        for (const auto &[flag, box] : usage_boxes_)
            if (box->isChecked()) usage.push_back(flag);
    } else {
        for (const auto &flag : usage_text_->text().split(QRegularExpression{QStringLiteral("[,\\s]+")},
                                                          Qt::SkipEmptyParts))
            usage.push_back(flag.toStdString());
    }
    changed["usage"] = std::move(usage);
    commit(changed);
}

void GraphCanvasImageFields::commitSize() {
    auto changed = parameters();
    // UX-5d, decision 1: behind the f button the size is an expression the
    // engine evaluates (and refuses, with its sentence, when it cannot).
    const bool expression = size_expression_toggle_->isChecked();
    const auto written = [](QLineEdit *line, Json number) {
        const auto text = line->text().trimmed();
        if (text.isEmpty()) return number;
        bool plain = false;
        const double value = text.toDouble(&plain);
        if (plain) return number.is_number_integer() ? Json(static_cast<int>(value)) : Json(value);
        return Json{{"$expr", text.toStdString()}};
    };
    if (size_fixed_->isChecked()) {
        // The parser wants extent_scale beside a fixed size; it is 1 there.
        changed["extent_scale"] = 1.0;
        changed["width"] = expression ? written(width_expression_, width_->value()) : Json(width_->value());
        changed["height"] =
            expression ? written(height_expression_, height_->value()) : Json(height_->value());
    } else {
        changed.erase("width");
        changed.erase("height");
        changed["extent_scale"] = expression ? written(extent_expression_, extent_scale_->value())
                                             : Json(extent_scale_->value());
    }
    // One layer is what an image without the key has.
    if (layers_->value() > 1 || changed.contains("layers")) changed["layers"] = layers_->value();
    showSizeExpression(expression);
    if (changed == parameters()) return;
    commit(changed);
}

void GraphCanvasImageFields::showSizeExpression(bool expression) {
    const bool fixed = size_fixed_->isChecked();
    extent_scale_->setVisible(!expression);
    extent_expression_->setVisible(expression);
    width_->setVisible(!expression);
    height_->setVisible(!expression);
    width_expression_->setVisible(expression);
    height_expression_->setVisible(expression);
    extent_scale_->setEnabled(!fixed);
    extent_expression_->setEnabled(!fixed);
    width_->setEnabled(fixed);
    height_->setEnabled(fixed);
    width_expression_->setEnabled(fixed);
    height_expression_->setEnabled(fixed);
    if (size_form_ != nullptr) size_form_->setRowVisible(size_expression_row_, expression);
}

void GraphCanvasImageFields::putSizeExpression(bool half) {
    {
        const QSignalBlocker block{size_expression_toggle_};
        size_expression_toggle_->setChecked(true);
    }
    if (size_fixed_->isChecked()) {
        const auto pixels = [&](QSpinBox *spin) {
            return half ? QStringLiteral("%1 / pow2(element)").arg(spin->value())
                        : QStringLiteral("%1 * pow2(element)").arg(spin->value());
        };
        width_expression_->setText(pixels(width_));
        height_expression_->setText(pixels(height_));
    } else {
        extent_expression_->setText(half ? QStringLiteral("1 / pow2(element + 1)")
                                         : QStringLiteral("pow2(element)"));
    }
    commitSize();
}

// --- Decision 2: render.fullscreen / render.raster.

namespace {

// UX-5b: a shader list's entry shows the engine's mark for a shader whose
// ports are connected by hand; its data is the reference itself.
void addShaderChoice(QComboBox &combo, const std::string &reference) {
    combo.addItem(qtext(shaderChoiceLabel(reference)), qtext(reference));
    combo.setItemData(combo.count() - 1, qtext(shaderChoiceTooltip(reference)), Qt::ToolTipRole);
}

// The Pass Editor's parts are as wide as their longest choice; beside the
// canvas they take what the column gives them, so the canvas keeps its width.
void compactCombos(QWidget &parent) {
    for (auto *combo : parent.findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(6);
    }
    for (auto *line : parent.findChildren<QLineEdit *>()) line->setMinimumWidth(40);
}

std::string lowerOp(const Json &parameters, const char *key) {
    auto value = stringOf(parameters, key);
    std::ranges::transform(value, value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "dontcare") value = "dont_care";
    return value;
}

} // namespace

GraphCanvasPassFields::GraphCanvasPassFields(GraphCanvasModel &model,
                                             std::function<void()> submitted, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasPass"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 4);
    heading_ = new QLabel(this);
    heading_->setObjectName(passFieldName(QStringLiteral("heading")));
    heading_->setFont(studioSmallFont());
    heading_->setWordWrap(true);
    layout->addWidget(heading_);
    fields_ = new QWidget(this);
    form_ = new QFormLayout(fields_);
    form_->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(fields_);
    const auto shaderCombo = [this](const QString &name, const QString &tip) {
        auto *combo = new QComboBox(fields_);
        combo->setObjectName(passFieldName(name));
        combo->setToolTip(tip);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(12);
        return combo;
    };
    vertex_ = shaderCombo(QStringLiteral("shader.vertex"),
                          tr("The vertex shader: the project's shaders folder and the engine's."));
    fragment_ = shaderCombo(QStringLiteral("shader.fragment"),
                            tr("The fragment shader: the project's shaders folder and the "
                               "engine's fullscreen shaders."));
    form_->addRow(caption(tr("Vertex"), fields_), vertex_);
    auto *fragment_row = new QWidget(fields_);
    auto *fragment_layout = new QHBoxLayout(fragment_row);
    fragment_layout->setContentsMargins(0, 0, 0, 0);
    fragment_layout->addWidget(fragment_, 1);
    new_shader_ = new QPushButton(tr("New…"), fragment_row);
    new_shader_->setObjectName(passFieldName(QStringLiteral("newFragmentShader")));
    new_shader_->setToolTip(tr("Write a new fragment shader into the project from one of the "
                               "Pass Editor's templates, and draw with it."));
    new_shader_->setCheckable(true);
    fragment_layout->addWidget(new_shader_);
    form_->addRow(caption(tr("Fragment"), fields_), fragment_row);
    connect(vertex_, &QComboBox::currentIndexChanged, this, [this] { commit("vertex", vertex_); });
    connect(fragment_, &QComboBox::currentIndexChanged, this, [this] { commit("fragment", fragment_); });

    // UX-5b, decision 2: the Pass Editor's New... row.
    template_row_ = new QWidget(fields_);
    template_row_->setObjectName(passFieldName(QStringLiteral("shaderTemplateRow")));
    auto *template_layout = new QGridLayout(template_row_);
    template_layout->setContentsMargins(0, 0, 0, 0);
    template_kind_ = new QComboBox(template_row_);
    template_kind_->setObjectName(passFieldName(QStringLiteral("shaderTemplateKind")));
    for (const auto kind : FullscreenShaderTemplateKinds)
        template_kind_->addItem(qtext(std::string{fullscreenShaderTemplateKindLabel(kind)}),
                                qtext(std::string{fullscreenShaderTemplateKindName(kind)}));
    template_kind_->setToolTip(tr("What the new shader starts as. Deferred lighting also turns "
                                  "on Uses scene lights and Camera position, and reads the "
                                  "G-buffer the graph's material pass writes."));
    template_name_ = new QLineEdit(template_row_);
    template_name_->setObjectName(passFieldName(QStringLiteral("shaderTemplateName")));
    template_name_->setToolTip(tr("Written to shaders/<name>.frag in the project. Letters, "
                                  "digits, '_' and '-'."));
    create_shader_ = new QPushButton(tr("Create"), template_row_);
    create_shader_->setObjectName(passFieldName(QStringLiteral("createFragmentShader")));
    template_notice_ = new QLabel(template_row_);
    template_notice_->setObjectName(passFieldName(QStringLiteral("shaderTemplateNotice")));
    template_notice_->setFont(studioSmallFont());
    template_notice_->setWordWrap(true);
    template_layout->addWidget(template_kind_, 0, 0, 1, 2);
    template_layout->addWidget(template_name_, 1, 0);
    template_layout->addWidget(create_shader_, 1, 1);
    template_layout->addWidget(template_notice_, 2, 0, 1, 2);
    form_->addRow(QString{}, template_row_);
    form_->setRowVisible(template_row_, false);
    connect(new_shader_, &QPushButton::toggled, this, [this](bool on) { showTemplateRow(on); });
    connect(template_kind_, &QComboBox::currentIndexChanged, this, [this] { proposeTemplateName(); });
    connect(create_shader_, &QPushButton::clicked, this, [this] { createTemplate(); });
    connect(template_name_, &QLineEdit::returnPressed, this, [this] { createTemplate(); });

    ports_ = new QLabel(fields_);
    ports_->setObjectName(passFieldName(QStringLiteral("ports")));
    ports_->setFont(studioSmallFont());
    ports_->setWordWrap(true);
    ports_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form_->addRow(caption(tr("Ports"), fields_), ports_);
    loop_element_ = new QLabel(fields_);
    loop_element_->setObjectName(passFieldName(QStringLiteral("loopElement")));
    loop_element_->setFont(studioSmallFont());
    loop_element_->setWordWrap(true);
    form_->addRow(QString{}, loop_element_);
    form_->setRowVisible(loop_element_, false);

    // UX-6a, decision 5: the sampling of each image the pass reads, beside
    // the canvas -- the right-click entries were the only place, and a
    // scenario could not reach them.
    inputs_ = new QWidget(fields_);
    inputs_->setObjectName(passFieldName(QStringLiteral("inputs")));
    inputs_form_ = new QFormLayout(inputs_);
    inputs_form_->setContentsMargins(0, 0, 0, 0);
    form_->addRow(caption(tr("Inputs"), fields_), inputs_);

    // UX-7, decision 2: shader.defines, a name and a value a row.
    defines_ = new ShaderDefinesForm(QStringLiteral("pelican.graphCanvasPass"), fields_);
    form_->addRow(caption(tr("Defines"), fields_), defines_);
    connect(defines_, &PassForm::changed, this, [this] {
        if (!loading_) commitDefines();
    });

    // UX-7, decision 3: a raster pass's depth attachment.
    depth_ = new QComboBox(fields_);
    depth_->setObjectName(passFieldName(QStringLiteral("depth")));
    depth_->setToolTip(tr("The depth image the pass tests and writes: none, a depth image of "
                          "the graph, or a new D32_SFLOAT one the size of its colour image."));
    depth_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    depth_->setMinimumContentsLength(12);
    form_->addRow(caption(tr("Depth"), fields_), depth_);
    connect(depth_, &QComboBox::currentIndexChanged, this, [this] { commitDepth(); });

    // UX-7, decision 4: a raster pass's draw, counted rather than typed as JSON.
    draw_ = new QWidget(fields_);
    draw_->setObjectName(passFieldName(QStringLiteral("draw")));
    auto *draw_layout = new QGridLayout(draw_);
    draw_layout->setContentsMargins(0, 0, 0, 0);
    vertex_count_ = new QSpinBox(draw_);
    vertex_count_->setObjectName(passFieldName(QStringLiteral("vertexCount")));
    vertex_count_->setRange(0, std::numeric_limits<int>::max());
    vertex_count_->setKeyboardTracking(false);
    vertex_count_->setToolTip(tr("draw.vertex_count: the vertices the pass draws (no vertex "
                                 "buffer: the vertex shader makes them from gl_VertexIndex)."));
    instance_count_ = new QSpinBox(draw_);
    instance_count_->setObjectName(passFieldName(QStringLiteral("instanceCount")));
    instance_count_->setRange(0, std::numeric_limits<int>::max());
    instance_count_->setKeyboardTracking(false);
    instance_count_->setToolTip(tr("draw.instance_count: how many times they are drawn "
                                   "(gl_InstanceIndex)."));
    grid_size_ = new QSpinBox(draw_);
    grid_size_->setObjectName(passFieldName(QStringLiteral("gridSize")));
    // 6 · 16384² still fits the count.
    grid_size_->setRange(2, 16385);
    grid_size_->setValue(257);
    grid_size_->setKeyboardTracking(false);
    grid_size_->setToolTip(tr("N: the grid's vertices along a side (257: 256 × 256 quads)."));
    grid_ = new QPushButton(draw_);
    grid_->setObjectName(passFieldName(QStringLiteral("grid")));
    const auto gridText = [this] {
        grid_->setText(tr("Grid %1×%1 (triangles)").arg(grid_size_->value()));
        grid_->setToolTip(tr("Vertices for an N × N grid of vertices drawn as triangles: "
                             "6 · (N − 1)² = %1.")
                              .arg(6LL * (grid_size_->value() - 1) * (grid_size_->value() - 1)));
    };
    gridText();
    draw_layout->addWidget(caption(tr("Vertices"), draw_), 0, 0);
    draw_layout->addWidget(vertex_count_, 0, 1, 1, 2);
    draw_layout->addWidget(caption(tr("Instances"), draw_), 1, 0);
    draw_layout->addWidget(instance_count_, 1, 1, 1, 2);
    draw_layout->addWidget(grid_size_, 2, 1);
    draw_layout->addWidget(grid_, 2, 2);
    draw_layout->setColumnStretch(2, 1);
    form_->addRow(caption(tr("Draw"), fields_), draw_);
    connect(grid_size_, &QSpinBox::valueChanged, this, gridText);
    for (QSpinBox *spin : {vertex_count_, instance_count_})
        connect(spin, &QAbstractSpinBox::editingFinished, this, [this] { commitDraw(); });
    connect(grid_, &QPushButton::clicked, this, [this] {
        const qint64 side = grid_size_->value() - 1;
        {
            const QSignalBlocker block{vertex_count_};
            vertex_count_->setValue(static_cast<int>(6 * side * side));
        }
        commitDraw();
    });

    // UX-5b, decision 3: the Pass Editor's settings, its UX-4a parts.
    settings_ = new QWidget(fields_);
    settings_->setObjectName(passFieldName(QStringLiteral("settings")));
    auto *settings_form = new QFormLayout(settings_);
    settings_form_ = settings_form;
    settings_form->setContentsMargins(0, 0, 0, 0);
    const auto prefix = QStringLiteral("pelican.graphCanvasPass");
    interface_ = new FullscreenPassInterfaceRow(settings_, prefix);
    settings_form->addRow(caption(tr("Frame data"), settings_), interface_);
    raster_ = new RasterStateForm(prefix, settings_);
    raster_->setDepthShown(false);
    raster_->setDrawShown(false);
    raster_->setToolTip(tr("raster_state: how the pass rasterizes and blends (additive for a "
                           "light added onto what is there)."));
    settings_form->addRow(caption(tr("Raster"), settings_), raster_);
    outputs_ = new AttachmentOpsForm(prefix, settings_);
    outputs_->setDepthOpsShown(false);
    outputs_->setToolTip(tr("Load, clear and store of the colour output: the pass's "
                            "color_load_op, color_store_op and clear_color."));
    settings_form->addRow(caption(tr("Output"), settings_), outputs_);
    form_->addRow(settings_);
    compactCombos(*settings_);
    // A typed clear colour is one edit when the typing stops, not one a key.
    settings_timer_ = new QTimer(this);
    settings_timer_->setSingleShot(true);
    settings_timer_->setInterval(500);
    connect(settings_timer_, &QTimer::timeout, this, [this] { commitSettings(); });
    retry_timer_ = new QTimer(this);
    retry_timer_->setInterval(100);
    connect(retry_timer_, &QTimer::timeout, this, [this] { retryWhenEditable(); });
    interface_->setChangedAction([this] {
        if (!loading_) commitSettings();
    });
    connect(raster_, &PassForm::changed, this, [this] {
        if (!loading_) commitSettings();
    });
    connect(outputs_, &PassForm::changed, this, [this] {
        if (loading_) return;
        // A Load / Store pick goes now; a clear colour waits for the typing.
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr &&
            outputs_->isAncestorOf(QApplication::focusWidget()))
            settings_timer_->start();
        else
            commitSettings();
    });
    hide();
}

void GraphCanvasPassFields::setProjectRoot(const std::filesystem::path &root) {
    project_root_ = root;
    new_shader_->setEnabled(!project_root_.empty());
    if (project_root_.empty()) new_shader_->setChecked(false);
}

void GraphCanvasPassFields::setShadersChanged(std::function<void()> changed) {
    shaders_changed_ = std::move(changed);
}

void GraphCanvasPassFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || (node->op != "render.fullscreen" && node->op != "render.raster")) {
        flushDraw();
        node_.clear();
        scope_.clear();
        op_.clear();
        shown_.clear();
        hide();
        return;
    }
    // New... stays open over a passing reselection of the same pass (a
    // redraw shows no node for a moment); another pass closes it.
    const auto template_key = scope + "|" + node->id;
    const bool other = template_key != template_for_;
    template_for_ = template_key;
    // A value waiting to be sent belongs to the pass it was given for.
    if (other && retry_) {
        retry_ = {};
        retry_timer_->stop();
        if (pending_changed_) pending_changed_();
    }
    // EL-4b: the shader combos are on screen: the engine's list is asked for
    // now (once), and replaces the fixed one when it arrives.
    model_.askEngineShaders();
    node_ = node->id;
    scope_ = scope;
    op_ = node->op;
    const auto current = model_.nodeParameters(scope_, node_);
    QStringList parts{qtext(scope_), qtext(node_), qtext(node->op), qtext(current.dump())};
    for (const auto &reference : model_.vertexShaderChoices()) parts << qtext(shaderChoiceLabel(reference));
    parts << QStringLiteral("|");
    for (const auto &reference : model_.fragmentShaderChoices()) parts << qtext(shaderChoiceLabel(reference));
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    const bool shown_changed = shown != shown_;
    if (shown_changed) {
        shown_ = shown;
        heading_->setText(node->op == "render.raster" ? tr("Raster pass %1").arg(qtext(node_))
                                                      : tr("Fullscreen pass %1").arg(qtext(node_)));
        load(current);
    }
    // The plan may say it later than the document: asked on every show.
    showLoopElementRead();
    if (other) {
        // Another pass: New... starts closed, its name proposed afresh.
        const QSignalBlocker block{new_shader_};
        new_shader_->setChecked(false);
        showTemplateRow(false);
        templateNotice({}, false);
    }
    followEditable();
    const bool appearing = isHidden() || shown_changed;
    show();
    // The rows a load showed have their places now, not after the event
    // loop's next layout pass: a press that comes first (a scenario's click
    // right after the pick) lands on the button, not on a row of no height.
    if (appearing) {
        for (QWidget *widget = this; widget != nullptr; widget = widget->parentWidget())
            if (QLayout *own = widget->layout()) own->activate();
        // A scroll area sizes what it holds on its own layout request.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
}

void GraphCanvasPassFields::showLoopElementRead() {
    QString line;
    switch (model_.loopElementRead(scope_, node_)) {
    case GraphCanvasLoopElementRead::in_loop:
        line = tr("This shader reads the loop element (pelican_loop_element)");
        break;
    case GraphCanvasLoopElementRead::outside_loop:
        line = tr("This shader reads the loop element (pelican_loop_element); outside a loop "
                  "it reads element 0");
        break;
    case GraphCanvasLoopElementRead::none:
        break;
    }
    loop_element_->setText(line);
    form_->setRowVisible(loop_element_, !line.isEmpty());
}

void GraphCanvasPassFields::followEditable() {
    fields_->setEnabled(model_.canEdit());
    retryWhenEditable();
}

bool GraphCanvasPassFields::deferWhileBusy(std::function<void()> again) {
    if (model_.canEdit()) return false;
    retry_ = std::move(again);
    // The model says nothing when it becomes editable; it is asked.
    retry_timer_->start();
    if (pending_changed_) pending_changed_();
    return true;
}

void GraphCanvasPassFields::retryWhenEditable() {
    if (!retry_) {
        retry_timer_->stop();
        return;
    }
    if (!model_.canEdit() || node_.empty()) return;
    retry_timer_->stop();
    auto again = std::exchange(retry_, {});
    again();
    if (pending_changed_) pending_changed_();
}

void GraphCanvasPassFields::flushDraw() {
    if (node_.empty() || op_ != "render.raster") return;
    bool typed = false;
    for (QSpinBox *spin : {vertex_count_, instance_count_})
        if (spin->hasFocus()) {
            spin->interpretText();
            typed = true;
        }
    if (typed) commitDraw();
}

void GraphCanvasPassFields::load(const Json &parameters) {
    const auto shader = parameters.is_object() ? parameters.value("shader", Json::object()) : Json::object();
    for (auto [combo, stage, choices] :
         {std::tuple{vertex_, "vertex", &model_.vertexShaderChoices()},
          std::tuple{fragment_, "fragment", &model_.fragmentShaderChoices()}}) {
        const QSignalBlocker block{combo};
        combo->clear();
        for (const auto &reference : *choices) addShaderChoice(*combo, reference);
        selectOrAdd(*combo, qtext(stringOf(shader, stage)));
        // A pass that names no shader for the stage has none to replace.
        form_->setRowVisible(combo == fragment_ ? combo->parentWidget() : combo,
                             shader.is_object() && shader.contains(stage));
    }
    // A template is a fullscreen pass's: a raster pass draws its own vertices.
    new_shader_->setVisible(op_ == "render.fullscreen");
    new_shader_->setEnabled(!project_root_.empty());
    if (op_ != "render.fullscreen") showTemplateRow(false);
    QStringList lines;
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object()) : Json::object();
    if (ports.is_object())
        for (const auto &[name, port] : ports.items()) {
            QStringList facts;
            for (const char *key : {"kind", "access", "binding", "attachment"})
                if (const auto value = stringOf(port, key); !value.empty()) facts << qtext(value);
            lines << QStringLiteral("%1: %2").arg(qtext(name), facts.join(QLatin1Char{' '}));
        }
    QString tip = tr("A pass's input sockets follow its shader's declarations: connect a line to "
                     "one on the canvas. The ports are still in the details' parameters as JSON.");
    // UX-5b, decision 1: what the engine says of the shader it draws with.
    const auto fragment = stringOf(shader, "fragment");
    if (const auto engine = model_.engineShader(fragment)) {
        if (engine->ports_by_hand) {
            lines << tr("%1 takes its ports from the pass that runs it: connect them yourself.")
                         .arg(qtext(fragment));
            tip = qtext(shaderChoiceTooltip(fragment));
        }
        for (const auto &port : engine->resource_ports)
            lines << tr("resource port %1 (declared by the shader)").arg(qtext(port));
    }
    ports_->setText(lines.isEmpty() ? tr("(none declared)") : lines.join(QLatin1Char{'\n'}));
    ports_->setToolTip(tip);
    loadInputSettings(parameters);
    loadDefines(parameters);
    loadDepth();
    loadDraw(parameters);
    loadPassSettings(parameters);
}

void GraphCanvasPassFields::loadDefines(const Json &parameters) {
    const auto shader =
        parameters.is_object() ? parameters.value("shader", Json::object()) : Json::object();
    const bool previous = loading_;
    loading_ = true;
    defines_->setPassName(node_);
    defines_->load(shader.is_object() ? shader.value("defines", Json(nullptr)) : Json(nullptr));
    loading_ = previous;
}

void GraphCanvasPassFields::commitDefines() {
    if (node_.empty()) return;
    // A row the engine would refuse is red with its sentence, and not sent.
    if (!defines_->refusal().isEmpty()) return;
    auto changed = model_.nodeParameters(scope_, node_);
    if (!changed.is_object()) return;
    auto shader = changed.value("shader", Json::object());
    if (!shader.is_object()) return;
    const auto defines = defines_->defines();
    if (defines.is_null()) shader.erase("defines");
    else shader["defines"] = defines;
    changed["shader"] = std::move(shader);
    if (changed == model_.nodeParameters(scope_, node_)) return;
    if (model_.setNodeParameters(scope_, node_, changed)) {
        if (submitted_) submitted_();
        return;
    }
    // The value itself waits: a read arriving meanwhile reloads the rows.
    if (deferWhileBusy([this, defines] {
            const bool previous = loading_;
            loading_ = true;
            defines_->load(defines);
            loading_ = previous;
            commitDefines();
        }))
        return;
    // Refused: the rows show the document again, from the event loop (the
    // table whose signal is handled is the one a load refills).
    QMetaObject::invokeMethod(
        this, [this] { loadDefines(model_.nodeParameters(scope_, node_)); }, Qt::QueuedConnection);
}

void GraphCanvasPassFields::loadDepth() {
    const bool raster = op_ == "render.raster";
    form_->setRowVisible(depth_, raster);
    if (!raster) return;
    const QSignalBlocker block{depth_};
    depth_->clear();
    depth_->addItem(tr("(none)"), QString{});
    for (const auto &image : model_.rasterDepthChoices(scope_)) depth_->addItem(qtext(image), qtext(image));
    depth_->addItem(tr("New depth image (D32_SFLOAT)"),
                    qtext(std::string{GraphCanvasModel::kNewRasterDepth}));
    selectOrAdd(*depth_, qtext(model_.rasterDepth(scope_, node_)));
    if (depth_->currentIndex() < 0) depth_->setCurrentIndex(0);
}

void GraphCanvasPassFields::commitDepth() {
    if (node_.empty() || op_ != "render.raster") return;
    const auto picked = depth_->currentData().toString().toStdString();
    if (picked == model_.rasterDepth(scope_, node_)) return;
    if (model_.setRasterDepth(scope_, node_, picked)) {
        if (submitted_) submitted_();
        return;
    }
    if (deferWhileBusy([this, picked] {
            const QSignalBlocker block{depth_};
            selectOrAdd(*depth_, qtext(picked));
            commitDepth();
        }))
        return;
    QMetaObject::invokeMethod(this, [this] { loadDepth(); }, Qt::QueuedConnection);
}

void GraphCanvasPassFields::loadDraw(const Json &parameters) {
    const bool raster = op_ == "render.raster";
    form_->setRowVisible(draw_, raster);
    if (!raster) return;
    const auto draw = parameters.is_object() ? parameters.value("draw", Json::object()) : Json::object();
    const auto count = [&](const char *key, int fallback) {
        const auto found = draw.is_object() ? draw.find(key) : draw.end();
        return draw.is_object() && found != draw.end() && found->is_number_integer()
                   ? static_cast<int>(std::clamp<std::int64_t>(found->get<std::int64_t>(), 0,
                                                               std::numeric_limits<int>::max()))
                   : fallback;
    };
    for (auto [spin, key, fallback] : {std::tuple{vertex_count_, "vertex_count", 3},
                                       std::tuple{instance_count_, "instance_count", 1}}) {
        // A count being typed (not yet confirmed) is the hand's, not the
        // document's: a reload under it would put the old one back.
        bool parsed = false;
        const int typed = spin->cleanText().toInt(&parsed);
        if (spin->hasFocus() && parsed && typed != spin->value()) continue;
        const QSignalBlocker block{spin};
        spin->setValue(count(key, fallback));
    }
    // A count an attribute or an index buffer gives is not typed here.
    const bool derived = draw.is_object() && (draw.contains("vertex_source") ||
                                              draw.contains("index_source") ||
                                              stringOf(draw, "operation") == "indexed");
    for (QWidget *field : std::initializer_list<QWidget *>{vertex_count_, grid_size_, grid_})
        field->setEnabled(!derived);
    vertex_count_->setToolTip(
        derived ? tr("The count comes from the draw's source (an attribute or an index buffer).")
                : tr("draw.vertex_count: the vertices the pass draws (no vertex buffer: the "
                     "vertex shader makes them from gl_VertexIndex)."));
}

void GraphCanvasPassFields::commitDraw() {
    if (node_.empty() || op_ != "render.raster") return;
    auto changed = model_.nodeParameters(scope_, node_);
    if (!changed.is_object()) return;
    auto draw = changed.value("draw", Json::object());
    if (!draw.is_object()) draw = Json::object();
    if (!draw.contains("vertex_source") && !draw.contains("index_source") &&
        stringOf(draw, "operation") != "indexed")
        draw["vertex_count"] = vertex_count_->value();
    // One instance is what a draw without the key draws.
    if (instance_count_->value() != 1 || draw.contains("instance_count"))
        draw["instance_count"] = instance_count_->value();
    changed["draw"] = std::move(draw);
    if (changed == model_.nodeParameters(scope_, node_)) return;
    if (model_.setNodeParameters(scope_, node_, changed)) {
        if (submitted_) submitted_();
        return;
    }
    if (deferWhileBusy([this, vertices = vertex_count_->value(),
                        instances = instance_count_->value()] {
            {
                const QSignalBlocker block_vertices{vertex_count_};
                const QSignalBlocker block_instances{instance_count_};
                vertex_count_->setValue(vertices);
                instance_count_->setValue(instances);
            }
            commitDraw();
        }))
        return;
    loadDraw(model_.nodeParameters(scope_, node_));
}

void GraphCanvasPassFields::loadInputSettings(const Json &parameters) {
    // The rows a load replaces may hold the combo whose signal is handled.
    while (inputs_form_->rowCount() > 0) {
        const auto row = inputs_form_->takeRow(0);
        for (auto *item : {row.labelItem, row.fieldItem}) {
            if (item == nullptr) continue;
            if (auto *widget = item->widget()) retire(widget);
            delete item;
        }
    }
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object()) : Json::object();
    int rows = 0;
    if (ports.is_object())
        for (const auto &[port, declaration] : ports.items()) {
            if (!declaration.is_object() || stringOf(declaration, "kind") != "image" ||
                stringOf(declaration, "access") != "read" || declaration.contains("attachment"))
                continue;
            const auto settings = model_.portSettings(scope_, node_, port);
            if (!settings) continue;
            auto *row = new QWidget(inputs_);
            auto *row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 0, 0, 0);
            const auto name = passFieldName(QStringLiteral("input.") + qtext(port) + QLatin1Char{'.'});
            // UX-7, decision 1 (engine RS-14): a numbered input takes them
            // too, written on its port as a named one's are.
            const QString sampling_why =
                tr("Only an image the shader samples by a binding of its own takes sampling.");
            const QString mip_why = tr("An attachment takes no mip of its own here.");
            const auto combo = [&](const char *field,
                                   const std::vector<std::pair<QString, QString>> &values,
                                   const QString &current, bool allowed, const QString &why) {
                auto *box = new QComboBox(row);
                box->setObjectName(name + QLatin1String{field});
                for (const auto &[label, value] : values) box->addItem(label, value);
                const int index = box->findData(current);
                box->setCurrentIndex(index < 0 ? 0 : index);
                box->setEnabled(allowed);
                box->setToolTip(allowed ? QString{} : why);
                box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
                box->setMinimumContentsLength(6);
                row_layout->addWidget(box, 1);
                connect(box, &QComboBox::currentIndexChanged, this,
                        [this, port, field, box] { commitInputSetting(port, field, box); });
            };
            combo("filter",
                  {{tr("default (linear)"), QString{}},
                   {QStringLiteral("linear"), QStringLiteral("linear")},
                   {QStringLiteral("nearest"), QStringLiteral("nearest")}},
                  qtext(settings->filter), settings->sampling_allowed, sampling_why);
            combo("address",
                  {{tr("default (repeat)"), QString{}},
                   {QStringLiteral("repeat"), QStringLiteral("repeat")},
                   {QStringLiteral("mirrored_repeat"), QStringLiteral("mirrored_repeat")},
                   {QStringLiteral("clamp_to_edge"), QStringLiteral("clamp_to_edge")}},
                  qtext(settings->address), settings->sampling_allowed, sampling_why);
            std::vector<std::pair<QString, QString>> mips;
            for (int level = 0; level <= 15; ++level)
                mips.emplace_back(tr("mip %1").arg(level), QString::number(level));
            combo("mip", mips, QString::number(settings->mip), settings->subresource_allowed,
                  mip_why);
            auto *label = caption(qtext(port), inputs_);
            label->setObjectName(name + QStringLiteral("label"));
            inputs_form_->addRow(label, row);
            ++rows;
        }
    form_->setRowVisible(inputs_, rows > 0);
}

void GraphCanvasPassFields::commitInputSetting(const std::string &port, const char *field,
                                               QComboBox *combo) {
    if (node_.empty() || combo == nullptr) return;
    auto settings = model_.portSettings(scope_, node_, port);
    if (!settings) return;
    const auto value = combo->currentData().toString();
    const std::string_view which{field};
    if (which == "filter") settings->filter = value.toStdString();
    else if (which == "address") settings->address = value.toStdString();
    else if (which == "mip") settings->mip = value.toLongLong();
    if (settings == model_.portSettings(scope_, node_, port)) return;
    if (model_.setPortSettings(scope_, node_, port, *settings)) {
        if (submitted_) submitted_();
        return;
    }
    // Refused: the rows show the document again, from the event loop.
    QMetaObject::invokeMethod(
        this, [this] { loadInputSettings(model_.nodeParameters(scope_, node_)); },
        Qt::QueuedConnection);
}

std::vector<std::string> GraphCanvasPassFields::colorPorts(const Json &parameters) const {
    std::vector<std::pair<std::int64_t, std::string>> found;
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object()) : Json::object();
    if (ports.is_object())
        for (const auto &[name, port] : ports.items()) {
            if (!port.is_object() || stringOf(port, "attachment") != "color") continue;
            const auto location = port.value("location", Json(0));
            found.emplace_back(location.is_number_integer() ? location.get<std::int64_t>() : 0, name);
        }
    std::ranges::sort(found);
    std::vector<std::string> names;
    for (auto &[location, name] : found) names.push_back(std::move(name));
    return names;
}

void GraphCanvasPassFields::loadPassSettings(const Json &parameters) {
    // Frame data, blend and the colour output's ops are a fullscreen pass's.
    // UX-7, decision 3: a raster pass's raster_state (cull, blend, and the
    // depth rows once it has a depth image) is the same form.
    const bool fullscreen = op_ == "render.fullscreen";
    const bool raster = op_ == "render.raster";
    form_->setRowVisible(settings_, fullscreen || raster);
    settings_form_->setRowVisible(interface_, fullscreen);
    settings_form_->setRowVisible(outputs_, fullscreen);
    if (raster) {
        loading_ = true;
        const Json declaration = parameters.is_object() ? parameters : Json::object();
        raster_->setDepthShown(!model_.rasterDepth(scope_, node_).empty());
        raster_->setColorAttachments(colorPorts(declaration));
        raster_->load(declaration.value("raster_state", Json(nullptr)), Json(nullptr));
        compactCombos(*settings_);
        loading_ = false;
        return;
    }
    if (!fullscreen) return;
    loading_ = true;
    const Json declaration = parameters.is_object() ? parameters : Json::object();
    interface_->load(declaration);
    const auto colors = colorPorts(declaration);
    raster_->setDepthShown(false);
    raster_->setColorAttachments(colors);
    raster_->load(declaration.value("raster_state", Json(nullptr)), Json(nullptr));
    // The pass's own ops stand for each colour output: a graph pass names no
    // output list for them to sit in.
    loaded_settings_ = Json{{"load", lowerOp(declaration, "color_load_op")},
                            {"store", lowerOp(declaration, "color_store_op")},
                            {"clear", declaration.value("clear_color", Json(nullptr))}};
    Json entries = Json::array();
    Json clear = Json::object();
    for (const auto &port : colors) {
        Json entry{{"target", port}};
        if (!loaded_settings_["load"].get<std::string>().empty())
            entry["load_op"] = loaded_settings_["load"];
        if (!loaded_settings_["store"].get<std::string>().empty())
            entry["store_op"] = loaded_settings_["store"];
        entries.push_back(entry.size() == 1 ? Json(port) : entry);
        if (loaded_settings_["clear"].is_array()) clear[port] = loaded_settings_["clear"];
    }
    outputs_->load(entries, clear.empty() ? Json(nullptr) : clear, Json(nullptr));
    outputs_->setDepthOpsShown(false);
    // The rows a load built are new widgets.
    compactCombos(*settings_);
    loading_ = false;
}

void GraphCanvasPassFields::commitSettings() {
    settings_timer_->stop();
    if (node_.empty()) return;
    if (op_ == "render.raster") {
        // UX-7, decision 3: a raster pass's raster_state only.
        auto changed = model_.nodeParameters(scope_, node_);
        if (!changed.is_object()) return;
        if (const auto state = raster_->rasterState(); state.is_null())
            changed.erase("raster_state");
        else
            changed["raster_state"] = state;
        if (changed == model_.nodeParameters(scope_, node_)) return;
        if (model_.setNodeParameters(scope_, node_, changed)) {
            if (submitted_) submitted_();
            return;
        }
        if (deferWhileBusy([this, state = raster_->rasterState()] {
                loading_ = true;
                raster_->load(state, Json(nullptr));
                loading_ = false;
                commitSettings();
            }))
            return;
        QMetaObject::invokeMethod(
            this, [this] { loadPassSettings(model_.nodeParameters(scope_, node_)); },
            Qt::QueuedConnection);
        return;
    }
    if (op_ != "render.fullscreen") return;
    auto changed = model_.nodeParameters(scope_, node_);
    if (!changed.is_object()) return;
    interface_->apply(changed);
    if (const auto state = raster_->rasterState(); state.is_null())
        changed.erase("raster_state");
    else
        changed["raster_state"] = state;
    // The row that moved says what the pass's ops become.
    auto load = loaded_settings_.value("load", std::string{});
    auto store = loaded_settings_.value("store", std::string{});
    const auto entries = outputs_->color();
    for (const auto &entry : entries.is_array() ? entries : Json::array({entries})) {
        const auto row_load = entry.is_object() ? lowerOp(entry, "load_op") : std::string{};
        const auto row_store = entry.is_object() ? lowerOp(entry, "store_op") : std::string{};
        if (row_load != loaded_settings_.value("load", std::string{})) load = row_load;
        if (row_store != loaded_settings_.value("store", std::string{})) store = row_store;
    }
    if (load.empty()) changed.erase("color_load_op");
    else changed["color_load_op"] = load;
    if (store.empty()) changed.erase("color_store_op");
    else changed["color_store_op"] = store;
    Json clear = loaded_settings_.value("clear", Json(nullptr));
    try {
        const auto colors = outputs_->clearColors();
        bool any = false;
        if (colors.is_object())
            for (const auto &[target, value] : colors.items()) {
                any = true;
                if (value != loaded_settings_.value("clear", Json(nullptr))) clear = value;
            }
        if (!any) clear = nullptr;
    } catch (const std::exception &) {
        // A colour still being typed: nothing is sent for it yet.
        return;
    }
    if (clear.is_null()) changed.erase("clear_color");
    else changed["clear_color"] = clear;
    if (changed == model_.nodeParameters(scope_, node_)) return;
    if (model_.setNodeParameters(scope_, node_, changed)) {
        if (submitted_) submitted_();
        return;
    }
    // Refused: the fields show the document again -- from the event loop,
    // for the row whose signal is being handled is one a load replaces.
    QMetaObject::invokeMethod(
        this, [this] { loadPassSettings(model_.nodeParameters(scope_, node_)); },
        Qt::QueuedConnection);
}

void GraphCanvasPassFields::commit(const char *stage, QComboBox *combo) {
    if (node_.empty()) return;
    const auto reference = combo->currentData().toString().toStdString();
    if (reference.empty()) return;
    auto changed = model_.nodeParameters(scope_, node_);
    auto shader = changed.value("shader", Json::object());
    if (!shader.is_object() || !shader.contains(stage)) return;
    shader[stage] = reference;
    changed["shader"] = std::move(shader);
    if (model_.setNodeParameters(scope_, node_, changed)) {
        if (submitted_) submitted_();
        return;
    }
    load(model_.nodeParameters(scope_, node_));
}

void GraphCanvasPassFields::showTemplateRow(bool shown) {
    form_->setRowVisible(template_row_, shown);
    if (shown) proposeTemplateName();
}

void GraphCanvasPassFields::proposeTemplateName() {
    if (project_root_.empty()) return;
    const auto kind = fullscreenShaderTemplateKindFromName(
                          template_kind_->currentData().toString().toStdString())
                          .value_or(FullscreenShaderTemplateKind::pass_through);
    // Named after what it is, or the pass for a pass-through; a file
    // already there is never the proposal.
    const std::string base = kind == FullscreenShaderTemplateKind::pass_through
                                 ? node_
                                 : std::string{fullscreenShaderTemplateKindName(kind)};
    const auto current = template_name_->text().trimmed().toStdString();
    if (!current.empty() && current != auto_template_name_) return;
    auto_template_name_ = engineShaderCopyName(project_root_, base);
    template_name_->setText(qtext(auto_template_name_));
}

void GraphCanvasPassFields::templateNotice(const QString &text, bool failure) {
    template_notice_->setStyleSheet(failure ? QStringLiteral("color: #d94c3d;") : QString{});
    template_notice_->setText(text);
}

void GraphCanvasPassFields::createTemplate() {
    if (node_.empty() || project_root_.empty() || op_ != "render.fullscreen") return;
    const auto name = template_name_->text().trimmed().toStdString();
    if (!isValidShaderTemplateName(name)) {
        templateNotice(tr("A shader name is letters, digits, '_' and '-'."), true);
        return;
    }
    const auto kind = fullscreenShaderTemplateKindFromName(
                          template_kind_->currentData().toString().toStdString())
                          .value_or(FullscreenShaderTemplateKind::pass_through);
    const auto parameters = model_.nodeParameters(scope_, node_);
    // What the shader reads: the inputs the pass has, in their order; with
    // none, the images the template's roles name, each connected by a line
    // from the pass that writes it.
    std::vector<FullscreenShaderTemplateInput> inputs;
    std::vector<GraphCanvasTemplateSource> connect;
    const auto numbered = GraphCanvasModel::numberedPorts(parameters);
    const auto sources = model_.templateSources(scope_, node_);
    if (!numbered.empty()) {
        for (const auto &[index, port] : numbered) {
            (void)index;
            inputs.push_back({port, true});
        }
    } else {
        std::vector<std::string> names;
        for (const auto &source : sources) names.push_back(source.resource);
        // RS-10: the shadow_directional feature's shadow_map, declared outside
        // the graph, is read through a reference the edit adds.
        bool shadow_declared = false;
        for (const auto &target : model_.declaredRenderTargets())
            if (target.name == ShadowMapInput &&
                std::ranges::find(names, target.name) == names.end()) {
                names.push_back(target.name);
                shadow_declared = true;
            }
        for (const auto &resource : fullscreenTemplateInputFill(kind, names)) {
            const auto found = std::ranges::find(sources, resource, &GraphCanvasTemplateSource::resource);
            if (found == sources.end()) {
                if (!shadow_declared || resource != ShadowMapInput) break;
                inputs.push_back({resource, true});
                connect.push_back({resource, {}, "resource"});
                continue;
            }
            inputs.push_back({resource, true});
            connect.push_back(*found);
        }
    }
    ShaderTemplateWrite written;
    try {
        auto source = fullscreenFragmentShaderTemplate(node_, inputs, kind);
        // LC-1s, decision 1: a pass in a loop's body reads which copy it is.
        if (model_.isLoopBodyScope(scope_)) source = withLoopElementInclude(source);
        written = writeShaderTemplate(project_root_, name, source);
    } catch (const std::exception &error) {
        templateNotice(tr("Could not write the shader: %1").arg(QString::fromUtf8(error.what())),
                       true);
        return;
    }
    const auto file = QStringLiteral("shaders/%1.frag").arg(qtext(name));
    if (!written.created) {
        templateNotice(tr("%1 is already in the project and was left as it is: pick it from the "
                          "Fragment list, or name the new one otherwise.")
                           .arg(file),
                       true);
        return;
    }
    if (shaders_changed_) shaders_changed_();
    const auto settings = fullscreenShaderTemplatePassSettings(kind);
    if (!model_.setPassShaderTemplate(scope_, node_, written.reference, settings.uses_light_data,
                                      settings.push_constants, connect)) {
        templateNotice(tr("Wrote %1, but the pass could not take it now: pick it from the "
                          "Fragment list.")
                           .arg(file),
                       true);
        return;
    }
    // UX-6a, decision 1: the name is used now, typed or not. The next New...
    // (or the next template picked) proposes its own free name over it.
    auto_template_name_ = name;
    templateNotice(tr("Wrote %1 reading %2 input(s); the pass draws with %3.")
                       .arg(file)
                       .arg(static_cast<qulonglong>(inputs.size()))
                       .arg(qtext(written.reference)),
                   false);
    if (submitted_) submitted_();
}

// --- UX-3, decision 2: Place.

QString GraphCanvasPlaceFields::placeData(const GraphCanvasPlace &place) {
    if (place.kind == "automatic") return QStringLiteral("automatic|");
    return qtext(place.kind) + QLatin1Char{'|'} + qtext(place.target);
}

GraphCanvasPlaceFields::GraphCanvasPlaceFields(GraphCanvasModel &model,
                                               std::function<void()> submitted, QWidget *parent)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)} {
    setObjectName(QStringLiteral("pelican.graphCanvasPlace"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 2);
    fields_ = new QWidget(this);
    auto *form = new QFormLayout(fields_);
    form->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(fields_);
    place_ = new QComboBox(fields_);
    place_->setObjectName(placeFieldName(QStringLiteral("place")));
    place_->setToolTip(tr("Where the pass runs among the frame's passes: where the engine "
                          "puts it, before or after a pass, or after an anchor."));
    form->addRow(caption(tr("Place"), fields_), place_);
    pass_set_ = new QComboBox(fields_);
    pass_set_->setObjectName(placeFieldName(QStringLiteral("passSet")));
    pass_set_->setToolTip(tr("The pass family (pass_set) it runs in."));
    form->addRow(caption(tr("Family"), fields_), pass_set_);
    connect(place_, &QComboBox::currentIndexChanged, this, [this] { commit(); });
    connect(pass_set_, &QComboBox::currentIndexChanged, this, [this] { commit(); });
    note_ = new QLabel(this);
    note_->setObjectName(placeFieldName(QStringLiteral("note")));
    note_->setFont(studioSmallFont());
    note_->setWordWrap(true);
    note_->hide();
    layout->addWidget(note_);
    hide();
}

void GraphCanvasPlaceFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || !GraphCanvasModel::showsPlace(node->op)) {
        node_.clear();
        scope_.clear();
        op_.clear();
        shown_.clear();
        hide();
        return;
    }
    node_ = node->id;
    scope_ = scope;
    op_ = node->op;
    // The families come with the authoring context.
    model_.askRenderTargetVocabulary();
    QStringList parts{qtext(scope_), qtext(node_), qtext(op_),
                      qtext(model_.nodeParameters(scope_, node_).dump())};
    for (const auto &pass : model_.placePasses(node_)) parts << qtext(pass);
    parts << QStringLiteral("|");
    for (const auto &family : model_.passFamilies()) parts << qtext(family);
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown != shown_) {
        shown_ = shown;
        load();
    }
    followEditable();
    show();
}

void GraphCanvasPlaceFields::followEditable() {
    fields_->setEnabled(model_.canEdit() && GraphCanvasModel::takesPlace(op_));
}

void GraphCanvasPlaceFields::load() {
    const auto place = model_.place(scope_, node_);
    {
        const QSignalBlocker block{place_};
        place_->clear();
        place_->addItem(tr("automatic"), QStringLiteral("automatic|"));
        for (const auto &pass : model_.placePasses(node_)) {
            place_->addItem(tr("before %1").arg(qtext(pass)), QStringLiteral("before|") + qtext(pass));
            place_->addItem(tr("after %1").arg(qtext(pass)), QStringLiteral("after|") + qtext(pass));
        }
        for (const auto &anchor : GraphCanvasModel::loopAnchors())
            place_->addItem(tr("after anchor %1").arg(qtext(anchor)),
                            QStringLiteral("anchor|") + qtext(anchor));
        // What the document says stays visible, a pass the plan does not
        // list (yet) and an insert the field has no form for included.
        const auto current = placeData(place);
        if (place_->findData(current) < 0) {
            const auto label = place.kind == "other" ? tr("insert %1").arg(qtext(place.target))
                               : place.kind == "anchor"
                                   ? tr("after anchor %1").arg(qtext(place.target))
                                   : tr("%1 %2").arg(qtext(place.kind), qtext(place.target));
            place_->addItem(label, current);
        }
        place_->setCurrentIndex(place_->findData(current));
    }
    {
        const QSignalBlocker block{pass_set_};
        pass_set_->clear();
        pass_set_->addItem(tr("(the frame's first)"), QString{});
        for (const auto &family : model_.passFamilies())
            pass_set_->addItem(qtext(family), qtext(family));
        if (!place.pass_set.empty() && pass_set_->findData(qtext(place.pass_set)) < 0)
            pass_set_->addItem(qtext(place.pass_set), qtext(place.pass_set));
        pass_set_->setCurrentIndex(std::max(0, pass_set_->findData(qtext(place.pass_set))));
    }
    const bool compute = !GraphCanvasModel::takesPlace(op_);
    note_->setVisible(compute);
    note_->setText(compute ? tr("A compute task runs after every pass; it takes no place.")
                           : QString{});
}

void GraphCanvasPlaceFields::commit() {
    if (node_.empty()) return;
    const auto data = place_->currentData().toString();
    const auto bar = data.indexOf(QLatin1Char{'|'});
    GraphCanvasPlace place;
    place.kind = data.left(bar).toStdString();
    place.target = data.mid(bar + 1).toStdString();
    const auto current = model_.place(scope_, node_);
    // A pass that stays where it is keeps its insert's `declared`.
    place.declared =
        current.declared && current.kind == place.kind && current.target == place.target;
    place.pass_set = pass_set_->currentData().toString().toStdString();
    if (place.kind == "other") return;
    if (model_.setPlace(scope_, node_, place)) {
        if (submitted_) submitted_();
        return;
    }
    // Refused, or nothing changed: the field says what the document says.
    load();
}

// --- UX-7, decision 5: the parameters an op cannot be placed without.

GraphCanvasRequiredDialog::GraphCanvasRequiredDialog(const GraphCanvasModel &model,
                                                     const std::string &scope,
                                                     const std::string &op, QWidget *parent)
    : QDialog{parent} {
    setObjectName(QStringLiteral("pelican.graphCanvasRequired"));
    setWindowTitle(tr("%1 needs").arg(qtext(op)));
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(
        tr("The engine does not take %1 without these. They can be changed afterwards in the "
           "node's details.")
            .arg(qtext(op)),
        this);
    intro->setWordWrap(true);
    intro->setFont(studioSmallFont());
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    const auto fieldName = [](const std::string &parameter) {
        return QStringLiteral("pelican.graphCanvasRequired.") + qtext(parameter);
    };
    for (const auto &required : GraphCanvasModel::requiredParameters(op)) {
        QWidget *field = nullptr;
        if (required.kind == "number") {
            auto *spin = new QDoubleSpinBox(this);
            spin->setRange(-1.0e9, 1.0e9);
            spin->setDecimals(4);
            spin->setValue(0.0);
            spin->setToolTip(tr("The constant's value."));
            field = spin;
        } else {
            auto *combo = new QComboBox(this);
            if (required.kind == "input") {
                for (const auto &name : GraphCanvasModel::valueInputNames())
                    combo->addItem(qtext(name), qtext(name));
                combo->setToolTip(tr("The value the engine hands the graph each frame: "
                                     "scene_time from the scene's start, time, delta_time."));
            } else {
                // A buffer of the graph's, or a new name typed in.
                combo->setEditable(true);
                combo->setInsertPolicy(QComboBox::NoInsert);
                for (const auto &buffer : model.bufferChoices(scope))
                    combo->addItem(qtext(buffer), qtext(buffer));
                if (combo->count() == 0) combo->setEditText(QStringLiteral("value_buffer"));
                combo->setToolTip(tr("The buffer the value is written into: one of the graph's "
                                     "render.buffer nodes, or a new name."));
                connect(combo, &QComboBox::editTextChanged, this, [this] { follow(); });
            }
            field = combo;
        }
        field->setObjectName(fieldName(required.name));
        form->addRow(qtext(required.name), field);
        fields_.emplace_back(required, field);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    ok_->setObjectName(QStringLiteral("pelican.graphCanvasRequired.ok"));
    buttons->button(QDialogButtonBox::Cancel)
        ->setObjectName(QStringLiteral("pelican.graphCanvasRequired.cancel"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    follow();
}

void GraphCanvasRequiredDialog::follow() {
    // A buffer needs a name.
    const auto json = parameters();
    bool ready = true;
    for (const auto &[required, field] : fields_)
        if (required.kind == "buffer" &&
            (!json.contains(required.name) || json.at(required.name).get<std::string>().empty()))
            ready = false;
    if (ok_ != nullptr) ok_->setEnabled(ready);
}

nlohmann::json GraphCanvasRequiredDialog::parameters() const {
    Json result = Json::object();
    for (const auto &[required, field] : fields_) {
        if (const auto *spin = qobject_cast<const QDoubleSpinBox *>(field)) {
            result[required.name] = spin->value();
        } else if (const auto *combo = qobject_cast<const QComboBox *>(field)) {
            const auto text = (combo->isEditable() ? combo->currentText() : combo->currentData().toString())
                                  .trimmed()
                                  .toStdString();
            result[required.name] = text;
        }
    }
    return result;
}

// --- UX-3, decision 3: a socket's settings, on its right-click.

void addGraphCanvasSocketEntries(QMenu &menu, GraphCanvasModel &model, const std::string &scope,
                                 const std::string &node, const std::string &port,
                                 std::function<void()> submitted) {
    const auto settings = model.portSettings(scope, node, port);
    if (!settings) return;
    const bool editable = model.canEdit();
    // Each entry sends the settings with its one change, read again at the
    // moment it fires.
    const auto send = [&model, scope, node, port, submitted](
                          const std::function<void(GraphCanvasPortSettings &)> &change) {
        auto now = model.portSettings(scope, node, port);
        if (!now) return;
        change(*now);
        if (model.setPortSettings(scope, node, port, *now) && submitted) submitted();
    };
    auto *heading = menu.addSection(QObject::tr("Socket %1").arg(qtext(port)));
    heading->setObjectName(socketFieldName(QStringLiteral("heading")));

    auto *history = menu.addAction(QObject::tr("Previous frame"));
    history->setObjectName(socketFieldName(QStringLiteral("history")));
    history->setCheckable(true);
    history->setChecked(settings->history);
    history->setEnabled(editable && settings->history_allowed);
    history->setToolTip(settings->history_allowed
                            ? QObject::tr("Read the image as the previous frame left it.")
                            : QObject::tr("Only an image the pass reads (not an attachment) can "
                                          "be read as the previous frame."));
    QObject::connect(history, &QAction::triggered, &menu, [send](bool checked) {
        send([checked](GraphCanvasPortSettings &now) { now.history = checked; });
    });

    const auto choiceMenu = [&](const QString &title, const char *field,
                                const std::vector<std::pair<QString, QString>> &values,
                                const std::string &current, bool allowed,
                                std::string GraphCanvasPortSettings::*member) {
        auto *sub = menu.addMenu(title);
        sub->setObjectName(socketFieldName(QString::fromLatin1(field)));
        sub->setEnabled(editable && allowed);
        if (!allowed)
            sub->setToolTip(QObject::tr("Only an image the shader samples by a binding of its "
                                        "own takes sampling."));
        auto *group = new QActionGroup(sub);
        for (const auto &[label, value] : values) {
            auto *action = sub->addAction(label);
            action->setObjectName(socketFieldName(
                QString::fromLatin1(field) + QLatin1Char{'.'} +
                (value.isEmpty() ? QStringLiteral("default") : value)));
            action->setCheckable(true);
            action->setChecked(qtext(current) == value);
            group->addAction(action);
            QObject::connect(action, &QAction::triggered, &menu, [send, member, value] {
                send([&](GraphCanvasPortSettings &now) { now.*member = value.toStdString(); });
            });
        }
    };
    choiceMenu(QObject::tr("Filter"), "filter",
               {{QObject::tr("default (linear)"), QString{}},
                {QStringLiteral("linear"), QStringLiteral("linear")},
                {QStringLiteral("nearest"), QStringLiteral("nearest")}},
               settings->filter, settings->sampling_allowed, &GraphCanvasPortSettings::filter);
    choiceMenu(QObject::tr("Address"), "address",
               {{QObject::tr("default (repeat)"), QString{}},
                {QStringLiteral("repeat"), QStringLiteral("repeat")},
                {QStringLiteral("mirrored_repeat"), QStringLiteral("mirrored_repeat")},
                {QStringLiteral("clamp_to_edge"), QStringLiteral("clamp_to_edge")}},
               settings->address, settings->sampling_allowed, &GraphCanvasPortSettings::address);

    // The numbers: a spin each, inside the menu.
    const auto spinEntry = [&](const QString &label, const char *field, std::int64_t value,
                               int maximum, bool allowed, const QString &why,
                               std::int64_t GraphCanvasPortSettings::*member) {
        auto *row = new QWidget(&menu);
        auto *row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(24, 2, 8, 2);
        auto *name = new QLabel(label, row);
        name->setFont(studioSmallFont());
        auto *spin = new QSpinBox(row);
        spin->setObjectName(socketFieldName(QString::fromLatin1(field)));
        spin->setRange(0, maximum);
        spin->setValue(static_cast<int>(std::clamp<std::int64_t>(value, 0, maximum)));
        spin->setKeyboardTracking(false);
        spin->setEnabled(editable && allowed);
        row->setToolTip(allowed ? QString{} : why);
        row_layout->addWidget(name, 1);
        row_layout->addWidget(spin);
        auto *entry = new QWidgetAction(&menu);
        entry->setObjectName(socketFieldName(QString::fromLatin1(field) + QStringLiteral("Row")));
        entry->setDefaultWidget(row);
        entry->setEnabled(editable && allowed);
        menu.addAction(entry);
        // Each value the spin takes is one edit (typed digits count once:
        // no keyboard tracking); the menu may close before a focus leaves.
        QObject::connect(spin, &QSpinBox::valueChanged, &menu, [send, member](int picked) {
            send([&](GraphCanvasPortSettings &now) { now.*member = picked; });
        });
    };
    const auto no_subresource = QObject::tr("An attachment reads the whole image.");
    spinEntry(QObject::tr("Mip level"), "mip", settings->mip, 15, settings->subresource_allowed,
              no_subresource, &GraphCanvasPortSettings::mip);
    spinEntry(QObject::tr("Layer"), "layer", settings->layer, 2047, settings->subresource_allowed,
              no_subresource, &GraphCanvasPortSettings::layer);
    spinEntry(QObject::tr("Attachment location"), "location", settings->location, 7,
              settings->location_allowed, QObject::tr("Only a colour attachment has a location."),
              &GraphCanvasPortSettings::location);
    menu.addSeparator();
}

} // namespace PelicanStudio
