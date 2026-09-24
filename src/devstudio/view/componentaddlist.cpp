#include "componentaddlist.hpp"

#include <QBrush>
#include <QComboBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QString>
#include <QVariant>

namespace PelicanStudio {

void fillComponentAddTypes(QComboBox &types, const std::vector<ComponentAddRow> &rows) {
    types.clear();
    const QBrush grey{types.palette().color(QPalette::Disabled, QPalette::Text)};
    for (const auto &row : rows) {
        types.addItem(QString::fromStdString(row.label), QString::fromStdString(row.key));
        const int index = types.count() - 1;
        types.setItemData(index, QVariant::fromValue(static_cast<qulonglong>(row.candidate)),
                          ComponentAddCandidateRole);
        types.setItemData(index, row.addable, ComponentAddAddableRole);
        types.setItemData(index, QString::fromStdString(row.blocked), ComponentAddBlockedRole);
        if (!row.addable) types.setItemData(index, grey, Qt::ForegroundRole);
    }
}

std::optional<ComponentAddRow> currentComponentAddRow(const QComboBox &types) {
    const int index = types.currentIndex();
    if (index < 0) return std::nullopt;
    const auto candidate = types.itemData(index, ComponentAddCandidateRole);
    if (!candidate.isValid()) return std::nullopt;
    ComponentAddRow row;
    row.candidate = static_cast<std::size_t>(candidate.toULongLong());
    row.key = types.itemData(index, Qt::UserRole).toString().toStdString();
    row.label = types.itemText(index).toStdString();
    row.addable = types.itemData(index, ComponentAddAddableRole).toBool();
    row.blocked = types.itemData(index, ComponentAddBlockedRole).toString().toStdString();
    return row;
}

bool applyComponentAddAvailability(const QComboBox &types, bool ready, QPushButton &add,
                                   QPlainTextEdit &declaration) {
    const auto row = currentComponentAddRow(types);
    const bool addable = row && row->addable;
    add.setEnabled(ready && addable);
    declaration.setReadOnly(!ready || !addable);
    return addable;
}

} // namespace PelicanStudio
