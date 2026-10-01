#include "passform.hpp"

#include <QComboBox>
#include <QSignalBlocker>
#include <QStringList>

#include <utility>

namespace PelicanStudio {

PassForm::PassForm(QString object_prefix, QWidget *parent)
    : QWidget(parent), prefix_{std::move(object_prefix)} {}

QString PassForm::childName(std::string_view suffix) const {
    return prefix_ + QLatin1Char('.') + passform::text(suffix);
}

void PassForm::notifyChanged() {
    if (!loading_) emit changed();
}

namespace passform {

namespace {
// An entry that holds a JSON value the list does not name keeps it here.
constexpr int RawJsonRole = Qt::UserRole + 1;
} // namespace

QString text(std::string_view value) {
    return QString::fromUtf8(value.data(),
                             static_cast<qsizetype>(value.size()));
}

void fillOptionalChoices(QComboBox &combo, const QString &default_label,
                         const std::vector<std::string> &values) {
    const QSignalBlocker blocker{&combo};
    combo.clear();
    combo.addItem(default_label, QString{});
    for (const auto &value : values) combo.addItem(text(value), text(value));
    combo.setCurrentIndex(0);
}

void showOptionalValue(QComboBox &combo, const Json *value) {
    const QSignalBlocker blocker{&combo};
    if (value == nullptr || value->is_null()) {
        combo.setCurrentIndex(0);
        return;
    }
    if (value->is_string()) {
        selectData(combo, text(value->get<std::string>()));
        return;
    }
    const QString raw = text(value->dump());
    for (int index = 1; index < combo.count(); ++index) {
        if (combo.itemData(index, RawJsonRole).toString() == raw) {
            combo.setCurrentIndex(index);
            return;
        }
    }
    combo.addItem(QObject::tr("custom %1").arg(raw), raw);
    combo.setItemData(combo.count() - 1, raw, RawJsonRole);
    combo.setItemData(combo.count() - 1,
                      QObject::tr("Kept as the declaration has it: %1").arg(raw),
                      Qt::ToolTipRole);
    combo.setCurrentIndex(combo.count() - 1);
}

std::optional<Json> optionalValue(const QComboBox &combo) {
    const int index = combo.currentIndex();
    if (index <= 0) return std::nullopt;
    const auto raw = combo.itemData(index, RawJsonRole).toString();
    if (!raw.isEmpty()) return Json::parse(raw.toStdString());
    return Json(combo.itemData(index).toString().toStdString());
}

void selectData(QComboBox &combo, const QString &value) {
    int found = combo.findData(value);
    if (found < 0) {
        combo.addItem(value, value);
        found = combo.count() - 1;
    }
    combo.setCurrentIndex(found);
}

std::vector<std::string> splitNames(const QString &line) {
    std::vector<std::string> names;
    for (const auto &part : line.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const auto name = part.trimmed();
        if (!name.isEmpty()) names.push_back(name.toStdString());
    }
    return names;
}

QString joinNames(const Json &names) {
    QStringList parts;
    if (names.is_array())
        for (const auto &name : names)
            parts.push_back(name.is_string() ? text(name.get<std::string>())
                                             : text(name.dump()));
    return parts.join(QStringLiteral(", "));
}

} // namespace passform

} // namespace PelicanStudio
