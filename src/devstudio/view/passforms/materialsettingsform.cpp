#include "materialsettingsform.hpp"

#include "materialoutput.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {

using passform::Json;
using passform::text;

namespace {

constexpr std::array material_contracts{"legacy_gbuffer_v1", "deferred_geometry_v1",
                                        "forward_opaque_v1", "forward_transparent_v1"};
constexpr std::array resource_kinds{"screen_inputs", "surface_resources",
                                    "material_resources"};
// What Other may hold: the settings the form has no fields for.
constexpr std::array other_keys{"gpu_draw_source"};

std::vector<std::string> outputTypeNames() {
    std::vector<std::string> result;
    for (int value = static_cast<int>(Pelican::MaterialOutputType::floating);
         value <= static_cast<int>(Pelican::MaterialOutputType::uvec4); ++value)
        result.emplace_back(Pelican::materialOutputTypeName(
            static_cast<Pelican::MaterialOutputType>(value)));
    return result;
}

std::vector<std::string> outputSourceNames() {
    std::vector<std::string> result;
    for (int value = static_cast<int>(Pelican::MaterialOutputSource::custom);
         value <= static_cast<int>(Pelican::MaterialOutputSource::lighting_scene_color);
         ++value)
        result.emplace_back(Pelican::materialOutputSourceName(
            static_cast<Pelican::MaterialOutputSource>(value)));
    return result;
}

std::vector<std::string> writeMaskNames() {
    std::vector<std::string> result;
    for (int mask = Pelican::materialOutputWriteRgba; mask >= 0; --mask)
        result.push_back(
            Pelican::materialOutputWriteMaskName(static_cast<std::uint8_t>(mask)));
    return result;
}

// The names a binding of each kind can have: the engine's screen input
// contracts (materialscreeninput.cpp) and its feature-owned surface ones. A
// material resource is the shader's own port name.
std::vector<std::string> resourceNamesOf(std::string_view kind) {
    if (kind == "screen_inputs")
        return {"opaque_color", "opaque_depth", "scene_depth", "linear_view_depth"};
    if (kind == "surface_resources")
        return {"directional_shadow", "point_shadow", "spot_shadow"};
    return {};
}

void putOptional(Json &object, const char *key, std::optional<Json> value) {
    if (value)
        object[key] = std::move(*value);
    else
        object.erase(key);
}

Json objectOr(const Json &object, const char *key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_object() ? *found : Json::object();
}

// A row's widgets leave the form at once (a Remove button may be the one
// whose click is being handled, so they are deleted once it returns).
void retire(std::initializer_list<QWidget *> widgets) {
    for (QWidget *widget : widgets) {
        if (widget == nullptr) continue;
        widget->hide();
        widget->setParent(nullptr);
        widget->deleteLater();
    }
}

QComboBox *makeChoice(QWidget *parent, const QString &name,
                      const std::vector<std::string> &values) {
    auto *combo = new QComboBox(parent);
    combo->setObjectName(name);
    for (const auto &value : values) combo->addItem(text(value), text(value));
    return combo;
}

} // namespace

struct MaterialSettingsForm::OutputRow {
    Json original = Json::object();
    std::string loaded_name;
    QLineEdit *name = nullptr;
    QComboBox *type = nullptr;
    QComboBox *source = nullptr;
    QComboBox *blend = nullptr;
    QComboBox *write_mask = nullptr;
    QToolButton *remove = nullptr;
    ~OutputRow() {
        retire({name, type, source, blend, write_mask, remove});
    }
};

struct MaterialSettingsForm::ResourceRow {
    Json original = nullptr;
    QComboBox *kind = nullptr;
    QComboBox *name = nullptr;
    QComboBox *resource = nullptr;
    QToolButton *remove = nullptr;
    ~ResourceRow() { retire({kind, name, resource, remove}); }
};

const std::vector<std::string> &MaterialSettingsForm::keys() {
    static const std::vector<std::string> names{
        "material_range",   "material_filter",      "material_variant",
        "material_contract", "gpu_draw_source",     "material_outputs",
        "material_output_states", "screen_inputs",  "surface_resources",
        "material_resources"};
    return names;
}

MaterialSettingsForm::MaterialSettingsForm(const QString &object_prefix,
                                           QWidget *parent)
    : PassForm(object_prefix, parent) {
    setObjectName(childName("materialSettings"));
    auto *form = new QFormLayout(this);
    form->setContentsMargins(0, 0, 0, 0);
    const auto changed = [this] { notifyChanged(); };

    contract_ = new QComboBox(this);
    contract_->setObjectName(childName("materialContract"));
    contract_->addItem(tr("Default (no explicit contract)"), QString{});
    for (const auto *contract : material_contracts)
        contract_->addItem(QString::fromLatin1(contract), QString::fromLatin1(contract));
    contract_->setToolTip(tr("material_contract: what the pass's materials write"));
    QObject::connect(contract_, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     changed);
    form->addRow(tr("Contract"), contract_);

    auto *filter_row = new QWidget(this);
    auto *filter_layout = new QHBoxLayout(filter_row);
    filter_layout->setContentsMargins(0, 0, 0, 0);
    filter_include_ = new QLineEdit(filter_row);
    filter_include_->setObjectName(childName("materialFilter.include"));
    filter_include_->setPlaceholderText(tr("any of these tags"));
    filter_include_->setToolTip(tr("material_filter.include: materials with any "
                                   "of these tags (commas between); empty: every material"));
    filter_exclude_ = new QLineEdit(filter_row);
    filter_exclude_->setObjectName(childName("materialFilter.exclude"));
    filter_exclude_->setPlaceholderText(tr("none of these tags"));
    filter_exclude_->setToolTip(tr("material_filter.exclude: leave out materials "
                                   "with any of these tags"));
    filter_layout->addWidget(filter_include_, 1);
    filter_layout->addWidget(filter_exclude_, 1);
    for (auto *field : {filter_include_, filter_exclude_})
        QObject::connect(field, &QLineEdit::textChanged, this, changed);
    form->addRow(tr("Tags"), filter_row);

    variant_ = new QComboBox(this);
    variant_->setObjectName(childName("materialVariant"));
    variant_->setEditable(true);
    variant_->setInsertPolicy(QComboBox::NoInsert);
    variant_->setToolTip(tr("material_variant: the variant of each material the "
                            "pass draws; empty: the material itself"));
    QObject::connect(variant_, &QComboBox::currentTextChanged, this, changed);
    form->addRow(tr("Variant"), variant_);

    auto *range_row = new QWidget(this);
    auto *range_layout = new QHBoxLayout(range_row);
    range_layout->setContentsMargins(0, 0, 0, 0);
    range_ = new QCheckBox(tr("Only materials"), range_row);
    range_->setObjectName(childName("materialRange"));
    range_->setToolTip(tr("material_range: draw only the materials from start, "
                          "count of them"));
    range_start_ = new QSpinBox(range_row);
    range_start_->setObjectName(childName("materialRange.start"));
    range_start_->setRange(0, std::numeric_limits<int>::max());
    range_start_->setPrefix(tr("from "));
    range_count_ = new QSpinBox(range_row);
    range_count_->setObjectName(childName("materialRange.count"));
    range_count_->setRange(0, std::numeric_limits<int>::max());
    range_count_->setPrefix(tr("count "));
    range_layout->addWidget(range_);
    range_layout->addWidget(range_start_);
    range_layout->addWidget(range_count_);
    range_layout->addStretch(1);
    QObject::connect(range_, &QCheckBox::toggled, this, [this](bool on) {
        range_start_->setEnabled(on);
        range_count_->setEnabled(on);
        notifyChanged();
    });
    for (auto *spin : {range_start_, range_count_})
        QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, changed);
    form->addRow(tr("Range"), range_row);

    outputs_box_ = new QWidget(this);
    outputs_box_->setObjectName(childName("materialOutputs"));
    auto *outputs_layout = new QVBoxLayout(outputs_box_);
    outputs_layout->setContentsMargins(0, 0, 0, 0);
    outputs_name_ = new QLineEdit(outputs_box_);
    outputs_name_->setObjectName(childName("materialOutputs.name"));
    outputs_name_->setPlaceholderText(tr("the outputs' schema name"));
    outputs_name_->setToolTip(tr("material_outputs.name"));
    QObject::connect(outputs_name_, &QLineEdit::textChanged, this, changed);
    outputs_layout->addWidget(outputs_name_);
    auto *outputs_rows = new QWidget(outputs_box_);
    outputs_grid_ = new QGridLayout(outputs_rows);
    outputs_grid_->setContentsMargins(0, 0, 0, 0);
    int column = 0;
    for (const auto &heading :
         {tr("Output"), tr("Type"), tr("Source"), tr("Blend"), tr("Write mask")})
        outputs_grid_->addWidget(new QLabel(heading, outputs_rows), 0, column++);
    outputs_layout->addWidget(outputs_rows);
    add_output_ = new QPushButton(tr("Add output"), outputs_box_);
    add_output_->setObjectName(childName("materialOutputs.add"));
    QObject::connect(add_output_, &QPushButton::clicked, this, [this] {
        if (outputs_name_->text().trimmed().isEmpty())
            outputs_name_->setText(QStringLiteral("pass_outputs"));
        addOutputRow(Json{{"name", "output_" + std::to_string(output_rows_.size())},
                          {"type", "vec4"},
                          {"source", "custom"}},
                     nullptr);
        notifyChanged();
    });
    outputs_layout->addWidget(add_output_, 0, Qt::AlignLeft);
    form->addRow(tr("Outputs"), outputs_box_);

    resources_box_ = new QWidget(this);
    resources_box_->setObjectName(childName("materialResources"));
    auto *resources_layout = new QVBoxLayout(resources_box_);
    resources_layout->setContentsMargins(0, 0, 0, 0);
    auto *resources_rows = new QWidget(resources_box_);
    resources_grid_ = new QGridLayout(resources_rows);
    resources_grid_->setContentsMargins(0, 0, 0, 0);
    column = 0;
    for (const auto &heading : {tr("Binding"), tr("Name"), tr("Reads")})
        resources_grid_->addWidget(new QLabel(heading, resources_rows), 0, column++);
    resources_layout->addWidget(resources_rows);
    add_resource_ = new QPushButton(tr("Add binding"), resources_box_);
    add_resource_->setObjectName(childName("materialResources.add"));
    QObject::connect(add_resource_, &QPushButton::clicked, this, [this] {
        addResourceRow("screen_inputs", "opaque_color", Json(nullptr));
        notifyChanged();
    });
    resources_layout->addWidget(add_resource_, 0, Qt::AlignLeft);
    form->addRow(tr("Resources"), resources_box_);

    other_ = new QPlainTextEdit(this);
    other_->setObjectName(childName("materialOther"));
    other_->setMaximumHeight(80);
    other_->setPlaceholderText(tr("gpu_draw_source (JSON object), when the pass "
                                  "draws from GPU-written commands"));
    other_->setToolTip(tr("The settings this form has no fields for, as JSON: "
                          "{\"gpu_draw_source\": {...}}"));
    QObject::connect(other_, &QPlainTextEdit::textChanged, this, changed);
    form->addRow(tr("Other (JSON)"), other_);
    reset();
}

MaterialSettingsForm::~MaterialSettingsForm() = default;

void MaterialSettingsForm::reset() { load(Json::object()); }

void MaterialSettingsForm::setResourceCandidates(const std::vector<std::string> &resources) {
    resource_candidates_ = resources;
    for (auto &row : resource_rows_) {
        const QSignalBlocker blocker{row->resource};
        const QString current = row->resource->currentText();
        row->resource->clear();
        for (const auto &resource : resources) row->resource->addItem(text(resource));
        row->resource->setEditText(current);
    }
}

void MaterialSettingsForm::setVariantCandidates(const std::vector<std::string> &variants) {
    const QSignalBlocker blocker{variant_};
    const QString current = variant_->currentText();
    variant_->clear();
    for (const auto &variant : variants) variant_->addItem(text(variant));
    variant_->setEditText(current);
}

void MaterialSettingsForm::load(const Json &declaration) {
    const Loading loading{*this};
    original_ = Json::object();
    if (declaration.is_object())
        for (const auto &key : keys())
            if (declaration.contains(key)) original_[key] = declaration.at(key);

    const auto contract = original_.find("material_contract");
    passform::showOptionalValue(*contract_,
                                contract == original_.end() ? nullptr : &*contract);
    const Json filter = objectOr(original_, "material_filter");
    filter_include_->setText(passform::joinNames(filter.value("include", Json::array())));
    filter_exclude_->setText(passform::joinNames(filter.value("exclude", Json::array())));
    variant_->setEditText(original_.contains("material_variant") &&
                                  original_.at("material_variant").is_string()
                              ? text(original_.at("material_variant").get<std::string>())
                              : QString{});
    const Json range = objectOr(original_, "material_range");
    range_->setChecked(original_.contains("material_range"));
    range_start_->setValue(range.value("start", 0));
    range_count_->setValue(range.value("count", 0));
    range_start_->setEnabled(range_->isChecked());
    range_count_->setEnabled(range_->isChecked());

    output_rows_.clear();
    resource_rows_.clear();
    next_line_ = 1;
    const Json outputs = objectOr(original_, "material_outputs");
    const Json states = objectOr(original_, "material_output_states");
    outputs_name_->setText(text(outputs.value("name", std::string{})));
    for (const auto &output : outputs.value("outputs", Json::array())) {
        const auto name = output.is_object() ? output.value("name", std::string{})
                                             : std::string{};
        const auto state = states.find(name);
        addOutputRow(output, state == states.end() ? nullptr : &*state);
    }
    for (const auto *kind : resource_kinds) {
        const Json bindings = objectOr(original_, kind);
        for (const auto &[name, value] : bindings.items()) addResourceRow(kind, name, value);
    }

    Json other = Json::object();
    for (const auto *key : other_keys)
        if (original_.contains(key)) other[key] = original_.at(key);
    other_->setPlainText(other.empty() ? QString{} : text(other.dump(2)));
}

void MaterialSettingsForm::addOutputRow(const Json &output, const Json *state) {
    auto row = std::make_unique<OutputRow>();
    row->original = output.is_object() ? output : Json::object();
    row->loaded_name = row->original.value("name", std::string{});
    auto *parent = outputs_grid_->parentWidget();
    row->name = new QLineEdit(text(row->loaded_name), parent);
    row->type = makeChoice(parent, {}, outputTypeNames());
    passform::selectData(*row->type, text(row->original.value("type", std::string{"vec4"})));
    row->source = makeChoice(parent, {}, outputSourceNames());
    passform::selectData(*row->source,
                         text(row->original.value("source", std::string{"custom"})));
    row->blend = new QComboBox(parent);
    passform::fillOptionalChoices(*row->blend, tr("Material's"),
                                  {"opaque", "blend", "additive"});
    row->write_mask = new QComboBox(parent);
    passform::fillOptionalChoices(*row->write_mask, tr("Material's"), writeMaskNames());
    if (state != nullptr && state->is_object()) {
        const auto blend = state->find("blend");
        passform::showOptionalValue(*row->blend, blend == state->end() ? nullptr : &*blend);
        const auto mask = state->find("write_mask");
        passform::showOptionalValue(*row->write_mask, mask == state->end() ? nullptr : &*mask);
    }
    row->blend->setToolTip(tr("material_output_states.<output>.blend; Material's: "
                              "the material's own render state"));
    row->write_mask->setToolTip(tr("material_output_states.<output>.write_mask"));
    row->remove = new QToolButton(parent);
    row->remove->setText(QStringLiteral("x"));
    row->remove->setToolTip(tr("Remove this output"));
    const int line = next_line_++;
    outputs_grid_->addWidget(row->name, line, 0);
    outputs_grid_->addWidget(row->type, line, 1);
    outputs_grid_->addWidget(row->source, line, 2);
    outputs_grid_->addWidget(row->blend, line, 3);
    outputs_grid_->addWidget(row->write_mask, line, 4);
    outputs_grid_->addWidget(row->remove, line, 5);
    const auto changed = [this] { notifyChanged(); };
    QObject::connect(row->name, &QLineEdit::textChanged, this, changed);
    for (auto *combo : {row->type, row->source, row->blend, row->write_mask})
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                         changed);
    QObject::connect(row->remove, &QToolButton::clicked, this,
                     [this, button = row->remove] {
                         std::erase_if(output_rows_, [button](const auto &candidate) {
                             return candidate->remove == button;
                         });
                         renumber();
                         notifyChanged();
                     });
    output_rows_.push_back(std::move(row));
    renumber();
}

void MaterialSettingsForm::fillResourceNames(ResourceRow &row) {
    const QSignalBlocker blocker{row.name};
    const QString current = row.name->currentText();
    row.name->clear();
    for (const auto &name : resourceNamesOf(row.kind->currentData().toString().toStdString()))
        row.name->addItem(text(name));
    row.name->setEditText(current);
}

void MaterialSettingsForm::addResourceRow(const std::string &kind, const std::string &name,
                                          const Json &value) {
    auto row = std::make_unique<ResourceRow>();
    row->original = value;
    auto *parent = resources_grid_->parentWidget();
    row->kind = new QComboBox(parent);
    for (const auto *each : resource_kinds)
        row->kind->addItem(QString::fromLatin1(each), QString::fromLatin1(each));
    passform::selectData(*row->kind, text(kind));
    row->kind->setToolTip(tr("screen_inputs: images the engine made earlier in the "
                             "frame; surface_resources: a feature's (shadows); "
                             "material_resources: the shader's own ports"));
    row->name = new QComboBox(parent);
    row->name->setEditable(true);
    row->name->setInsertPolicy(QComboBox::NoInsert);
    fillResourceNames(*row);
    row->name->setEditText(text(name));
    row->resource = new QComboBox(parent);
    row->resource->setEditable(true);
    row->resource->setInsertPolicy(QComboBox::NoInsert);
    for (const auto &resource : resource_candidates_) row->resource->addItem(text(resource));
    const std::string reads = value.is_string() ? value.get<std::string>()
                              : value.is_object() ? value.value("resource", std::string{})
                                                  : std::string{};
    row->resource->setEditText(text(reads));
    if (value.is_object())
        row->resource->setToolTip(tr("Its other fields stay: %1").arg(text(value.dump())));
    row->remove = new QToolButton(parent);
    row->remove->setText(QStringLiteral("x"));
    row->remove->setToolTip(tr("Remove this binding"));
    const int line = next_line_++;
    resources_grid_->addWidget(row->kind, line, 0);
    resources_grid_->addWidget(row->name, line, 1);
    resources_grid_->addWidget(row->resource, line, 2);
    resources_grid_->addWidget(row->remove, line, 3);
    const auto changed = [this] { notifyChanged(); };
    QObject::connect(row->kind, qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this, raw = row.get()] {
                         fillResourceNames(*raw);
                         notifyChanged();
                     });
    QObject::connect(row->name, &QComboBox::currentTextChanged, this, changed);
    QObject::connect(row->resource, &QComboBox::currentTextChanged, this, changed);
    QObject::connect(row->remove, &QToolButton::clicked, this,
                     [this, button = row->remove] {
                         std::erase_if(resource_rows_, [button](const auto &candidate) {
                             return candidate->remove == button;
                         });
                         renumber();
                         notifyChanged();
                     });
    resource_rows_.push_back(std::move(row));
    renumber();
}

// The rows' children are named by their place, so the automation channel
// reaches the n-th row whatever was removed before it.
void MaterialSettingsForm::renumber() {
    for (std::size_t index = 0; index < output_rows_.size(); ++index) {
        const auto base = childName("materialOutputs.") + QString::number(index);
        auto &row = *output_rows_[index];
        row.name->setObjectName(base + QStringLiteral(".name"));
        row.type->setObjectName(base + QStringLiteral(".type"));
        row.source->setObjectName(base + QStringLiteral(".source"));
        row.blend->setObjectName(base + QStringLiteral(".blend"));
        row.write_mask->setObjectName(base + QStringLiteral(".writeMask"));
        row.remove->setObjectName(base + QStringLiteral(".remove"));
    }
    for (std::size_t index = 0; index < resource_rows_.size(); ++index) {
        const auto base = childName("materialResources.") + QString::number(index);
        auto &row = *resource_rows_[index];
        row.kind->setObjectName(base + QStringLiteral(".kind"));
        row.name->setObjectName(base + QStringLiteral(".name"));
        row.resource->setObjectName(base + QStringLiteral(".resource"));
        row.remove->setObjectName(base + QStringLiteral(".remove"));
    }
}

Json MaterialSettingsForm::settings() const {
    Json result = Json::object();
    if (const auto contract = passform::optionalValue(*contract_))
        result["material_contract"] = *contract;

    Json filter = objectOr(original_, "material_filter");
    for (const auto &[key, field] :
         {std::pair{"include", filter_include_}, std::pair{"exclude", filter_exclude_}}) {
        const auto names = passform::splitNames(field->text());
        if (names.empty())
            filter.erase(key);
        else
            filter[key] = names;
    }
    if (!filter.empty()) result["material_filter"] = std::move(filter);

    const auto variant = variant_->currentText().trimmed();
    if (!variant.isEmpty()) result["material_variant"] = variant.toStdString();

    if (range_->isChecked()) {
        Json range = objectOr(original_, "material_range");
        range["start"] = range_start_->value();
        range["count"] = range_count_->value();
        result["material_range"] = std::move(range);
    }

    if (!output_rows_.empty()) {
        Json outputs = objectOr(original_, "material_outputs");
        if (!outputs.contains("schema")) outputs["schema"] = "pelican.material_outputs";
        if (!outputs.contains("version")) outputs["version"] = 1;
        outputs["name"] = outputs_name_->text().trimmed().toStdString();
        Json entries = Json::array();
        Json states = objectOr(original_, "material_output_states");
        const Json original_states = states;
        for (const auto &row : output_rows_)
            if (!row->loaded_name.empty()) states.erase(row->loaded_name);
        for (const auto &row : output_rows_) {
            Json entry = row->original;
            const auto name = row->name->text().trimmed().toStdString();
            entry["name"] = name;
            entry["type"] = row->type->currentData().toString().toStdString();
            entry["source"] = row->source->currentData().toString().toStdString();
            entries.push_back(std::move(entry));
            Json state = row->loaded_name.empty()
                             ? Json::object()
                             : objectOr(original_states, row->loaded_name.c_str());
            putOptional(state, "blend", passform::optionalValue(*row->blend));
            putOptional(state, "write_mask", passform::optionalValue(*row->write_mask));
            if (!state.empty()) states[name] = std::move(state);
        }
        outputs["outputs"] = std::move(entries);
        result["material_outputs"] = std::move(outputs);
        if (!states.empty()) result["material_output_states"] = std::move(states);
    }

    for (const auto *kind : resource_kinds) {
        Json bindings = Json::object();
        bool any = false;
        for (const auto &row : resource_rows_) {
            if (row->kind->currentData().toString() != QString::fromLatin1(kind)) continue;
            const auto name = row->name->currentText().trimmed().toStdString();
            const auto reads = row->resource->currentText().trimmed().toStdString();
            Json value = row->original.is_object() ? row->original : Json(reads);
            if (value.is_object() && !reads.empty()) value["resource"] = reads;
            bindings[name] = std::move(value);
            any = true;
        }
        if (any) result[kind] = std::move(bindings);
    }

    const auto other_text = other_->toPlainText().trimmed();
    if (!other_text.isEmpty()) {
        const Json other = Json::parse(other_text.toStdString());
        if (!other.is_object())
            throw std::runtime_error("Material settings Other must be a JSON object.");
        for (const auto &[key, value] : other.items()) {
            if (std::find_if(other_keys.begin(), other_keys.end(),
                             [&](const char *known) { return key == known; }) ==
                other_keys.end())
                throw std::runtime_error("Unknown material setting: " + key);
            if (!value.is_null()) result[key] = value;
        }
    }
    return result;
}

} // namespace PelicanStudio
