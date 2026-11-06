#pragma once

#include "../model/prefabplacement.hpp"

#include <QObject>

#include <functional>
#include <string>
#include <vector>

class QMenu;

namespace PelicanStudio {

class PrefabPlaceMenu final : public QObject {
    QMenu *menu_ = nullptr;
    std::function<void(const std::string &prefab)> place_;

    void placeholder(const QString &text);

  public:
    PrefabPlaceMenu(QMenu &add_object_menu,
                    std::function<void(const std::string &prefab)> place,
                    QObject *parent);

    void setList(const std::vector<PrefabListEntry> &entries);
    void engineUnavailable();
    void listFailed();
};

} // namespace PelicanStudio
