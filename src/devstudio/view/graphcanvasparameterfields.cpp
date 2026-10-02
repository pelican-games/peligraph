#include "graphcanvasparameterfields.hpp"

#include "featureparameterform.hpp"
#include "graphcanvasrenderfields.hpp"
#include "studiotheme.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <set>
#include <system_error>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr auto kInvalidStyle = "QDoubleSpinBox, QLineEdit, QComboBox, QCheckBox, QPushButton "
                               "{ border: 1px solid #d94c3d; }";
constexpr auto kRedText = "color: #d94c3d;";

QString qtext(const std::string &value) { return QString::fromStdString(value); }

std::string textOf(const Json &value, const char *key) {
    const auto found = value.find(key);
    return found != value.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

std::string lower(std::string_view value) {
    std::string result{value};
    std::ranges::transform(result, result.begin(),
                           [](unsigned char letter) { return static_cast<char>(std::tolower(letter)); });
    return result;
}

bool isExpressionValue(const Json &value) {
    return value.is_object() && (value.contains("$expr") || value.contains("$format"));
}

std::optional<double> numberOf(const Json &schema, const char *key) {
    const auto found = schema.find(key);
    if (found == schema.end() || !found->is_number()) return std::nullopt;
    return found->get<double>();
}

bool sameJson(const Json &a, const Json &b) {
    if (a.is_number() && b.is_number())
        return std::abs(a.get<double>() - b.get<double>()) <=
               1e-9 * std::max({1.0, std::abs(a.get<double>()), std::abs(b.get<double>())});
    return a == b;
}

QString shortJson(const Json &value) {
    if (value.is_string()) return qtext(value.get<std::string>());
    if (value.is_number_float()) return QString::number(value.get<double>(), 'g', 6);
    return qtext(value.dump());
}

// What a new row of an array or a map holds: the item's default, else the
// least value of its kind.
Json itemFallback(const Json &schema) {
    if (auto fallback = graphParameterDefault(schema); !fallback.is_null()) return fallback;
    const auto kind = textOf(schema, "kind");
    if (kind == "number") return 0.0;
    if (kind == "integer") return std::max<std::int64_t>(0, static_cast<std::int64_t>(
                                                                numberOf(schema, "min").value_or(0.0)));
    if (kind == "bool") return false;
    if (kind == "enum") {
        const auto values = schema.value("enum", Json::array());
        return values.empty() ? Json(std::string{}) : values.front();
    }
    if (kind == "vec2") return Json::array({0.0, 0.0});
    if (kind == "vec3") return Json::array({0.0, 0.0, 0.0});
    if (kind == "vec4") return Json::array({0.0, 0.0, 0.0, 0.0});
    if (kind == "color") return Json::array({0.0, 0.0, 0.0, 1.0});
    if (kind == "object") return Json::object();
    if (kind == "array") return Json::array();
    if (kind == "json") return nullptr;
    return "";
}

std::string toolTipOf(const Json &schema) {
    std::string tip = textOf(schema, "description");
    const auto min = numberOf(schema, "min");
    const auto max = numberOf(schema, "max");
    const auto bound = [](double value) {
        return value == std::floor(value) && std::abs(value) < 1e15
                   ? std::to_string(static_cast<long long>(value))
                   : QString::number(value, 'g', 6).toStdString();
    };
    if (min || max) {
        if (!tip.empty()) tip += "\n";
        tip += "Range: " + (min ? bound(*min) : std::string{}) + ".." + (max ? bound(*max) : std::string{});
    }
    if (const auto found = schema.find("default"); found != schema.end()) {
        if (!tip.empty()) tip += "\n";
        tip += "Default: " + shortJson(*found).toStdString();
    }
    return tip;
}

// The engine's path under one parameter, split at its first step: ".a.b" or
// "a.b" -> ("a", ".b"); "[2].c" -> ("2", ".c").
std::pair<std::string, std::string> firstStep(std::string path) {
    if (!path.empty() && path.front() == '.') path.erase(0, 1);
    if (path.empty()) return {};
    if (path.front() == '[') {
        const auto close = path.find(']');
        if (close == std::string::npos) return {path, {}};
        return {path.substr(1, close - 1), path.substr(close + 1)};
    }
    const auto end = path.find_first_of(".[");
    if (end == std::string::npos) return {path, {}};
    return {path.substr(0, end), path.substr(end)};
}

std::filesystem::path freeName(const std::filesystem::path &folder, const std::filesystem::path &file) {
    auto target = folder / file.filename();
    std::error_code error;
    for (int index = 2; std::filesystem::exists(target, error); ++index)
        target = folder / (file.stem().string() + "_" + std::to_string(index) + file.extension().string());
    return target;
}

} // namespace

Json graphParameterDefault(const Json &schema) {
    const auto found = schema.find("default");
    return found == schema.end() ? Json{} : *found;
}

std::vector<std::string> projectAssetReferences(const std::filesystem::path &project_root,
                                                const std::vector<std::string> &extensions) {
    std::vector<std::string> references;
    if (project_root.empty()) return references;
    std::error_code error;
    std::filesystem::recursive_directory_iterator walk{
        project_root, std::filesystem::directory_options::skip_permission_denied, error};
    for (auto end = std::filesystem::recursive_directory_iterator{}; !error && walk != end;
         walk.increment(error)) {
        std::error_code kind;
        if (walk->is_directory(kind)) {
            const auto name = walk->path().filename().string();
            if (!name.empty() && name.front() == '.') walk.disable_recursion_pending();
            continue;
        }
        const auto extension = lower(walk->path().extension().string());
        if (!extensions.empty() && std::ranges::find(extensions, extension) == extensions.end()) continue;
        const auto relative = walk->path().lexically_relative(project_root).generic_u8string();
        if (relative.empty() || relative.starts_with(u8"..")) continue;
        references.push_back("project://" + std::string{relative.begin(), relative.end()});
        if (references.size() >= 2000) break;
    }
    std::ranges::sort(references);
    return references;
}

std::vector<std::string> assetParameterExtensions(std::string_view op, std::string_view name,
                                                  std::string_view hint) {
    const auto said = lower(hint) + " " + lower(name);
    const std::vector<std::string> models{".gltf", ".glb", ".vrm"};
    if (name == "rig" || op.starts_with("pose.")) return models;
    if (op.starts_with("audio.") || said.find("sound") != std::string::npos) return {".wav", ".ogg"};
    if (said.find("model") != std::string::npos || said.find("mesh") != std::string::npos) return models;
    if (said.find("image") != std::string::npos || said.find("texture") != std::string::npos)
        return {".ktx2", ".png"};
    return {};
}

// --- One parameter's editor.

struct GraphParameterEditor::Parts {
    QHBoxLayout *line = nullptr;
    QWidget *plain = nullptr;
    QDoubleSpinBox *spin = nullptr;
    QSlider *slider = nullptr;
    int slider_steps = 1000;
    std::vector<QDoubleSpinBox *> spins;
    QPushButton *swatch = nullptr;
    QCheckBox *check = nullptr;
    QComboBox *combo = nullptr;
    QLineEdit *text = nullptr;
    QPushButton *import = nullptr;
    QToolButton *reset = nullptr;
    QToolButton *fx = nullptr;
    QLineEdit *expression = nullptr;
    // An object's fields.
    QToolButton *fold = nullptr;
    QWidget *body = nullptr;
    QFormLayout *form = nullptr;
    std::vector<std::pair<std::string, GraphParameterEditor *>> fields;
    std::vector<std::pair<std::string, QLabel *>> field_labels;
    Json extra = Json::object();
    // An array's or a map's rows.
    struct RowParts {
        QWidget *row = nullptr;
        QLineEdit *key = nullptr;
        GraphParameterEditor *editor = nullptr;
    };
    QVBoxLayout *rows_layout = nullptr;
    std::vector<RowParts> rows;
    QPushButton *add = nullptr;
    int row_serial = 0;
};

GraphParameterEditor::GraphParameterEditor(Json schema, std::shared_ptr<const GraphParameterChoices> choices,
                                           const QString &name, const std::string &hint, QWidget *parent)
    : QWidget{parent}, schema_(std::move(schema)), choices_{std::move(choices)}, name_{name}, hint_{hint},
      parts_{std::make_unique<Parts>()} {
    setObjectName(name_ + QStringLiteral(".editor"));
    build();
    load();
}

GraphParameterEditor::~GraphParameterEditor() = default;

std::string GraphParameterEditor::kind() const { return textOf(schema_, "kind"); }

QString GraphParameterEditor::child(const QString &suffix) const {
    return suffix.isEmpty() ? name_ : name_ + QLatin1Char{'.'} + suffix;
}

void GraphParameterEditor::build() {
    auto &parts = *parts_;
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(2);
    parts.line = new QHBoxLayout;
    parts.line->setContentsMargins(0, 0, 0, 0);
    outer->addLayout(parts.line);
    setToolTip(qtext(toolTipOf(schema_)));
    const auto k = kind();
    const bool has_fields = schema_.contains("fields") && schema_.at("fields").is_array();
    if (k == "number" || k == "integer") {
        buildNumber();
    } else if (k == "bool") {
        parts.check = new QCheckBox(this);
        parts.check->setObjectName(child({}));
        parts.plain = parts.check;
        parts.line->addWidget(parts.check, 1);
        connect(parts.check, &QCheckBox::toggled, this, [this] { changed(false); });
    } else if (k == "enum" || k == "node_ref" || k == "asset" || (k == "shader" && !has_fields) ||
               (k == "string" && hint_.starts_with("shader:"))) {
        buildCombo();
    } else if (k == "string") {
        // A line; a combo too, shown when Studio knows the values.
        parts.text = new QLineEdit(this);
        parts.text->setObjectName(child({}));
        parts.line->addWidget(parts.text, 1);
        parts.plain = parts.text;
        connect(parts.text, &QLineEdit::editingFinished, this, [this] {
            if (parts_->text->isModified()) changed(false);
            parts_->text->setModified(false);
        });
        buildCombo();
        parts.combo->hide();
        parts.plain = parts.text;
    } else if (k == "vec2" || k == "vec3" || k == "vec4") {
        buildVector(k.back() - '0', false);
    } else if (k == "color") {
        buildVector(4, true);
    } else if ((k == "object" || k == "shader") && has_fields) {
        buildObject();
    } else if ((k == "object" && schema_.contains("items")) || k == "array") {
        buildRows();
    } else {
        // expression, json, or an object the table says nothing more of.
        parts.text = new QLineEdit(this);
        parts.text->setObjectName(child({}));
        parts.text->setPlaceholderText(k == "expression" ? tr("expression") : tr("JSON"));
        parts.line->addWidget(parts.text, 1);
        parts.plain = parts.text;
        connect(parts.text, &QLineEdit::editingFinished, this, [this] {
            if (!parts_->text->isModified()) return;
            parts_->text->setModified(false);
            if (kind() != "expression") {
                const auto bytes = parts_->text->text().trimmed().toStdString();
                if (!bytes.empty() && !Json::accept(bytes)) {
                    setInvalid(std::string{});
                    return;
                }
                setInvalid(std::nullopt);
            }
            changed(false);
        });
    }
    // An entry a loop body may write as an expression: the f line beside it.
    if (schema_.value("expression", false) && k != "expression" && parts.plain != nullptr) {
        parts.fx = new QToolButton(this);
        parts.fx->setObjectName(child(QStringLiteral("f")));
        parts.fx->setText(QStringLiteral("ƒ"));
        parts.fx->setCheckable(true);
        parts.fx->setToolTip(tr("Write it as an expression of the loop (element, count, ...)."));
        parts.expression = new QLineEdit(this);
        parts.expression->setObjectName(child(QStringLiteral("expression")));
        parts.expression->setPlaceholderText(tr("expression"));
        parts.expression->hide();
        parts.line->insertWidget(1, parts.expression, 1);
        parts.line->addWidget(parts.fx);
        connect(parts.fx, &QToolButton::toggled, this, [this](bool on) {
            if (loading_) return;
            showExpression(on);
            if (!on) changed(false);
        });
        connect(parts.expression, &QLineEdit::editingFinished, this, [this] {
            if (!parts_->expression->isModified()) return;
            parts_->expression->setModified(false);
            changed(false);
        });
    }
    if (schema_.contains("default") && parts.fold == nullptr) {
        parts.reset = new QToolButton(this);
        parts.reset->setObjectName(child(QStringLiteral("reset")));
        parts.reset->setText(tr("Reset"));
        parts.reset->setToolTip(tr("Back to the engine's default: %1")
                                    .arg(shortJson(schema_.at("default"))));
        parts.line->addWidget(parts.reset, 0, Qt::AlignTop);
        connect(parts.reset, &QToolButton::clicked, this, [this] {
            setValue(Json{});
            if (edited_) edited_(false);
        });
    }
}

void GraphParameterEditor::buildNumber() {
    auto &parts = *parts_;
    const bool integer = kind() == "integer";
    const auto min = numberOf(schema_, "min");
    const auto max = numberOf(schema_, "max");
    // Not held to the table's range: a value outside it is the engine's to
    // refuse, by name (decision 4).
    parts.spin = makeParameterSpin(this, child({}), integer, std::nullopt);
    if (integer) parts.spin->setRange(-9.0e15, 9.0e15);
    parts.spin->setToolTip(qtext(toolTipOf(schema_)));
    auto *box = new QWidget(this);
    auto *line = new QHBoxLayout(box);
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(parts.spin, 1);
    if (min && max && *max > *min && *max - *min <= 1.0e6) {
        parts.slider = new QSlider(Qt::Horizontal, box);
        parts.slider->setObjectName(child(QStringLiteral("slider")));
        const double span = *max - *min;
        parts.slider_steps = integer && span <= 1000.0 ? static_cast<int>(span) : 1000;
        parts.slider->setRange(0, parts.slider_steps);
        parts.slider->setPageStep(std::max(1, parts.slider_steps / 10));
        parts.slider->setToolTip(parts.spin->toolTip());
        line->addWidget(parts.slider, 2);
        connect(parts.slider, &QSlider::valueChanged, this, [this, min, max, integer](int step) {
            if (loading_) return;
            double value = *min + (*max - *min) * step / parts_->slider_steps;
            if (integer) value = std::round(value);
            const QSignalBlocker block{parts_->spin};
            parts_->spin->setValue(value);
            changed(true);
        });
    }
    connect(parts.spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, min, max] {
        if (loading_) return;
        if (parts_->slider != nullptr && min && max) {
            const QSignalBlocker block{parts_->slider};
            parts_->slider->setValue(static_cast<int>(std::lround(
                (parts_->spin->value() - *min) / (*max - *min) * parts_->slider_steps)));
        }
        changed(true);
    });
    parts.plain = box;
    parts.line->addWidget(box, 1);
}

void GraphParameterEditor::buildVector(int count, bool colour) {
    auto &parts = *parts_;
    auto *box = new QWidget(this);
    box->setObjectName(child({}));
    auto *line = new QHBoxLayout(box);
    line->setContentsMargins(0, 0, 0, 0);
    if (colour) {
        parts.swatch = new QPushButton(box);
        parts.swatch->setObjectName(child(QStringLiteral("swatch")));
        parts.swatch->setFixedSize(36, 20);
        parts.swatch->setAutoDefault(false);
        parts.swatch->setToolTip(tr("Pick a colour. The values are stored linear, as picked."));
        line->addWidget(parts.swatch);
        connect(parts.swatch, &QPushButton::clicked, this, [this] {
            auto &own = *parts_;
            const auto channel = [&](std::size_t at) {
                return static_cast<float>(std::clamp(own.spins[at]->value(), 0.0, 1.0));
            };
            const auto picked = QColorDialog::getColor(
                QColor::fromRgbF(channel(0), channel(1), channel(2), channel(3)), this, name_,
                QColorDialog::ShowAlphaChannel);
            if (!picked.isValid()) return;
            const std::array<double, 4> values{picked.redF(), picked.greenF(), picked.blueF(),
                                               picked.alphaF()};
            for (std::size_t at = 0; at < own.spins.size(); ++at) {
                const QSignalBlocker block{own.spins[at]};
                own.spins[at]->setValue(values[at]);
            }
            changed(false);
        });
    }
    static constexpr std::array<const char *, 4> axes{"x", "y", "z", "w"};
    static constexpr std::array<const char *, 4> channels{"r", "g", "b", "a"};
    for (int at = 0; at < count; ++at) {
        const auto *axis = colour ? channels[static_cast<std::size_t>(at)] : axes[static_cast<std::size_t>(at)];
        auto *caption = new QLabel(QString::fromLatin1(axis).toUpper(), box);
        caption->setFont(studioSmallFont());
        line->addWidget(caption);
        auto *spin = makeParameterSpin(box, child(QString::fromLatin1(axis)), false, std::nullopt);
        if (colour) spin->setDecimals(4);
        parts.spins.push_back(spin);
        line->addWidget(spin, 1);
        connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] {
            if (loading_) return;
            showColour();
            changed(true);
        });
    }
    parts.plain = box;
    parts.line->addWidget(box, 1);
}

void GraphParameterEditor::buildCombo() {
    auto &parts = *parts_;
    parts.combo = new QComboBox(this);
    parts.combo->setObjectName(child({}));
    parts.combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    parts.combo->setMinimumContentsLength(8);
    parts.combo->setToolTip(qtext(toolTipOf(schema_)));
    parts.line->addWidget(parts.combo, 1);
    parts.plain = parts.combo;
    connect(parts.combo, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_) changed(false);
    });
    if (kind() == "asset" && choices_ != nullptr && !choices_->project_root.empty()) {
        parts.import = new QPushButton(tr("Import…"), this);
        parts.import->setObjectName(child(QStringLiteral("import")));
        parts.import->setAutoDefault(false);
        parts.import->setToolTip(tr("Copy a file into the project's assets and use it here (a name "
                                    "already there gets _2)."));
        parts.line->addWidget(parts.import);
        connect(parts.import, &QPushButton::clicked, this, [this] { importAsset(); });
    }
}

void GraphParameterEditor::buildObject() {
    auto &parts = *parts_;
    const auto fields = schema_.at("fields");
    parts.fold = new QToolButton(this);
    parts.fold->setObjectName(child(QStringLiteral("fold")));
    parts.fold->setCheckable(true);
    parts.fold->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    parts.fold->setAutoRaise(true);
    parts.fold->setText(tr("%n field(s)", nullptr, static_cast<int>(fields.size())));
    parts.line->addWidget(parts.fold);
    parts.line->addStretch(1);
    parts.body = new QWidget(this);
    parts.form = new QFormLayout(parts.body);
    parts.form->setContentsMargins(12, 0, 0, 0);
    parts.form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    static_cast<QVBoxLayout *>(layout())->addWidget(parts.body);
    const bool shader = kind() == "shader";
    const auto hint = hint_ + " " + textOf(schema_, "description");
    for (const auto &field : fields) {
        if (!field.is_object()) continue;
        const auto name = textOf(field, "name");
        if (name.empty()) continue;
        const auto field_hint =
            shader && textOf(field, "kind") == "string" ? "shader:" + name : hint;
        auto *editor = new GraphParameterEditor(field, choices_, child(qtext(name)), field_hint, parts.body);
        auto *label = new QLabel(qtext(name) + (field.value("required", false) ? QStringLiteral(" *")
                                                                                : QString{}),
                                 parts.body);
        label->setFont(studioSmallFont());
        label->setToolTip(qtext(toolTipOf(field)));
        parts.form->addRow(label, editor);
        editor->setEdited([this](bool continuous) { changed(continuous); });
        parts.fields.emplace_back(name, editor);
        parts.field_labels.emplace_back(name, label);
    }
    connect(parts.fold, &QToolButton::toggled, this, [this](bool open) {
        parts_->body->setVisible(open);
        parts_->fold->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    parts.fold->setArrowType(Qt::RightArrow);
    parts.body->hide();
}

void GraphParameterEditor::buildRows() {
    auto &parts = *parts_;
    auto *list = new QWidget(this);
    list->setObjectName(child({}));
    parts.rows_layout = new QVBoxLayout(list);
    parts.rows_layout->setContentsMargins(0, 0, 0, 0);
    parts.rows_layout->setSpacing(2);
    parts.add = new QPushButton(QStringLiteral("+"), this);
    parts.add->setObjectName(child(QStringLiteral("add")));
    parts.add->setAutoDefault(false);
    parts.add->setFixedWidth(28);
    parts.add->setToolTip(kind() == "array" ? tr("Add a row.") : tr("Add a key."));
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->addWidget(list);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(parts.add);
    bottom->addStretch(1);
    column->addLayout(bottom);
    parts.line->addLayout(column, 1);
    parts.plain = list;
    connect(parts.add, &QPushButton::clicked, this, [this] {
        const auto items = schema_.value("items", Json{{"kind", "json"}});
        std::string key;
        if (kind() != "array") {
            std::set<std::string> taken;
            for (const auto &row : parts_->rows)
                if (row.key != nullptr) taken.insert(row.key->text().toStdString());
            for (int index = 1; key.empty() || taken.contains(key); ++index) key = "key" + std::to_string(index);
        }
        addRow(itemFallback(items), key);
        changed(false);
    });
}

void GraphParameterEditor::addRow(const Json &value, const std::string &key) {
    auto &parts = *parts_;
    const auto items = schema_.value("items", Json{{"kind", "json"}});
    const auto index = parts.row_serial++;
    auto *row = new QWidget(this);
    auto *line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    Parts::RowParts entry;
    entry.row = row;
    if (kind() != "array") {
        entry.key = new QLineEdit(qtext(key), row);
        entry.key->setObjectName(child(QString::number(index) + QStringLiteral(".key")));
        entry.key->setMaximumWidth(110);
        line->addWidget(entry.key);
        connect(entry.key, &QLineEdit::editingFinished, this, [this, field = entry.key] {
            if (!field->isModified()) return;
            field->setModified(false);
            changed(false);
        });
    }
    entry.editor = new GraphParameterEditor(items, choices_, child(QString::number(index)),
                                            hint_ + " " + textOf(schema_, "description"), row);
    entry.editor->setValue(value);
    line->addWidget(entry.editor, 1);
    auto *remove = new QToolButton(row);
    remove->setObjectName(child(QString::number(index) + QStringLiteral(".remove")));
    remove->setText(QStringLiteral("×"));
    remove->setToolTip(tr("Remove this row."));
    line->addWidget(remove);
    entry.editor->setEdited([this](bool continuous) { changed(continuous); });
    connect(remove, &QToolButton::clicked, this, [this, row] {
        auto &own = parts_->rows;
        const auto found = std::ranges::find(own, row, &Parts::RowParts::row);
        if (found == own.end()) return;
        own.erase(found);
        row->hide();
        for (auto *named : row->findChildren<QWidget *>()) named->setObjectName({});
        row->deleteLater();
        changed(false);
    });
    parts.rows_layout->addWidget(row);
    parts.rows.push_back(entry);
}

std::vector<std::string> GraphParameterEditor::comboChoices() const {
    std::vector<std::string> result;
    const auto k = kind();
    if (k == "enum") {
        for (const auto &value : schema_.value("enum", Json::array()))
            if (value.is_string()) result.push_back(value.get<std::string>());
        return result;
    }
    if (choices_ == nullptr) return result;
    if (k == "node_ref") {
        if (choices_->model != nullptr)
            for (const auto &node : choices_->model->nodes(choices_->scope))
                if (node.id != choices_->node) result.push_back(node.id);
        return result;
    }
    if (k == "asset")
        return projectAssetReferences(choices_->project_root,
                                      assetParameterExtensions(choices_->op, textOf(schema_, "name"),
                                                               hint_ + " " + textOf(schema_, "description")));
    if (k == "shader" || hint_.starts_with("shader:")) {
        std::string stage = hint_.starts_with("shader:") ? hint_.substr(7) : std::string{};
        if (choices_->op == "render.compute") stage = "compute";
        if (stage != "vertex" && stage != "fragment" && stage != "compute") stage.clear();
        if (choices_->model != nullptr)
            for (const auto &shader : choices_->model->engineShaders())
                if (stage.empty() || shader.stage == stage) result.push_back(shader.reference);
        for (const auto &[reference, shader_stage] : projectShaderReferences(choices_->project_root))
            if (stage.empty() || shader_stage.toStdString() == stage) result.push_back(reference.toStdString());
        return result;
    }
    if (k == "string" && choices_->strings)
        if (auto known = choices_->strings(textOf(schema_, "name"), around_)) return *known;
    return result;
}

void GraphParameterEditor::fillCombo() {
    auto &parts = *parts_;
    if (parts.combo == nullptr) return;
    const QSignalBlocker block{parts.combo};
    const bool string_kind = kind() == "string" && !hint_.starts_with("shader:");
    std::vector<std::string> choices = comboChoices();
    if (string_kind) {
        // A string Studio knows nothing of is typed.
        const bool known = choices_ != nullptr && choices_->strings &&
                           choices_->strings(textOf(schema_, "name"), around_).has_value();
        parts.combo->setVisible(known);
        parts.text->setVisible(!known);
        parts.plain = known ? static_cast<QWidget *>(parts.combo) : parts.text;
        if (!known) return;
    }
    const auto fallback = graphParameterDefault(schema_);
    const auto current = set_ && value_.is_string() ? value_.get<std::string>()
                         : fallback.is_string()     ? fallback.get<std::string>()
                                                    : std::string{};
    if (!current.empty() && std::ranges::find(choices, current) == choices.end())
        choices.insert(choices.begin(), current);
    // A required one shown written takes no "(choose)"; one that had it keeps
    // it while it is on screen, so a pick does not move the rows under it.
    const bool had_placeholder = parts.combo->count() > 0 && parts.combo->itemData(0).toString().isEmpty();
    parts.combo->clear();
    // Nothing written: "(none)" -- the engine's default for an entry with one
    // is its own item, marked.
    if (!fallback.is_string()) {
        const bool required = schema_.value("required", false);
        if (!required) parts.combo->addItem(tr("(none)"), QString{});
        else if (current.empty() || (had_placeholder && picked_))
            parts.combo->addItem(tr("(choose)"), QString{});
    }
    for (const auto &choice : choices)
        parts.combo->addItem(fallback.is_string() && choice == fallback.get<std::string>()
                                 ? tr("%1 (default)").arg(qtext(choice))
                                 : qtext(choice),
                             qtext(choice));
    parts.combo->setCurrentIndex(std::max(0, parts.combo->findData(qtext(current))));
}

void GraphParameterEditor::refreshChoices(const Json &around) {
    around_ = around.is_object() ? around : Json::object();
    if (parts_->combo != nullptr && kind() == "string") {
        const bool was = loading_;
        loading_ = true;
        fillCombo();
        loading_ = was;
    }
    for (const auto &[name, editor] : parts_->fields) editor->refreshChoices(around);
}

void GraphParameterEditor::setValue(const Json &value) {
    picked_ = false;
    set_ = !value.is_null();
    value_ = value;
    load();
}

void GraphParameterEditor::load() {
    auto &parts = *parts_;
    const bool was = loading_;
    loading_ = true;
    const auto shown = set_ ? value_ : graphParameterDefault(schema_);
    const bool expression = parts.fx != nullptr && isExpressionValue(shown);
    if (parts.fx != nullptr) {
        const QSignalBlocker block{parts.fx};
        parts.fx->setChecked(expression);
        parts.expression->setText(expression ? qtext(shown.contains("$expr") && shown.at("$expr").is_string()
                                                         ? shown.at("$expr").get<std::string>()
                                                         : shown.dump())
                                             : QString{});
        showExpression(expression);
    }
    const auto k = kind();
    if (parts.spin != nullptr) {
        const QSignalBlocker block{parts.spin};
        parts.spin->setValue(shown.is_number() ? shown.get<double>() : 0.0);
        if (parts.slider != nullptr) {
            const auto min = numberOf(schema_, "min").value_or(0.0);
            const auto max = numberOf(schema_, "max").value_or(1.0);
            const QSignalBlocker block_slider{parts.slider};
            parts.slider->setValue(static_cast<int>(
                std::lround((parts.spin->value() - min) / (max - min) * parts.slider_steps)));
        }
    } else if (parts.check != nullptr) {
        const QSignalBlocker block{parts.check};
        parts.check->setChecked(shown.is_boolean() && shown.get<bool>());
    } else if (!parts.spins.empty()) {
        for (std::size_t at = 0; at < parts.spins.size(); ++at) {
            const QSignalBlocker block{parts.spins[at]};
            const bool given = shown.is_array() && at < shown.size() && shown.at(at).is_number();
            parts.spins[at]->setValue(given ? shown.at(at).get<double>() : (k == "color" && at == 3 ? 1.0 : 0.0));
        }
        showColour();
    } else if (!parts.fields.empty() || parts.fold != nullptr) {
        parts.extra = Json::object();
        if (shown.is_object()) parts.extra = shown;
        for (const auto &[name, editor] : parts.fields) {
            const auto found = shown.is_object() ? shown.find(name) : Json::const_iterator{};
            editor->setValue(shown.is_object() && found != shown.end() ? *found : Json{});
            parts.extra.erase(name);
        }
    } else if (parts.rows_layout != nullptr) {
        for (auto &row : parts.rows) delete row.row;
        parts.rows.clear();
        parts.row_serial = 0;
        if (shown.is_array() && k == "array")
            for (const auto &element : shown) addRow(element, {});
        else if (shown.is_object() && k != "array")
            for (const auto &[key, member] : shown.items()) addRow(member, key);
    } else if (parts.combo != nullptr) {
        fillCombo();
        if (parts.text != nullptr) {
            const QSignalBlocker block{parts.text};
            parts.text->setText(shown.is_string() ? qtext(shown.get<std::string>()) : QString{});
        }
    } else if (parts.text != nullptr) {
        const QSignalBlocker block{parts.text};
        if (k == "expression")
            parts.text->setText(shown.is_object() && shown.contains("$expr") && shown.at("$expr").is_string()
                                    ? qtext(shown.at("$expr").get<std::string>())
                                    : shown.is_string() ? qtext(shown.get<std::string>()) : QString{});
        else
            parts.text->setText(shown.is_null() ? QString{} : qtext(shown.dump()));
    }
    showReset();
    loading_ = was;
}

Json GraphParameterEditor::widgetValue() const {
    const auto &parts = *parts_;
    const auto k = kind();
    if (parts.fx != nullptr && parts.fx->isChecked()) {
        const auto text = parts.expression->text().trimmed().toStdString();
        return text.empty() ? Json{} : Json{{"$expr", text}};
    }
    if (parts.spin != nullptr) {
        if (k == "integer") return static_cast<std::int64_t>(std::llround(parts.spin->value()));
        return parts.spin->value();
    }
    if (parts.check != nullptr) return parts.check->isChecked();
    if (!parts.spins.empty()) {
        auto values = Json::array();
        for (const auto *spin : parts.spins) values.push_back(spin->value());
        return values;
    }
    if (parts.fold != nullptr) {
        auto result = parts.extra.is_object() ? parts.extra : Json::object();
        for (const auto &[name, editor] : parts.fields)
            if (auto field = editor->value(); !field.is_null()) result[name] = std::move(field);
        return result.empty() ? Json{} : result;
    }
    if (parts.rows_layout != nullptr) {
        if (k == "array") {
            auto result = Json::array();
            for (const auto &row : parts.rows) {
                auto element = row.editor->value();
                result.push_back(element.is_null() ? itemFallback(schema_.value("items", Json::object()))
                                                   : std::move(element));
            }
            return result;
        }
        auto result = Json::object();
        for (const auto &row : parts.rows) {
            const auto key = row.key->text().trimmed().toStdString();
            if (key.empty()) continue;
            auto member = row.editor->value();
            result[key] = member.is_null() ? itemFallback(schema_.value("items", Json::object()))
                                           : std::move(member);
        }
        return result;
    }
    if (parts.combo != nullptr && (parts.text == nullptr || parts.plain == parts.combo)) {
        const auto data = parts.combo->currentData().toString().toStdString();
        return data.empty() ? Json{} : Json(data);
    }
    if (parts.text != nullptr) {
        const auto text = parts.text->text().toStdString();
        if (k == "string") return text.empty() ? Json{} : Json(text);
        if (k == "expression") return text.empty() ? Json{} : Json{{"$expr", text}};
        if (text.empty()) return Json{};
        auto parsed = Json::parse(text, nullptr, false);
        return parsed.is_discarded() ? value_ : parsed;
    }
    return value_;
}

Json GraphParameterEditor::value() const {
    if (!set_) return Json{};
    return value_;
}

void GraphParameterEditor::setEdited(std::function<void(bool continuous)> edited) {
    edited_ = std::move(edited);
}

void GraphParameterEditor::changed(bool continuous) {
    if (loading_) return;
    value_ = widgetValue();
    set_ = !value_.is_null();
    picked_ = true;
    showReset();
    if (edited_) edited_(continuous);
}

void GraphParameterEditor::showReset() {
    if (parts_->reset == nullptr) return;
    parts_->reset->setVisible(set_ && !sameJson(value_, graphParameterDefault(schema_)));
}

void GraphParameterEditor::showColour() {
    auto &parts = *parts_;
    if (parts.swatch == nullptr || parts.spins.size() < 3) return;
    const auto channel = [&](std::size_t at) {
        return static_cast<float>(std::clamp(parts.spins[at]->value(), 0.0, 1.0));
    };
    parts.swatch->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #808080;")
                                    .arg(QColor::fromRgbF(channel(0), channel(1), channel(2)).name()));
}

void GraphParameterEditor::showExpression(bool expression) {
    auto &parts = *parts_;
    if (parts.fx == nullptr) return;
    if (parts.plain != nullptr) parts.plain->setVisible(!expression);
    parts.expression->setVisible(expression);
}

void GraphParameterEditor::importAsset() {
    if (choices_ == nullptr || choices_->project_root.empty()) return;
    const auto extensions = assetParameterExtensions(choices_->op, textOf(schema_, "name"),
                                                     hint_ + " " + textOf(schema_, "description"));
    QStringList patterns;
    for (const auto &extension : extensions) {
        patterns << QStringLiteral("*") + qtext(extension);
        patterns << QStringLiteral("*") + qtext(extension).toUpper();
    }
    // Qt's own dialog, so a scenario can name its file field; window-modal,
    // answered when it closes.
    auto *dialog = new QFileDialog{window(), tr("Import a file for %1").arg(name_)};
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("pelican.assetImportDialog"));
    dialog->setOption(QFileDialog::DontUseNativeDialog, true);
    dialog->setFileMode(QFileDialog::ExistingFile);
    if (!patterns.isEmpty()) dialog->setNameFilter(tr("Files (%1)").arg(patterns.join(QLatin1Char{' '})));
    if (auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("fileNameEdit")))
        name->setObjectName(QStringLiteral("pelican.assetImportDialog.fileName"));
    connect(dialog, &QFileDialog::fileSelected, this, [this, extensions](const QString &picked) {
        const std::filesystem::path file{picked.toStdWString()};
        const auto root = choices_->project_root;
        const auto folder =
            root / "assets" /
            (extensions.empty()                 ? "files"
             : extensions.front() == ".wav"     ? "audio"
             : extensions.front() == ".ktx2"    ? "images"
                                                : "models");
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        const auto target = freeName(folder, file);
        if (error || !std::filesystem::copy_file(file, target, error) || error) return;
        const auto relative = target.lexically_relative(root).generic_u8string();
        const auto reference = "project://" + std::string{relative.begin(), relative.end()};
        set_ = true;
        value_ = reference;
        load();
        changed(false);
    });
    dialog->open();
}

void GraphParameterEditor::setInvalid(std::optional<std::string> path) {
    auto &parts = *parts_;
    invalid_ = path.has_value();
    const auto [step, rest] = path ? firstStep(*path) : std::pair<std::string, std::string>{};
    const bool here = path && (step.empty() || (parts.fields.empty() && parts.rows.empty()));
    // The field itself, or the field / row the path goes on to.
    if (parts.plain != nullptr) parts.plain->setStyleSheet(here ? QString::fromLatin1(kInvalidStyle) : QString{});
    for (const auto &[name, editor] : parts.fields)
        editor->setInvalid(path && !here && name == step ? std::optional<std::string>{rest} : std::nullopt);
    for (const auto &[name, label] : parts.field_labels)
        label->setStyleSheet(path && !here && name == step ? QString::fromLatin1(kRedText) : QString{});
    for (std::size_t index = 0; index < parts.rows.size(); ++index) {
        const auto &row = parts.rows[index];
        const bool named = kind() == "array" ? step == std::to_string(index)
                                             : row.key != nullptr && row.key->text().toStdString() == step;
        row.editor->setInvalid(path && !here && named ? std::optional<std::string>{rest} : std::nullopt);
    }
}

bool GraphParameterEditor::typing() const {
    const auto *focused = QApplication::focusWidget();
    return focused != nullptr && (focused == this || isAncestorOf(focused));
}

void GraphParameterEditor::setEditable(bool editable) {
    for (auto *child : findChildren<QWidget *>()) {
        if (qobject_cast<QLabel *>(child) != nullptr) continue;
        if (child == parts_->fold) continue;
        child->setEnabled(editable);
    }
}

// --- The node's fields.

namespace {

struct OwnFields {
    std::string_view op;
    std::vector<std::string> names;
};

const std::vector<OwnFields> &ownFields() {
    static const std::vector<OwnFields> all{
        {"render.image",
         {"type", "initial", "lifetime", "format", "extent_scale", "width", "height", "layers",
          "mip_levels", "usage", "history"}},
        {"render.fullscreen",
         {"ports", "shader", "schedule", "push_constants", "uses_light_data", "raster_state",
          "color_load_op", "color_store_op", "clear_color"}},
        {"render.raster",
         {"ports", "shader", "draw", "schedule", "raster_state", "color_load_op", "color_store_op",
          "clear_color"}},
        {"render.compute", {"ports", "shader", "ray_tracing", "schedule"}},
        {"render.material_pass",
         {"material_contract", "material_variant", "material_filter", "material_range",
          "material_outputs", "material_output_states", "screen_inputs", "surface_resources",
          "material_resources", "gpu_draw_source", "color_load_op", "color_store_op", "clear_color",
          "depth_load_op", "depth_store_op"}},
        {"value.curve", {"keys"}},
        {"value.playback", {"source_begin", "source_end", "duration", "speed", "delay", "wrap"}},
        {"render.scalar", {"target", "value", "offset"}},
        {"audio.playback", {"asset", "bus", "wrap"}},
        {"audio.volume", {}},
        {"audio.pitch", {}},
        {"ui.value", {"node"}},
        {"ui.visible", {"node"}},
    };
    return all;
}

} // namespace

bool GraphCanvasParameterFields::hasOwnFields(std::string_view op) {
    return std::ranges::any_of(ownFields(), [&](const OwnFields &entry) { return entry.op == op; });
}

std::vector<std::string> GraphCanvasParameterFields::ownFieldNames(std::string_view op) {
    for (const auto &entry : ownFields())
        if (entry.op == op) return entry.names;
    return {};
}

GraphCanvasParameterFields::GraphCanvasParameterFields(GraphCanvasModel &model,
                                                       std::function<void()> submitted, QWidget *parent,
                                                       const QString &prefix)
    : QWidget{parent}, model_{model}, submitted_{std::move(submitted)}, prefix_{prefix},
      held_{std::make_unique<GraphCanvasHeldEdits>(model, this)} {
    setObjectName(prefix_);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(2);
    heading_ = new QLabel(this);
    heading_->setObjectName(prefix_ + QStringLiteral(".heading"));
    heading_->setFont(studioSmallFont());
    outer->addWidget(heading_);
    more_ = new QToolButton(this);
    more_->setObjectName(prefix_ + QStringLiteral(".more"));
    more_->setText(tr("More parameters"));
    more_->setCheckable(true);
    more_->setAutoRaise(true);
    more_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    more_->setArrowType(Qt::RightArrow);
    outer->addWidget(more_);
    body_ = new QWidget(this);
    form_ = new QFormLayout(body_);
    form_->setContentsMargins(0, 0, 0, 0);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    outer->addWidget(body_);
    note_ = new QLabel(this);
    note_->setObjectName(prefix_ + QStringLiteral(".incomplete"));
    note_->setFont(studioSmallFont());
    note_->setWordWrap(true);
    note_->hide();
    outer->addWidget(note_);
    connect(more_, &QToolButton::toggled, this, [this](bool open) {
        body_->setVisible(open);
        more_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    gather_ = new QTimer(this);
    gather_->setSingleShot(true);
    gather_->setInterval(150);
    connect(gather_, &QTimer::timeout, this, [this] { send(); });
    held_->setSent([this] {
        if (submitted_) submitted_();
    });
    hide();
}

GraphCanvasParameterFields::~GraphCanvasParameterFields() = default;

bool GraphCanvasParameterFields::valuePending() const noexcept {
    return held_->pending() || gather_->isActive();
}

void GraphCanvasParameterFields::setPendingChanged(std::function<void()> changed) {
    held_->setChanged(std::move(changed));
}

void GraphCanvasParameterFields::setProjectRoot(const std::filesystem::path &root) {
    if (project_root_ == root) return;
    project_root_ = root;
    shown_.clear();
}

void GraphCanvasParameterFields::showNode(const GraphCanvasNode *node, const std::string &scope) {
    const auto table = node != nullptr ? model_.operationParameters(node->op) : Json::array();
    const bool shows = node != nullptr && !node->loop && !node->instance && !table.empty() &&
                       (!shows_ || shows_(node->op));
    if (!shows) {
        // The rows stay made: the same node picked again (a document read
        // passes through no selection) is shown in them as they are.
        release();
        node_.clear();
        scope_.clear();
        op_.clear();
        hide();
        return;
    }
    // Another node: what was typed for the last one goes first.
    if (node_ != node->id || scope_ != scope) release();
    node_ = node->id;
    scope_ = scope;
    op_ = node->op;
    // The rows are made again for another node, op or table; otherwise only
    // the values are read again.
    const QStringList parts{qtext(scope), qtext(node->id), qtext(node->op), qtext(table.dump()),
                            qtext(project_root_.generic_string())};
    const auto shown = parts.join(QChar{0x1f}).toStdString();
    if (shown != shown_) {
        shown_ = shown;
        rebuild(*node, table);
    }
    if (rows_.empty()) {
        hide();
        return;
    }
    connected_inputs_.clear();
    for (const auto &input : node->inputs)
        if (input.connected) connected_inputs_.push_back(input.port);
    // What an edge feeds is off and says so; a view the View field picks is
    // its (LP-4f1), not a row here.
    const bool view = model_.viewField(scope, node->id).has_value();
    for (auto &row : rows_) {
        const bool connected =
            std::ranges::find(connected_inputs_, row.name) != connected_inputs_.end();
        if (connected != row.connected) {
            row.connected = connected;
            row.label->setText(qtext(row.name) + (row.required ? QStringLiteral(" *") : QString{}) +
                               (connected ? QStringLiteral(" ←") : QString{}));
            row.label->setToolTip(connected ? tr("An edge feeds %1: its value is used.").arg(qtext(row.name))
                                            : row.editor->toolTip());
        }
        if (row.name == "view_select") form_->setRowVisible(row.label, !view);
    }
    load(*node);
    followEditable();
    show();
}

void GraphCanvasParameterFields::rebuild(const GraphCanvasNode &node, const Json &table) {
    // From the event loop: a row's own signal may be what led here.
    for (auto &row : rows_) {
        const auto taken = form_->takeRow(row.label);
        for (auto *item : {taken.labelItem, taken.fieldItem}) {
            if (item == nullptr) continue;
            if (auto *widget = item->widget()) {
                widget->hide();
                widget->setObjectName({});
                for (auto *named : widget->findChildren<QWidget *>()) named->setObjectName({});
                widget->deleteLater();
            }
            delete item;
        }
    }
    rows_.clear();
    const bool own = hasOwnFields(node.op);
    auto skipped = ownFieldNames(node.op);
    // The place's and the view's fields show these.
    if (GraphCanvasModel::showsPlace(node.op))
        for (const auto *name : {"insert", "pass_set", "canonical_anchor"}) skipped.emplace_back(name);
    auto choices = std::make_shared<GraphParameterChoices>();
    choices->model = &model_;
    choices->scope = scope_;
    choices->node = node.id;
    choices->op = node.op;
    choices->project_root = project_root_;
    choices->strings = strings_;
    for (const auto &entry : table) {
        if (!entry.is_object()) continue;
        const auto name = textOf(entry, "name");
        if (name.empty() || std::ranges::find(skipped, name) != skipped.end()) continue;
        Row row;
        row.name = name;
        row.required = entry.value("required", false);
        for (const auto &alternative : entry.value("alternatives", Json::array()))
            if (alternative.is_string()) row.alternatives.push_back(alternative.get<std::string>());
        const auto field = prefix_ + QLatin1Char{'.'} + qtext(name);
        row.editor = new GraphParameterEditor(entry, choices, field, {}, body_);
        row.editor->setEdited([this](bool continuous) { edited(continuous); });
        row.label = new QLabel(qtext(name) + (row.required ? QStringLiteral(" *") : QString{}), body_);
        row.label->setObjectName(field + QStringLiteral(".label"));
        row.label->setToolTip(qtext(toolTipOf(entry)) +
                              (row.required ? tr("\nRequired: the engine does not take the node without it.")
                                            : QString{}));
        auto *cell = new QWidget(body_);
        auto *column = new QVBoxLayout(cell);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(1);
        column->addWidget(row.editor);
        row.problem = new QLabel(cell);
        row.problem->setObjectName(field + QStringLiteral(".problem"));
        row.problem->setFont(studioSmallFont());
        row.problem->setWordWrap(true);
        row.problem->setStyleSheet(QString::fromLatin1(kRedText));
        row.problem->hide();
        column->addWidget(row.problem);
        form_->addRow(row.label, cell);
        rows_.push_back(row);
    }
    heading_->setText(own ? QString{} : tr("Parameters (%1)").arg(qtext(node.op)));
    heading_->setVisible(!own);
    more_->setVisible(own && !rows_.empty());
    if (own) {
        const QSignalBlocker block{more_};
        body_->setVisible(more_->isChecked());
    } else {
        body_->show();
    }
    const bool complete = model_.operationParametersComplete(node.op);
    note_->setText(complete ? QString{}
                            : tr("The engine reads more of %1 than it lists; the rest is in the "
                                 "details' JSON rows.")
                                  .arg(qtext(node.op)));
    note_->setVisible(!complete && !own);
}

void GraphCanvasParameterFields::load(const GraphCanvasNode &node) {
    const auto parameters = node.parameters.is_object() ? node.parameters : Json::object();
    // A value typed and not yet sent is not taken from under the hand.
    if (gather_->isActive()) return;
    loaded_ = parameters;
    for (auto &row : rows_) {
        if (row.editor->typing()) continue;
        const auto found = parameters.find(row.name);
        row.editor->setValue(found != parameters.end() ? *found : Json{});
    }
    for (auto &row : rows_) row.editor->refreshChoices(parameters);
    showProblems();
    if (follow_) follow_(parameters);
}

void GraphCanvasParameterFields::showProblems() {
    const auto &refusal = model_.parameterRefusal();
    const bool ours = refusal && refusal->node == node_ && refusal->scope == scope_;
    const auto [refused, rest] = ours ? firstStep(refusal->parameter) : std::pair<std::string, std::string>{};
    for (auto &row : rows_) {
        QString problem;
        bool red = false;
        if (ours && row.name == refused) {
            problem = qtext(refusal->sentence);
            red = true;
            row.editor->setInvalid(rest);
        } else {
            row.editor->setInvalid(std::nullopt);
        }
        if (!red && row.required && !row.connected && row.editor->value().is_null()) {
            const bool alternative = std::ranges::any_of(row.alternatives, [&](const std::string &name) {
                const auto found = loaded_.find(name);
                return (found != loaded_.end() && !found->is_null()) ||
                       std::ranges::find(connected_inputs_, name) != connected_inputs_.end();
            });
            if (!alternative) {
                red = true;
                problem = row.alternatives.empty()
                              ? tr("Required: the engine does not take the node without it.")
                              : tr("Required, or %1 (or connect a line).")
                                    .arg(qtext(row.alternatives.front()));
            }
        }
        row.label->setStyleSheet(red ? QString::fromLatin1(kRedText) : QString{});
        row.problem->setText(problem);
        row.problem->setVisible(!problem.isEmpty());
    }
}

Json GraphCanvasParameterFields::parameters() const {
    auto result = loaded_.is_object() ? loaded_ : Json::object();
    for (const auto &row : rows_) {
        if (row.connected) continue;
        auto value = row.editor->value();
        if (value.is_null()) result.erase(row.name);
        else result[row.name] = std::move(value);
    }
    return result;
}

void GraphCanvasParameterFields::edited(bool continuous) {
    // Choices that follow another value (a clip from its rig) are read again.
    const auto now = parameters();
    for (auto &row : rows_) row.editor->refreshChoices(now);
    if (follow_) follow_(now);
    if (continuous) {
        gather_->start();
        return;
    }
    gather_->stop();
    send();
}

void GraphCanvasParameterFields::send() {
    if (node_.empty()) return;
    const auto changed = parameters();
    if (changed == loaded_) return;
    const auto base = loaded_;
    loaded_ = changed;
    if (held_->sendOrHold(heldParametersEdit(model_, scope_, node_, base, changed))) {
        if (submitted_) submitted_();
        return;
    }
    // Refused (nothing changed after all): the document again.
    loaded_ = base;
    for (auto &row : rows_) {
        const auto found = base.find(row.name);
        row.editor->setValue(found != base.end() ? *found : Json{});
    }
}

void GraphCanvasParameterFields::release() {
    if (gather_->isActive()) {
        gather_->stop();
        send();
    }
}

void GraphCanvasParameterFields::followEditable() {
    const bool editable = !node_.empty() && model_.canEdit();
    for (auto &row : rows_) row.editor->setEditable(editable && !row.connected);
    // A value held for a save in flight goes as soon as the model is free.
    held_->follow();
}

} // namespace PelicanStudio
