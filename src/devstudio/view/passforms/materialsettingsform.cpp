#include "materialsettingsform.hpp"

#include "materialoutput.hpp"
// UX-11, decision 2: the Tags are the Graph's tag picker.
#include "../graphcanvastagpicker.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
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
// UX-15a, decision 6: gpu_draw_source's layout that draws several materials
// (materialpassinfojsonparser.cpp; docs/manual/06_rendering.md).
constexpr const char *gpu_segments_layout = "draw_queue_segments_v1";

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

// The names a binding of each kind can have, for an engine that does not list
// them (before MP-1): its screen input contracts (materialscreeninput.cpp)
// and its feature-owned surface ones. A material resource is the shader's own
// port name.
std::vector<std::string> fixedResourceNamesOf(std::string_view kind) {
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

// A tag list's names; what is not a string is no tag.
std::vector<std::string> namesOf(const Json &names) {
    std::vector<std::string> result;
    if (names.is_array())
        for (const auto &name : names)
            if (name.is_string()) result.push_back(name.get<std::string>());
    return result;
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
    form_ = form;
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
    // UX-11, decision 2: picked among the engine's tags (the Loop's picker),
    // a new one typed in the menu's "+ new tag" line.
    filter_include_ = new GraphCanvasTagPicker(childName("materialFilter.include"),
                                               tr("any tag (every material)"), filter_row);
    filter_include_->setToolTip(tr("material_filter.include: materials with any "
                                   "of these tags; none picked: every material"));
    filter_exclude_ = new GraphCanvasTagPicker(childName("materialFilter.exclude"),
                                               tr("leave none out"), filter_row);
    filter_exclude_->setToolTip(tr("material_filter.exclude: leave out materials "
                                   "with any of these tags"));
    filter_layout->addWidget(filter_include_, 1);
    filter_layout->addWidget(filter_exclude_, 1);
    for (auto *field : {filter_include_, filter_exclude_}) {
        field->allowNewTags(true);
        field->changed = changed;
    }
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
        // UX-12c: the engine's first screen input, opaque_color before MP-1.
        const auto names = resourceNamesOf("screen_inputs");
        addResourceRow("screen_inputs", names.empty() ? std::string{"opaque_color"} : names.front(),
                       Json(nullptr));
        notifyChanged();
    });
    resources_layout->addWidget(add_resource_, 0, Qt::AlignLeft);
    form->addRow(tr("Resources"), resources_box_);

    // UX-15a, decision 6: gpu_draw_source, a field each.
    auto *gpu_row = new QWidget(this);
    auto *gpu_row_layout = new QVBoxLayout(gpu_row);
    gpu_row_layout->setContentsMargins(0, 0, 0, 0);
    gpu_draw_ = new QCheckBox(tr("Draw from GPU-written commands"), gpu_row);
    gpu_draw_->setObjectName(childName("gpuDrawSource"));
    gpu_draw_->setToolTip(tr("gpu_draw_source: a compute task writes this pass's indexed draw "
                             "commands and their count, and the GPU draws that many"));
    gpu_row_layout->addWidget(gpu_draw_);
    gpu_box_ = new QWidget(gpu_row);
    gpu_box_->setObjectName(childName("gpuDrawSource.fields"));
    auto *gpu_form = new QFormLayout(gpu_box_);
    gpu_form->setContentsMargins(0, 0, 0, 0);
    gpu_layout_ = new QComboBox(gpu_box_);
    gpu_layout_->setObjectName(childName("gpuDrawSource.layout"));
    passform::fillOptionalChoices(*gpu_layout_, tr("Default (fixed_state_v1)"),
                                  {"fixed_state_v1", gpu_segments_layout});
    gpu_layout_->setToolTip(tr("gpu_draw_source.layout: fixed_state_v1 draws one material "
                               "(material_range.count 1); draw_queue_segments_v1 draws several, "
                               "a segment each"));
    gpu_form->addRow(tr("Layout"), gpu_layout_);
    const auto buffer = [this](const char *name, const QString &tip) {
        auto *combo = new QComboBox(gpu_box_);
        combo->setObjectName(childName(name));
        combo->setToolTip(tip);
        return combo;
    };
    gpu_commands_ = buffer("gpuDrawSource.commands",
                           tr("gpu_draw_source.commands: the buffer of indexed draw commands "
                              "(command_layout indexed_draw)"));
    gpu_form->addRow(tr("Commands"), gpu_commands_);
    gpu_count_ = buffer("gpuDrawSource.count",
                        tr("gpu_draw_source.count: the buffer of the draw count "
                           "(command_layout draw_count)"));
    gpu_form->addRow(tr("Count"), gpu_count_);
    gpu_segments_ = buffer("gpuDrawSource.segments",
                           tr("gpu_draw_source.segments: the buffer of segments "
                              "(host_source scene_draw_segments_v1); draw_queue_segments_v1 only"));
    gpu_form->addRow(tr("Segments"), gpu_segments_);
    gpu_max_draws_ = new QSpinBox(gpu_box_);
    gpu_max_draws_->setObjectName(childName("gpuDrawSource.maxDrawCount"));
    gpu_max_draws_->setRange(1, std::numeric_limits<int>::max());
    gpu_max_draws_->setToolTip(tr("gpu_draw_source.max_draw_count: the most commands drawn "
                                  "(with segments: the output command slots in all)"));
    gpu_form->addRow(tr("Max draws"), gpu_max_draws_);
    auto *offsets = new QWidget(gpu_box_);
    auto *offsets_layout = new QHBoxLayout(offsets);
    offsets_layout->setContentsMargins(0, 0, 0, 0);
    const auto offset = [this, offsets, offsets_layout](const char *name, const QString &prefix,
                                                        const QString &tip) {
        auto *spin = new QSpinBox(offsets);
        spin->setObjectName(childName(name));
        spin->setRange(0, std::numeric_limits<int>::max());
        spin->setSingleStep(4);
        spin->setPrefix(prefix);
        spin->setSuffix(tr(" B"));
        spin->setToolTip(tip);
        offsets_layout->addWidget(spin);
        return spin;
    };
    gpu_command_offset_ = offset("gpuDrawSource.commandOffset", tr("commands at "),
                                 tr("gpu_draw_source.command_offset: bytes into the commands "
                                    "buffer (a multiple of 4)"));
    gpu_count_offset_ = offset("gpuDrawSource.countOffset", tr("count at "),
                               tr("gpu_draw_source.count_offset: bytes into the count buffer "
                                  "(a multiple of 4)"));
    gpu_form->addRow(tr("Offsets"), offsets);
    gpu_fallback_ = new QComboBox(gpu_box_);
    gpu_fallback_->setObjectName(childName("gpuDrawSource.fallback"));
    passform::fillOptionalChoices(*gpu_fallback_, tr("Default (none)"), {"cpu_draw_queue"});
    gpu_fallback_->setToolTip(tr("gpu_draw_source.fallback: cpu_draw_queue draws the CPU's draw "
                                 "queue on a device without drawIndirectCount"));
    gpu_form->addRow(tr("Fallback"), gpu_fallback_);
    gpu_execution_ = new QComboBox(gpu_box_);
    gpu_execution_->setObjectName(childName("gpuDrawSource.execution"));
    passform::fillOptionalChoices(*gpu_execution_, tr("Default (automatic)"), {"automatic", "cpu"});
    gpu_execution_->setToolTip(tr("gpu_draw_source.execution: automatic draws from the GPU's "
                                  "commands where the device can; cpu takes the fallback always"));
    gpu_form->addRow(tr("Execution"), gpu_execution_);
    gpu_row_layout->addWidget(gpu_box_);
    form->addRow(tr("GPU draws"), gpu_row);
    QObject::connect(gpu_draw_, &QCheckBox::toggled, this, [this](bool on) {
        // Turned on: the buffers picked by their names, so nothing is typed.
        if (on && !loading()) {
            std::string count;
            for (const auto &name : buffer_candidates_)
                if (name.find("count") != std::string::npos) {
                    count = name;
                    break;
                }
            std::string commands;
            for (const auto &name : buffer_candidates_)
                if (name != count && name.find("segment") == std::string::npos) {
                    commands = name;
                    break;
                }
            if (count.empty())
                for (const auto &name : buffer_candidates_)
                    if (name != commands) {
                        count = name;
                        break;
                    }
            if (gpu_commands_->currentIndex() < 0) fillBufferChoices(*gpu_commands_, commands);
            if (gpu_count_->currentIndex() < 0) fillBufferChoices(*gpu_count_, count);
        }
        followGpuDraw();
        notifyChanged();
    });
    QObject::connect(gpu_layout_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        // Segments: the buffer whose name says so, when none is picked.
        if (!loading() && gpu_segments_->currentIndex() < 0)
            for (const auto &name : buffer_candidates_)
                if (name.find("segment") != std::string::npos) {
                    fillBufferChoices(*gpu_segments_, name);
                    break;
                }
        followGpuDraw();
        notifyChanged();
    });
    for (auto *combo : {gpu_commands_, gpu_count_, gpu_segments_, gpu_fallback_, gpu_execution_})
        QObject::connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, changed);
    for (auto *spin : {gpu_max_draws_, gpu_command_offset_, gpu_count_offset_})
        QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, changed);
    reset();
}

MaterialSettingsForm::~MaterialSettingsForm() = default;

void MaterialSettingsForm::reset() { load(Json::object()); }

void MaterialSettingsForm::setResourceCandidates(const std::vector<std::string> &resources) {
    resource_candidates_ = resources;
    for (auto &row : resource_rows_) fillResourceReads(*row);
}

void MaterialSettingsForm::setBufferCandidates(const std::vector<std::string> &buffers) {
    if (buffers == buffer_candidates_) return;
    buffer_candidates_ = buffers;
    const Loading loading{*this};
    for (auto *combo : {gpu_commands_, gpu_count_, gpu_segments_})
        fillBufferChoices(*combo, combo->currentData().toString().toStdString());
}

// The buffers given, and `current` when they do not have it; `current`
// picked (none when it is empty).
void MaterialSettingsForm::fillBufferChoices(QComboBox &combo, const std::string &current) {
    const QSignalBlocker blocker{&combo};
    combo.clear();
    for (const auto &name : buffer_candidates_) combo.addItem(text(name), text(name));
    if (!current.empty() && combo.findData(text(current)) < 0)
        combo.addItem(text(current), text(current));
    combo.setCurrentIndex(current.empty() ? -1 : combo.findData(text(current)));
}

void MaterialSettingsForm::followGpuDraw() {
    gpu_box_->setVisible(gpu_draw_->isChecked());
    gpu_segments_->setEnabled(gpu_layout_->currentData().toString() ==
                              QString::fromLatin1(gpu_segments_layout));
}

void MaterialSettingsForm::loadGpuDraw(const Json &source) {
    const bool on = source.is_object();
    gpu_draw_->setChecked(on);
    const auto name = [&](const char *key) {
        return on && source.contains(key) && source.at(key).is_string()
                   ? source.at(key).get<std::string>()
                   : std::string{};
    };
    fillBufferChoices(*gpu_commands_, name("commands"));
    fillBufferChoices(*gpu_count_, name("count"));
    fillBufferChoices(*gpu_segments_, name("segments"));
    const auto number = [&](const char *key, int fallback) {
        if (!on || !source.contains(key) || !source.at(key).is_number_integer()) return fallback;
        const auto value = source.at(key).get<long long>();
        return static_cast<int>(
            std::clamp<long long>(value, 0, std::numeric_limits<int>::max()));
    };
    gpu_max_draws_->setValue(number("max_draw_count", 1));
    gpu_command_offset_->setValue(number("command_offset", 0));
    gpu_count_offset_->setValue(number("count_offset", 0));
    const auto optional = [&](QComboBox &combo, const char *key) {
        const Json *value = nullptr;
        if (on)
            if (const auto found = source.find(key); found != source.end()) value = &*found;
        passform::showOptionalValue(combo, value);
    };
    optional(*gpu_layout_, "layout");
    optional(*gpu_fallback_, "fallback");
    optional(*gpu_execution_, "execution");
    followGpuDraw();
}

// gpu_draw_source as the fields say; nullopt when GPU draws are off. What
// the pass held that the fields do not show stays.
std::optional<Json> MaterialSettingsForm::gpuDrawSource() const {
    if (!gpu_draw_->isChecked()) return std::nullopt;
    const Json original = objectOr(original_, "gpu_draw_source");
    Json source = original;
    const auto commands = gpu_commands_->currentData().toString().toStdString();
    const auto count = gpu_count_->currentData().toString().toStdString();
    if (commands.empty())
        throw std::runtime_error("GPU draws need the buffer of their commands.");
    if (count.empty()) throw std::runtime_error("GPU draws need the buffer of their count.");
    if (commands == count)
        throw std::runtime_error("GPU draws read their commands and count from two buffers.");
    source["commands"] = commands;
    source["count"] = count;
    source["max_draw_count"] = gpu_max_draws_->value();
    for (const auto &[key, spin] : {std::pair{"command_offset", gpu_command_offset_},
                                    std::pair{"count_offset", gpu_count_offset_}}) {
        if (spin->value() != 0 || original.contains(key))
            source[key] = spin->value();
        else
            source.erase(key);
    }
    putOptional(source, "layout", passform::optionalValue(*gpu_layout_));
    if (gpu_layout_->currentData().toString() == QString::fromLatin1(gpu_segments_layout)) {
        const auto segments = gpu_segments_->currentData().toString().toStdString();
        if (segments.empty())
            throw std::runtime_error("GPU draws in segments need the buffer of the segments.");
        source["segments"] = segments;
    } else {
        source.erase("segments");
    }
    putOptional(source, "fallback", passform::optionalValue(*gpu_fallback_));
    putOptional(source, "execution", passform::optionalValue(*gpu_execution_));
    return source;
}

void MaterialSettingsForm::setResourceNameCandidates(
    const std::string &kind, std::vector<std::pair<std::string, std::string>> names) {
    auto &held = name_candidates_[kind];
    if (held == names) return;
    held = std::move(names);
    for (auto &row : resource_rows_) {
        fillResourceNames(*row);
        fillResourceReads(*row);
    }
}

void MaterialSettingsForm::setResourceCandidateTypes(std::map<std::string, std::string> types) {
    if (types == candidate_types_) return;
    candidate_types_ = std::move(types);
    for (auto &row : resource_rows_) fillResourceReads(*row);
}

std::vector<std::string> MaterialSettingsForm::resourceNamesOf(const std::string &kind) const {
    if (const auto found = name_candidates_.find(kind);
        found != name_candidates_.end() && !found->second.empty()) {
        std::vector<std::string> names;
        for (const auto &[name, type] : found->second) names.push_back(name);
        return names;
    }
    return fixedResourceNamesOf(kind);
}

void MaterialSettingsForm::setVariantCandidates(const std::vector<std::string> &variants) {
    const QSignalBlocker blocker{variant_};
    const QString current = variant_->currentText();
    variant_->clear();
    for (const auto &variant : variants) variant_->addItem(text(variant));
    variant_->setEditText(current);
}

void MaterialSettingsForm::setTagCandidates(const std::vector<std::string> &tags) {
    if (tags == tag_candidates_) return;
    tag_candidates_ = tags;
    const Loading loading{*this};
    for (auto *picker : {filter_include_, filter_exclude_}) picker->setTags(tags, picker->selected());
}

void MaterialSettingsForm::setContractAndVariantShown(bool shown) {
    form_->setRowVisible(contract_, shown);
    form_->setRowVisible(variant_, shown);
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
    filter_include_->setTags(tag_candidates_, namesOf(filter.value("include", Json::array())));
    filter_exclude_->setTags(tag_candidates_, namesOf(filter.value("exclude", Json::array())));
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

    loadGpuDraw(original_.value("gpu_draw_source", Json(nullptr)));
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
    const auto kind = row.kind->currentData().toString().toStdString();
    const auto listed = name_candidates_.find(kind);
    for (const auto &name : resourceNamesOf(kind)) {
        row.name->addItem(text(name));
        // UX-12c: what the engine says the name reads.
        if (listed != name_candidates_.end())
            for (const auto &[each, type] : listed->second)
                if (each == name && !type.empty())
                    row.name->setItemData(row.name->count() - 1,
                                          tr("reads a %1").arg(text(type)), Qt::ToolTipRole);
    }
    row.name->setEditText(current);
}

// UX-12c, decision 3: the resources a row offers to read are those whose type
// is the one its name wants (or not known); what the row reads stays.
void MaterialSettingsForm::fillResourceReads(ResourceRow &row) {
    std::string wanted;
    if (const auto listed = name_candidates_.find(row.kind->currentData().toString().toStdString());
        listed != name_candidates_.end()) {
        const auto name = row.name->currentText().trimmed().toStdString();
        for (const auto &[each, type] : listed->second)
            if (each == name) wanted = type;
    }
    const QSignalBlocker blocker{row.resource};
    const QString current = row.resource->currentText();
    row.resource->clear();
    for (const auto &resource : resource_candidates_) {
        if (!wanted.empty())
            if (const auto type = candidate_types_.find(resource);
                type != candidate_types_.end() && !type->second.empty() && type->second != wanted)
                continue;
        row.resource->addItem(text(resource));
    }
    row.resource->setEditText(current);
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
    fillResourceReads(*row);
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
                         fillResourceReads(*raw);
                         notifyChanged();
                     });
    QObject::connect(row->name, &QComboBox::currentTextChanged, this, [this, raw = row.get()] {
        fillResourceReads(*raw);
        notifyChanged();
    });
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
        const auto names = field->selected();
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

    if (auto source = gpuDrawSource()) result["gpu_draw_source"] = std::move(*source);
    return result;
}

} // namespace PelicanStudio
