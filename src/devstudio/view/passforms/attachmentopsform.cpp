#include "attachmentopsform.hpp"

#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStringList>
#include <QToolButton>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {

using passform::Json;
using passform::text;

namespace {

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

// An attachment object with nothing but its target is the target's name.
Json bareWhenPlain(Json entry) {
    if (entry.is_object() && entry.size() == 1 && entry.contains("target"))
        return entry.at("target");
    return entry;
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
        combo.addItem(QObject::tr(label), QString::fromLatin1(value));
}

// Writes one combo's choice into an attachment entry (the other key keeps its
// spelling); a Default removes the key, and the entry is a bare name again
// when nothing else is left.
Json withOp(Json entry, const char *key, const QComboBox &combo) {
    if (entry.is_string()) entry = Json{{"target", entry}};
    const auto value = combo.currentData().toString().toStdString();
    if (value.empty())
        entry.erase(key);
    else
        entry[key] = value;
    return bareWhenPlain(std::move(entry));
}

void showOps(QComboBox &load, QComboBox &store, const Json &entry) {
    const QSignalBlocker load_blocker{&load};
    const QSignalBlocker store_blocker{&store};
    passform::selectData(load, text(canonicalOp(entry, "load_op")));
    passform::selectData(store, text(canonicalOp(entry, "store_op")));
}

} // namespace

struct AttachmentOpsForm::Row {
    QLabel *target = nullptr;
    QComboBox *load = nullptr;
    QLineEdit *clear_color = nullptr;
    // T-1: opens a colour picker for the field beside it.
    QToolButton *pick_color = nullptr;
    QComboBox *store = nullptr;
};

QString AttachmentOpsForm::clearColorText(double red, double green, double blue,
                                          double alpha) {
    QStringList parts;
    for (const double value : {red, green, blue, alpha})
        parts.push_back(QString::number(std::round(value * 1000.0) / 1000.0, 'g', 4));
    return parts.join(QStringLiteral(", "));
}

std::array<double, 4> AttachmentOpsForm::clearColorComponents(const QString &text) {
    std::array<double, 4> result{0.0, 0.0, 0.0, 1.0};
    const auto parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (parts.size() != 3 && parts.size() != 4) return result;
    std::array<double, 4> read{0.0, 0.0, 0.0, 1.0};
    for (qsizetype index = 0; index < parts.size(); ++index) {
        bool number = false;
        const double value = parts[index].trimmed().toDouble(&number);
        if (!number) return result;
        read[static_cast<std::size_t>(index)] = std::clamp(value, 0.0, 1.0);
    }
    return read;
}

AttachmentOpsForm::AttachmentOpsForm(const QString &object_prefix, QWidget *parent)
    : PassForm(object_prefix, parent) {
    setObjectName(childName("outputOps"));
    setToolTip(tr("Load, clear and store of each output. The clear colour is "
                  "the pass's clear_colors."));
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    depth_label_ = new QLabel(this);
    depth_load_ = new QComboBox(this);
    depth_load_->setObjectName(childName("depthLoad"));
    addChoices(*depth_load_, load_choices);
    depth_load_->setToolTip(tr("load_op of the depth output"));
    depth_store_ = new QComboBox(this);
    depth_store_->setObjectName(childName("depthStore"));
    addChoices(*depth_store_, store_choices);
    depth_store_->setToolTip(tr("store_op of the depth output"));
    grid_->addWidget(depth_label_, 0, 0);
    grid_->addWidget(depth_load_, 0, 1);
    grid_->addWidget(depth_store_, 0, 4);
    QObject::connect(depth_load_, qOverload<int>(&QComboBox::currentIndexChanged),
                     this, [this] { depthChanged("load_op", *depth_load_); });
    QObject::connect(depth_store_, qOverload<int>(&QComboBox::currentIndexChanged),
                     this, [this] { depthChanged("store_op", *depth_store_); });
    showDepth();
    hide();
}

AttachmentOpsForm::~AttachmentOpsForm() = default;

void AttachmentOpsForm::reset() { load(Json::array(), nullptr, nullptr); }

void AttachmentOpsForm::load(const Json &color, const Json &clear_colors,
                             const Json &depth) {
    const Loading loading{*this};
    single_entry_ = !color.is_null() && !color.is_array();
    entries_ = color.is_null() ? Json::array()
               : color.is_array() ? color
                                  : Json::array({color});
    targets_.clear();
    for (const auto &entry : entries_) targets_.push_back(entryTarget(entry));
    clear_colors_.clear();
    if (clear_colors.is_object())
        for (const auto &[target, value] : clear_colors.items()) {
            if (!value.is_array()) continue;
            QStringList parts;
            for (const auto &component : value) parts.push_back(text(component.dump()));
            clear_colors_[target] = parts.join(QStringLiteral(", "));
        }
    depth_ = depth;
    depth_target_ = entryTarget(depth);
    rebuild();
    showDepth();
}

void AttachmentOpsForm::setColorTargets(const std::vector<std::string> &targets) {
    if (targets == targets_) return;
    std::vector<bool> used(entries_.size(), false);
    Json entries = Json::array();
    for (const auto &target : targets) {
        Json entry = target;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            if (!used[index] && entryTarget(entries_[index]) == target) {
                used[index] = true;
                entry = entries_[index];
                break;
            }
        }
        entries.push_back(std::move(entry));
    }
    entries_ = std::move(entries);
    targets_ = targets;
    const Loading loading{*this};
    rebuild();
}

void AttachmentOpsForm::retargetColor(std::size_t index, const std::string &target) {
    if (index >= targets_.size() || target.empty() || targets_[index] == target) return;
    const auto before = targets_[index];
    auto &entry = entries_[index];
    if (entry.is_object()) entry["target"] = target;
    else entry = target;
    targets_[index] = target;
    if (const auto clear = clear_colors_.find(before); clear != clear_colors_.end()) {
        auto value = clear->second;
        clear_colors_.erase(clear);
        clear_colors_[target] = std::move(value);
    }
    const Loading loading{*this};
    rebuild();
}

void AttachmentOpsForm::setDepthTarget(const std::string &target) {
    depth_target_ = target;
    showDepth();
}

void AttachmentOpsForm::setDepthOpsShown(bool shown) {
    depth_ops_shown_ = shown;
    showDepth();
}

void AttachmentOpsForm::rebuild() {
    for (auto &row : rows_) {
        delete row.target;
        delete row.load;
        delete row.clear_color;
        delete row.pick_color;
        delete row.store;
    }
    rows_.clear();
    for (std::size_t index = 0; index < targets_.size(); ++index) {
        const QString number = QString::number(index);
        const QString target = text(targets_[index]);
        Row row;
        row.target = new QLabel(target, this);
        row.load = new QComboBox(this);
        row.load->setObjectName(childName("outputLoad.") + number);
        addChoices(*row.load, load_choices);
        row.load->setToolTip(tr("load_op of %1").arg(target));
        row.clear_color = new QLineEdit(this);
        row.clear_color->setObjectName(childName("outputClearColor.") + number);
        row.clear_color->setPlaceholderText(QStringLiteral("0, 0, 0, 1"));
        row.clear_color->setToolTip(
            tr("Clear colour of %1: r, g, b[, a] (clear_colors)").arg(target));
        row.pick_color = new QToolButton(this);
        row.pick_color->setObjectName(childName("outputClearColorPick.") + number);
        row.pick_color->setText(tr("Colour..."));
        row.pick_color->setToolTip(
            tr("Pick the clear colour of %1; it is written into the field").arg(target));
        row.store = new QComboBox(this);
        row.store->setObjectName(childName("outputStore.") + number);
        addChoices(*row.store, store_choices);
        row.store->setToolTip(tr("store_op of %1").arg(target));
        const int line = static_cast<int>(index) + 1;
        grid_->addWidget(row.target, line, 0);
        grid_->addWidget(row.load, line, 1);
        grid_->addWidget(row.clear_color, line, 2);
        grid_->addWidget(row.pick_color, line, 3);
        grid_->addWidget(row.store, line, 4);
        QObject::connect(row.pick_color, &QToolButton::clicked, this,
                         [this, index] { pickClearColor(index); });
        QObject::connect(row.load, qOverload<int>(&QComboBox::currentIndexChanged),
                         this, [this, index] { rowChanged(index, "load_op"); });
        QObject::connect(row.store, qOverload<int>(&QComboBox::currentIndexChanged),
                         this, [this, index] { rowChanged(index, "store_op"); });
        QObject::connect(row.clear_color, &QLineEdit::textChanged, this,
                         [this, index](const QString &value) {
                             if (index >= targets_.size()) return;
                             clear_colors_[targets_[index]] = value;
                             notifyChanged();
                         });
        rows_.push_back(row);
    }
    showRows();
}

void AttachmentOpsForm::showRows() {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        auto &row = rows_[index];
        showOps(*row.load, *row.store, entries_.at(index));
        const auto found = clear_colors_.find(targets_[index]);
        const QString clear_text =
            found == clear_colors_.end() ? QString{} : found->second;
        const QSignalBlocker clear{row.clear_color};
        if (row.clear_color->text() != clear_text) row.clear_color->setText(clear_text);
        showClearColor(index);
    }
    setVisible(!rows_.empty() || !depth_label_->isHidden());
}

void AttachmentOpsForm::showClearColor(std::size_t index) {
    auto &row = rows_[index];
    const auto load = row.load->currentData().toString();
    // A colour typed for a Default load stays visible: the pass may clear
    // through its own color_load_op.
    row.clear_color->setVisible(
        load == QStringLiteral("clear") ||
        (load.isEmpty() && !row.clear_color->text().trimmed().isEmpty()));
    row.pick_color->setVisible(!row.clear_color->isHidden());
}

void AttachmentOpsForm::rowChanged(std::size_t index, const char *key) {
    if (loading() || index >= rows_.size()) return;
    const auto &row = rows_[index];
    entries_[index] = withOp(entries_[index], key,
                             std::string_view{key} == "load_op" ? *row.load : *row.store);
    showClearColor(index);
    notifyChanged();
}

void AttachmentOpsForm::showDepth() {
    const bool shown = depth_ops_shown_ && !depth_target_.empty();
    depth_label_->setText(tr("depth %1").arg(text(depth_target_)));
    for (QWidget *widget : std::initializer_list<QWidget *>{depth_label_, depth_load_, depth_store_})
        widget->setVisible(shown);
    showOps(*depth_load_, *depth_store_, depth_);
    setVisible(!rows_.empty() || shown);
}

void AttachmentOpsForm::depthChanged(const char *key, const QComboBox &combo) {
    if (loading() || depth_target_.empty()) return;
    Json entry = depth_.is_object() ? depth_ : Json(depth_target_);
    if (entry.is_object()) entry["target"] = depth_target_;
    depth_ = withOp(std::move(entry), key, combo);
    notifyChanged();
}

void AttachmentOpsForm::pickClearColor(std::size_t index) {
    if (index >= rows_.size()) return;
    const auto current = clearColorComponents(rows_[index].clear_color->text());
    const QColor chosen = QColorDialog::getColor(
        QColor::fromRgbF(static_cast<float>(current[0]), static_cast<float>(current[1]),
                         static_cast<float>(current[2]), static_cast<float>(current[3])),
        this, tr("Clear colour of %1").arg(text(targets_[index])),
        QColorDialog::ShowAlphaChannel);
    // The rows may have been rebuilt while the picker was open.
    if (!chosen.isValid() || index >= rows_.size()) return;
    rows_[index].clear_color->setText(
        clearColorText(chosen.redF(), chosen.greenF(), chosen.blueF(), chosen.alphaF()));
}

Json AttachmentOpsForm::color() const {
    Json entries = Json::array();
    bool all_names = true;
    for (const auto &entry : entries_) {
        entries.push_back(bareWhenPlain(entry));
        all_names = all_names && entries.back().is_string();
    }
    if (!all_names && single_entry_ && entries.size() == 1) return entries.at(0);
    return entries;
}

Json AttachmentOpsForm::clearColors() const {
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

Json AttachmentOpsForm::depth() const {
    if (depth_target_.empty()) return nullptr;
    if (!depth_.is_object()) return depth_target_;
    Json entry = depth_;
    entry["target"] = depth_target_;
    return entry;
}

} // namespace PelicanStudio
