#pragma once

#include <QObject>
#include <iostream>

namespace PelicanStudio {

class TestBackend : public QObject {
    Q_OBJECT

    std::string m_name;

    Q_PROPERTY(QString name READ getName WRITE setName NOTIFY nameChanged)
  public:
    QString getName() const;
    void setName(QString new_name);
  signals:
    void nameChanged();
};

} // namespace PelicanStudio
