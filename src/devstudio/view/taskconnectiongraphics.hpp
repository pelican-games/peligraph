#pragma once

#include <QGraphicsView>
#include <QStringList>
#include <functional>
#include <vector>

namespace PelicanStudio {

inline constexpr int TaskConnectionSideRole = Qt::UserRole + 1;
inline constexpr int TaskConnectionNameRole = Qt::UserRole + 2;

class TaskConnectionGraphics final : public QGraphicsView {
public:
  struct PortGroup {
    QString resource;
    QStringList aliases;
  };
  struct Resource {
    QString name;
    QString kind;
  };

  explicit TaskConnectionGraphics(QWidget *parent = nullptr);
  void setConnections(std::vector<PortGroup> groups,
                      std::vector<Resource> resources);
  std::function<void(QString, QString)> resourceRebound;
  std::function<void(QString)> diagnostic;

protected:
  void mousePressEvent(QMouseEvent *event) override;

private:
  void redraw();
  QString kindOf(const QString &name) const;
  std::vector<PortGroup> groups_;
  std::vector<Resource> resources_;
  QString selected_;
};

} // namespace PelicanStudio
