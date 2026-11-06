#include "prefabplacemenu.hpp"

#include <QAction>
#include <QMenu>

namespace PelicanStudio {

PrefabPlaceMenu::PrefabPlaceMenu(
    QMenu &add_object_menu,
    std::function<void(const std::string &prefab)> place, QObject *parent)
    : QObject(parent), place_(std::move(place)) {
    menu_ = add_object_menu.addMenu(tr("Prefab"));
    menu_->setObjectName(QStringLiteral("pelican.outliner.addObject.prefabMenu"));
    menu_->setToolTipsVisible(true);
    engineUnavailable();
}

void PrefabPlaceMenu::placeholder(const QString &text) {
    menu_->clear();
    auto *action = menu_->addAction(text);
    action->setObjectName(QStringLiteral("pelican.outliner.addObject.prefabNone"));
    action->setEnabled(false);
}

void PrefabPlaceMenu::setList(const std::vector<PrefabListEntry> &entries) {
    if (entries.empty()) {
        placeholder(tr("No prefabs registered"));
        return;
    }
    menu_->clear();
    for (const auto &entry : entries) {
        auto *action = menu_->addAction(QString::fromStdString(prefabMenuLabel(entry)));
        action->setObjectName(QStringLiteral("pelican.outliner.addObject.prefab.") +
                              QString::fromStdString(entry.name));
        if (!entry.required_parameters.empty()) {
            action->setEnabled(false);
            QString names;
            for (const auto &name : entry.required_parameters) {
                if (!names.isEmpty()) names += QStringLiteral(", ");
                names += QString::fromStdString(name);
            }
            action->setToolTip(tr("Has no default for %1, and a value can't be set when placing yet.")
                                   .arg(names));
        } else {
            connect(action, &QAction::triggered, this,
                    [this, name = entry.name] { place_(name); });
        }
    }
}

void PrefabPlaceMenu::engineUnavailable() {
    placeholder(tr("Prefabs are listed when the engine is running"));
}

void PrefabPlaceMenu::listFailed() {
    placeholder(tr("Could not read the prefab list"));
}

} // namespace PelicanStudio
