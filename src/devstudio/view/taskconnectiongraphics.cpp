#include "taskconnectiongraphics.hpp"

#include <QFontMetrics>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QMouseEvent>
#include <QPainterPath>
#include <algorithm>
#include <map>

namespace PelicanStudio {
namespace {
constexpr int SideRole = TaskConnectionSideRole;
constexpr int NameRole = TaskConnectionNameRole;
constexpr qreal Width = 250;
constexpr qreal Right = 380;
constexpr qreal Step = 66;
} // namespace

TaskConnectionGraphics::TaskConnectionGraphics(QWidget *parent)
    : QGraphicsView(parent) {
  setScene(new QGraphicsScene(this));
  setRenderHint(QPainter::Antialiasing);
  setMinimumHeight(190);
  setMaximumHeight(300);
  setObjectName("pelican.computeTasks.connectionGraph");
  setAccessibleName(tr("Task resource connections"));
  setToolTip(tr("Select a port group on the left, then click a declared "
                "resource on the right. This updates the JSON draft; Apply "
                "task validates and saves it."));
}

QString TaskConnectionGraphics::kindOf(const QString &name) const {
  const auto found =
      std::find_if(resources_.begin(), resources_.end(),
                   [&](const auto &r) { return r.name == name; });
  return found == resources_.end() ? QString{} : found->kind;
}

void TaskConnectionGraphics::setConnections(std::vector<PortGroup> groups,
                                            std::vector<Resource> resources) {
  groups_ = std::move(groups);
  resources_ = std::move(resources);
  if (std::none_of(groups_.begin(), groups_.end(),
                   [&](const auto &g) { return g.resource == selected_; }))
    selected_.clear();
  redraw();
}

void TaskConnectionGraphics::setTargetFilter(QStringList names) {
  target_filter_ = std::move(names);
  redraw();
}

void TaskConnectionGraphics::redraw() {
  scene()->clear();
  const auto textColor = palette().color(QPalette::Text);
  auto heading = [&](const QString &text, qreal x) {
    auto *item = scene()->addText(text);
    item->setDefaultTextColor(textColor);
    item->setPos(x, 0);
  };
  heading(tr("Task ports (select a group)"), 0);
  heading(tr("Declared resources (click to connect)"), Right);
  std::vector<Resource> displayed;
  for (const auto &resource : resources_)
    if (target_filter_.isEmpty() || target_filter_.contains(resource.name))
      displayed.push_back(resource);
  for (const auto &group : groups_) {
    if (std::none_of(displayed.begin(), displayed.end(),
                     [&](const auto &r) { return r.name == group.resource; }))
      displayed.push_back({group.resource, kindOf(group.resource)});
  }
  std::map<QString, qreal> positions;
  auto box = [&](qreal x, qreal y, const QString &label, const QString &name,
                 int side, bool selected) {
    const QColor color = selected ? palette().color(QPalette::Highlight)
                                  : palette().color(QPalette::Mid);
    auto *rect =
        scene()->addRect(QRectF(x, y, Width, 54), QPen(color, selected ? 3 : 1),
                         palette().brush(QPalette::Base));
    rect->setData(SideRole, side);
    rect->setData(NameRole, name);
    rect->setToolTip(label);
    auto *text = new QGraphicsTextItem(rect);
    text->setDefaultTextColor(textColor);
    QStringList visibleLines;
    for (const auto &line : label.split('\n'))
      visibleLines.push_back(
          QFontMetrics(text->font())
              .elidedText(line, Qt::ElideRight, int(Width - 14)));
    text->setPlainText(visibleLines.join('\n'));
    text->setTextWidth(Width - 10);
    text->setPos(x + 4, y + 2);
  };
  for (std::size_t i = 0; i < displayed.size(); ++i) {
    const auto &resource = displayed[i];
    const qreal y = 34 + i * Step;
    positions[resource.name] = y + 27;
    const bool declared =
        std::any_of(resources_.begin(), resources_.end(),
                    [&](const auto &r) { return r.name == resource.name; });
    const auto kind = !declared ? tr("unresolved")
                                : (resource.kind.isEmpty() ? tr("unknown kind")
                                                           : resource.kind);
    const bool selectable = declared && (target_filter_.isEmpty() || target_filter_.contains(resource.name));
    box(Right, y, resource.name + "\n" + kind, resource.name, selectable ? 2 : 0,
        false);
  }
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    const auto &group = groups_[i];
    const qreal y = 34 + i * Step;
    const bool selected = group.resource == selected_;
    box(0, y, group.aliases.join(", ") + "\n" + group.resource, group.resource,
        1, selected);
    QPainterPath path(QPointF(Width, y + 27));
    path.cubicTo(Width + 55, y + 27, Right - 55, positions.at(group.resource),
                 Right, positions.at(group.resource));
    auto *line = scene()->addPath(
        path, QPen(selected ? palette().color(QPalette::Highlight)
                            : palette().color(QPalette::Mid),
                   selected ? 3 : 1.5));
    line->setZValue(-1);
  }
  if (groups_.empty()) {
    auto *empty = scene()->addText(
        tr("No valid resource ports in the current JSON draft."));
    empty->setDefaultTextColor(textColor);
    empty->setPos(0, 34);
  }
  scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-8, -8, 8, 8));
}

void TaskConnectionGraphics::mousePressEvent(QMouseEvent *event) {
  if (event->button() != Qt::LeftButton)
    return QGraphicsView::mousePressEvent(event);
  auto *item = itemAt(event->position().toPoint());
  while (item && !item->data(SideRole).isValid())
    item = item->parentItem();
  if (!item)
    return;
  const auto name = item->data(NameRole).toString();
  const auto side = item->data(SideRole).toInt();
  if (side == 1) {
    selected_ = name;
    redraw();
    return;
  }
  if (side != 2 || selected_.isEmpty())
    return;
  const auto fromKind = kindOf(selected_);
  const auto toKind = kindOf(name);
  const auto known = [](const QString &kind) {
    return kind == "image" || kind == "buffer";
  };
  if (known(fromKind) && known(toKind) && fromKind != toKind) {
    if (diagnostic)
      diagnostic(tr("An %1 resource cannot be connected to a %2 resource. The "
                    "JSON draft was kept.")
                     .arg(fromKind, toKind));
    return;
  }
  if (name != selected_ && resourceRebound)
    resourceRebound(selected_, name);
}

} // namespace PelicanStudio
