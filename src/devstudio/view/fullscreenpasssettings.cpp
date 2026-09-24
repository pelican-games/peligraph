#include "fullscreenpasssettings.hpp"

#include <nlohmann/json.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStringList>

#include <algorithm>
#include <cctype>
#include <array>
#include <optional>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(),
                             static_cast<qsizetype>(value.size()));
}

// The engine's spellings (renderingpassjsonhelpers.cpp), folded to one each so
// "Clear" and "clear" show as the same choice.
std::string canonicalOp(const Json &entry, const char *key) {
    if (!entry.is_object() || !entry.contains(key) ||
        !entry.at(key).is_string())
        return {};
    auto value = entry.at(key).get<std::string>();
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (value == "dontcare") value = "dont_care";
    return value;
}

std::string entryTarget(const Json &entry) {
    if (entry.is_string()) return entry.get<std::string>();
    if (entry.is_object() && entry.contains("target") &&
        entry.at("target").is_string())
        return entry.at("target").get<std::string>();
    return {};
}

// The colour outputs as the JSON field has them, when it has them for exactly
// these outputs in this order; otherwise nothing.
std::optional<Json> matchingEntries(const std::vector<std::string> &colors,
                                    const QString &json_text) {
    const auto trimmed = json_text.trimmed();
    if (trimmed.isEmpty()) {
        Json entries = Json::array();
        for (const auto &color : colors) entries.push_back(color);
        return entries;
    }
    const Json parsed =
        Json::parse(trimmed.toStdString(), nullptr, false);
    if (parsed.is_discarded()) return std::nullopt;
    const Json entries = parsed.is_array() ? parsed : Json::array({parsed});
    if (entries.size() != colors.size()) return std::nullopt;
    for (std::size_t index = 0; index < colors.size(); ++index)
        if (entryTarget(entries.at(index)) != colors[index])
            return std::nullopt;
    return entries;
}

// An attachment object with nothing but its target is the target's name.
QString encodeEntries(Json entries) {
    bool all_names = true;
    for (auto &entry : entries) {
        if (entry.is_object() && entry.size() == 1 && entry.contains("target"))
            entry = entry.at("target");
        all_names = all_names && entry.is_string();
    }
    return all_names ? QString{} : text(entries.dump());
}

constexpr std::array load_choices{
    std::pair{"Default", ""}, std::pair{"Load", "load"},
    std::pair{"Clear", "clear"}, std::pair{"Don't care", "dont_care"}};
constexpr std::array store_choices{std::pair{"Default", ""},
                                   std::pair{"Store", "store"},
                                   std::pair{"Don't care", "dont_care"}};

template <std::size_t Size>
void addChoices(QComboBox &combo,
                const std::array<std::pair<const char *, const char *>, Size>
                    &choices) {
    for (const auto &[label, value] : choices)
        combo.addItem(QString::fromLatin1(label), QString::fromLatin1(value));
}

// Selects the entry whose value is `value`; a spelling the list does not
// have is added, so what the JSON says is what the row shows.
void selectValue(QComboBox &combo, const std::string &value) {
    int found = combo.findData(text(value));
    if (found < 0) {
        combo.addItem(text(value), text(value));
        found = combo.count() - 1;
    }
    combo.setCurrentIndex(found);
}

} // namespace

// --- FullscreenPassInterfaceRow

FullscreenPassInterfaceRow::FullscreenPassInterfaceRow(QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("pelican.fullscreenPass.interface"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    uses_light_data_ = new QCheckBox(tr("Uses scene lights"), this);
    uses_light_data_->setObjectName(
        QStringLiteral("pelican.fullscreenPass.usesLightData"));
    uses_light_data_->setToolTip(
        tr("uses_light_data: the pass reads the scene lights "
           "(pelicanLights, pelican_lighting_v1.glsl)."));
    push_constants_ = new QComboBox(this);
    push_constants_->setObjectName(
        QStringLiteral("pelican.fullscreenPass.pushConstants"));
    push_constants_->addItem(tr("No push constants"), QString{});
    push_constants_->addItem(tr("Camera position"),
                             QStringLiteral("camera_position"));
    push_constants_->addItem(tr("Projection and view"),
                             QStringLiteral("projection_view"));
    push_constants_->setToolTip(
        tr("push_constants: what the engine declares for the pass. A "
           "lighting pass asks for the camera position."));
    layout->addWidget(uses_light_data_);
    layout->addWidget(push_constants_, 1);
    QObject::connect(uses_light_data_, &QCheckBox::toggled, this, [this] {
        if (changed_) changed_();
    });
    QObject::connect(push_constants_,
                     qOverload<int>(&QComboBox::currentIndexChanged), this,
                     [this] {
                         if (changed_) changed_();
                     });
}

void FullscreenPassInterfaceRow::setChangedAction(
    std::function<void()> action) {
    changed_ = std::move(action);
}

void FullscreenPassInterfaceRow::reset() {
    const QSignalBlocker lights{uses_light_data_};
    const QSignalBlocker push{push_constants_};
    uses_light_data_->setChecked(false);
    while (push_constants_->count() > 3) push_constants_->removeItem(3);
    push_constants_->setCurrentIndex(0);
}

void FullscreenPassInterfaceRow::load(const Json &declaration) {
    reset();
    const QSignalBlocker lights{uses_light_data_};
    const QSignalBlocker push{push_constants_};
    const auto uses = declaration.find("uses_light_data");
    uses_light_data_->setChecked(uses != declaration.end() &&
                                 uses->is_boolean() && uses->get<bool>());
    const auto push_value = declaration.find("push_constants");
    if (push_value != declaration.end() && push_value->is_string() &&
        push_value->get<std::string>() != "none")
        selectValue(*push_constants_, push_value->get<std::string>());
}

void FullscreenPassInterfaceRow::require(bool uses_light_data,
                                         std::string_view push_constants) {
    if (uses_light_data) uses_light_data_->setChecked(true);
    if (!push_constants.empty())
        selectValue(*push_constants_, std::string{push_constants});
}

bool FullscreenPassInterfaceRow::usesLightData() const {
    return uses_light_data_->isChecked();
}

std::string FullscreenPassInterfaceRow::pushConstants() const {
    return push_constants_->currentData().toString().toStdString();
}

void FullscreenPassInterfaceRow::apply(Json &draft) const {
    if (usesLightData())
        draft["uses_light_data"] = true;
    else
        draft.erase("uses_light_data");
    const auto push = pushConstants();
    if (push.empty())
        draft.erase("push_constants");
    else
        draft["push_constants"] = push;
}

void FullscreenPassInterfaceRow::addEditParams(const Json &original,
                                               Json &params) const {
    const auto uses = original.find("uses_light_data");
    const bool was_using =
        uses != original.end() && uses->is_boolean() && uses->get<bool>();
    if (was_using != usesLightData())
        params["uses_light_data"] = usesLightData() ? Json(true) : Json(nullptr);
    auto was_pushing = original.value("push_constants", std::string{});
    if (was_pushing == "none") was_pushing.clear();
    if (was_pushing != pushConstants())
        params["push_constants"] =
            pushConstants().empty() ? Json(nullptr) : Json(pushConstants());
}

// --- FullscreenOutputOpsEditor

struct FullscreenOutputOpsEditor::Row {
    QLabel *target = nullptr;
    QComboBox *load = nullptr;
    QLineEdit *clear_color = nullptr;
    QComboBox *store = nullptr;
};

FullscreenOutputOpsEditor::FullscreenOutputOpsEditor(QWidget *parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("pelican.fullscreenPass.outputOps"));
    setToolTip(tr("Load, clear and store of each colour output. They are "
                  "written into the output.color JSON; the clear colour is "
                  "the pass's clear_colors."));
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    hide();
}

FullscreenOutputOpsEditor::~FullscreenOutputOpsEditor() = default;

void FullscreenOutputOpsEditor::setJsonWriter(
    std::function<void(const QString &)> writer) {
    write_json_ = std::move(writer);
}

void FullscreenOutputOpsEditor::setChangedAction(
    std::function<void()> action) {
    changed_ = std::move(action);
}

void FullscreenOutputOpsEditor::reset() {
    clear_colors_.clear();
    rebuild({});
}

void FullscreenOutputOpsEditor::rebuild(
    const std::vector<std::string> &targets) {
    for (auto &row : rows_) {
        delete row.target;
        delete row.load;
        delete row.clear_color;
        delete row.store;
    }
    rows_.clear();
    targets_ = targets;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const QString number = QString::number(index);
        Row row;
        row.target = new QLabel(text(targets[index]), this);
        row.load = new QComboBox(this);
        row.load->setObjectName(
            QStringLiteral("pelican.fullscreenPass.outputLoad.") + number);
        addChoices(*row.load, load_choices);
        row.load->setToolTip(tr("load_op of %1").arg(text(targets[index])));
        row.clear_color = new QLineEdit(this);
        row.clear_color->setObjectName(
            QStringLiteral("pelican.fullscreenPass.outputClearColor.") +
            number);
        row.clear_color->setPlaceholderText(QStringLiteral("0, 0, 0, 1"));
        row.clear_color->setToolTip(
            tr("Clear colour of %1: r, g, b[, a] (clear_colors)")
                .arg(text(targets[index])));
        row.store = new QComboBox(this);
        row.store->setObjectName(
            QStringLiteral("pelican.fullscreenPass.outputStore.") + number);
        addChoices(*row.store, store_choices);
        row.store->setToolTip(tr("store_op of %1").arg(text(targets[index])));
        const int line = static_cast<int>(index);
        grid_->addWidget(row.target, line, 0);
        grid_->addWidget(row.load, line, 1);
        grid_->addWidget(row.clear_color, line, 2);
        grid_->addWidget(row.store, line, 3);
        QObject::connect(row.load,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this, index] { rowChanged(index); });
        QObject::connect(row.store,
                         qOverload<int>(&QComboBox::currentIndexChanged), this,
                         [this, index] { rowChanged(index); });
        QObject::connect(row.clear_color, &QLineEdit::textChanged, this,
                         [this, index](const QString &value) {
                             if (syncing_) return;
                             clear_colors_[targets_[index]] = value;
                             if (changed_) changed_();
                         });
        rows_.push_back(row);
    }
}

void FullscreenOutputOpsEditor::showClearColor(std::size_t index) {
    auto &row = rows_[index];
    const auto load = row.load->currentData().toString();
    // A colour typed for a Default load stays visible: the pass may clear
    // through its own color_load_op.
    row.clear_color->setVisible(
        load == QStringLiteral("clear") ||
        (load.isEmpty() && !row.clear_color->text().trimmed().isEmpty()));
}

void FullscreenOutputOpsEditor::sync(const std::vector<std::string> &colors,
                                     const QString &json_text) {
    syncing_ = true;
    json_text_ = json_text;
    if (colors != targets_) rebuild(colors);
    const auto entries = matchingEntries(colors, json_text);
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        auto &row = rows_[index];
        const QSignalBlocker load{row.load};
        const QSignalBlocker store{row.store};
        const QSignalBlocker clear{row.clear_color};
        row.load->setEnabled(entries.has_value());
        row.store->setEnabled(entries.has_value());
        if (entries) {
            selectValue(*row.load, canonicalOp(entries->at(index), "load_op"));
            selectValue(*row.store,
                        canonicalOp(entries->at(index), "store_op"));
        }
        const auto found = clear_colors_.find(targets_[index]);
        const QString clear_text =
            found == clear_colors_.end() ? QString{} : found->second;
        // Not while it is being typed in: setText would move the cursor.
        if (row.clear_color->text() != clear_text)
            row.clear_color->setText(clear_text);
        showClearColor(index);
    }
    setVisible(!rows_.empty());
    syncing_ = false;
}

void FullscreenOutputOpsEditor::rowChanged(std::size_t index) {
    if (syncing_ || !write_json_) return;
    showClearColor(index);
    // The rows are enabled only while the JSON field matches the outputs.
    auto entries = matchingEntries(targets_, json_text_);
    if (!entries) return;
    auto &entry = entries->at(index);
    if (entry.is_string()) entry = Json{{"target", entry}};
    for (const auto &[key, combo] : {std::pair{"load_op", rows_[index].load},
                                     std::pair{"store_op", rows_[index].store}}) {
        const auto value = combo->currentData().toString().toStdString();
        if (value.empty())
            entry.erase(key);
        else
            entry[key] = value;
    }
    write_json_(encodeEntries(std::move(*entries)));
}

void FullscreenOutputOpsEditor::loadClearColors(const Json &declaration) {
    clear_colors_.clear();
    const auto found = declaration.find("clear_colors");
    if (found == declaration.end() || !found->is_object()) return;
    for (const auto &[target, value] : found->items()) {
        if (!value.is_array()) continue;
        QStringList parts;
        for (const auto &component : value)
            parts.push_back(text(component.dump()));
        clear_colors_[target] = parts.join(QStringLiteral(", "));
    }
}

Json FullscreenOutputOpsEditor::clearColors() const {
    Json result = Json::object();
    for (const auto &target : targets_) {
        const auto found = clear_colors_.find(target);
        if (found == clear_colors_.end() || found->second.trimmed().isEmpty())
            continue;
        Json color = Json::array();
        for (const auto &part :
             found->second.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            bool number = false;
            const double value = part.trimmed().toDouble(&number);
            if (!number)
                throw std::runtime_error("clear colour of " + target +
                                         " must be numbers: r, g, b[, a]");
            color.push_back(value);
        }
        if (color.size() == 3) color.push_back(1.0);
        if (color.size() != 4)
            throw std::runtime_error("clear colour of " + target +
                                     " must be three or four numbers");
        result[target] = std::move(color);
    }
    return result.empty() ? Json(nullptr) : result;
}

QString FullscreenOutputOpsEditor::reconciledJson(
    const std::vector<std::string> &colors, const QString &json_text) {
    const auto trimmed = json_text.trimmed();
    if (trimmed.isEmpty()) return {};
    const Json parsed = Json::parse(trimmed.toStdString(), nullptr, false);
    if (parsed.is_discarded()) return json_text;
    Json old_entries = parsed.is_array() ? parsed : Json::array({parsed});
    std::vector<bool> used(old_entries.size(), false);
    Json entries = Json::array();
    for (const auto &color : colors) {
        Json entry = color;
        for (std::size_t index = 0; index < old_entries.size(); ++index) {
            if (!used[index] && entryTarget(old_entries[index]) == color) {
                used[index] = true;
                entry = old_entries[index];
                break;
            }
        }
        entries.push_back(std::move(entry));
    }
    return encodeEntries(std::move(entries));
}

} // namespace PelicanStudio
