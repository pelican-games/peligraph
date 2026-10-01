#include "graphcanvasrenderfields.hpp"

#include "studiotheme.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>
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
    fixed_layout->addWidget(size_fixed_);
    fixed_layout->addWidget(width_, 1);
    fixed_layout->addWidget(new QLabel(QStringLiteral("×"), fixed_row));
    fixed_layout->addWidget(height_, 1);
    form->addRow(QString{}, fixed_row);
    auto *group = new QButtonGroup(this);
    group->addButton(size_scale_);
    group->addButton(size_fixed_);

    layers_ = new QSpinBox(fields_);
    layers_->setObjectName(imageFieldName(QStringLiteral("layers")));
    layers_->setRange(1, 2048);
    layers_->setKeyboardTracking(false);
    layers_->setToolTip(tr("Array layers (six for a cube)."));
    form->addRow(caption(tr("Layers"), fields_), layers_);

    connect(size_scale_, &QRadioButton::clicked, this, [this] { commitSize(); });
    connect(size_fixed_, &QRadioButton::clicked, this, [this] { commitSize(); });
    for (QAbstractSpinBox *spin : std::initializer_list<QAbstractSpinBox *>{extent_scale_, width_,
                                                                           height_, layers_})
        connect(spin, &QAbstractSpinBox::editingFinished, this, [this] { commitSize(); });

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
        const auto formats = model_.imageFormatChoices(parameters);
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
    extent_scale_->setEnabled(!fixed);
    width_->setEnabled(fixed);
    height_->setEnabled(fixed);

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
    if (format.isEmpty()) return;
    auto changed = parameters();
    changed["format"] = format.toStdString();
    commit(changed);
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
    if (size_fixed_->isChecked()) {
        // The parser wants extent_scale beside a fixed size; it is 1 there.
        changed["extent_scale"] = 1.0;
        changed["width"] = width_->value();
        changed["height"] = height_->value();
    } else {
        changed.erase("width");
        changed.erase("height");
        changed["extent_scale"] = extent_scale_->value();
    }
    // One layer is what an image without the key has.
    if (layers_->value() > 1 || changed.contains("layers")) changed["layers"] = layers_->value();
    extent_scale_->setEnabled(!size_fixed_->isChecked());
    width_->setEnabled(size_fixed_->isChecked());
    height_->setEnabled(size_fixed_->isChecked());
    if (changed == parameters()) return;
    commit(changed);
}

// --- Decision 2: render.fullscreen / render.raster.

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
    form_->addRow(caption(tr("Fragment"), fields_), fragment_);
    connect(vertex_, &QComboBox::currentIndexChanged, this, [this] { commit("vertex", vertex_); });
    connect(fragment_, &QComboBox::currentIndexChanged, this, [this] { commit("fragment", fragment_); });
    ports_ = new QLabel(fields_);
    ports_->setObjectName(passFieldName(QStringLiteral("ports")));
    ports_->setFont(studioSmallFont());
    ports_->setWordWrap(true);
    ports_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form_->addRow(caption(tr("Ports"), fields_), ports_);
    hide();
}

void GraphCanvasPassFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    if (node == nullptr || (node->op != "render.fullscreen" && node->op != "render.raster")) {
        node_.clear();
        scope_.clear();
        shown_.clear();
        hide();
        return;
    }
    node_ = node->id;
    scope_ = scope;
    const auto current = model_.nodeParameters(scope_, node_);
    QStringList parts{qtext(scope_), qtext(node_), qtext(node->op), qtext(current.dump())};
    for (const auto &reference : model_.vertexShaderChoices()) parts << qtext(reference);
    parts << QStringLiteral("|");
    for (const auto &reference : model_.fragmentShaderChoices()) parts << qtext(reference);
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown != shown_) {
        shown_ = shown;
        heading_->setText(node->op == "render.raster" ? tr("Raster pass %1").arg(qtext(node_))
                                                      : tr("Fullscreen pass %1").arg(qtext(node_)));
        load(current);
    }
    followEditable();
    show();
}

void GraphCanvasPassFields::followEditable() { fields_->setEnabled(model_.canEdit()); }

void GraphCanvasPassFields::load(const Json &parameters) {
    const auto shader = parameters.is_object() ? parameters.value("shader", Json::object()) : Json::object();
    for (auto [combo, stage, choices] :
         {std::tuple{vertex_, "vertex", &model_.vertexShaderChoices()},
          std::tuple{fragment_, "fragment", &model_.fragmentShaderChoices()}}) {
        const QSignalBlocker block{combo};
        combo->clear();
        for (const auto &reference : *choices) combo->addItem(qtext(reference), qtext(reference));
        selectOrAdd(*combo, qtext(stringOf(shader, stage)));
        // A pass that names no shader for the stage has none to replace.
        form_->setRowVisible(combo, shader.is_object() && shader.contains(stage));
    }
    QStringList lines;
    const auto ports = parameters.is_object() ? parameters.value("ports", Json::object()) : Json::object();
    if (ports.is_object())
        for (const auto &[name, port] : ports.items()) {
            QStringList facts;
            for (const char *key : {"kind", "access", "binding", "attachment"})
                if (const auto value = stringOf(port, key); !value.empty()) facts << qtext(value);
            lines << QStringLiteral("%1: %2").arg(qtext(name), facts.join(QLatin1Char{' '}));
        }
    ports_->setText(lines.isEmpty() ? tr("(none declared)") : lines.join(QLatin1Char{'\n'}));
    ports_->setToolTip(tr("A pass's input sockets follow its shader's declarations: connect a "
                          "line to one on the canvas. The ports are still in the details' "
                          "parameters as JSON."));
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

} // namespace PelicanStudio
