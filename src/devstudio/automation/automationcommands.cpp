#include "automationcommands.hpp"
#include "automationserver.hpp"

#include "../view/frameplangraphics.hpp"
#include "../view/graphcanvaswidget.hpp"
#include "../view/nodegraphics.hpp"
#include "../viewport/embeddedviewport.hpp"
#include "../viewport/viewportgeometry.hpp"

#include <optional>
#include <cmath>
#include <cstring>
#include <QAction>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QTreeView>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGraphicsItem>
#include <QGraphicsProxyWidget>
#include <QGraphicsScene>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLineEdit>
#include <QModelIndex>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QPointingDevice>
#include <QRect>
#include <QScreen>
#include <QScrollBar>
#include <QSet>
#include <QTimer>
#include <QTransform>
#include <QVariant>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <QtTest/QTest>
#include <QtTest/qtestspontaneevent.h>

#include <algorithm>
#include <functional>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace PelicanStudio {
namespace {

#ifdef _WIN32
// Where a widget's global rect lies on the desktop, in physical pixels.
// GetWindowRect and the desktop are physical; the widget's global rect is in
// Qt's logical pixels, whose screen origins stay at their native place while
// the inside of each screen is scaled. Measure the widget from the window's
// client origin (logical, same screen) and scale only that offset, so a
// window on a second screen with another scale (a 125 % virtual monitor
// right of a 100 % one) is read where it is. Empty when the window has no
// handle.
QRect physicalScreenRect(QWidget *top_level, const QRect &global_rect) {
    if (top_level == nullptr) return {};
    const auto hwnd = reinterpret_cast<HWND>(top_level->winId());
    POINT client_origin{0, 0};
    if (hwnd == nullptr || !ClientToScreen(hwnd, &client_origin)) return {};
    const qreal ratio = top_level->devicePixelRatioF();
    const QPoint offset = global_rect.topLeft() - top_level->mapToGlobal(QPoint{0, 0});
    // At a fractional scale an edge falls inside a pixel, and where Qt puts
    // that pixel differs between widgets and native children (the engine's
    // window): keep only the pixels wholly inside, so the picture never
    // takes in a row of the panel beside it.
    const int left = static_cast<int>(std::ceil(offset.x() * ratio - 1e-6));
    const int top = static_cast<int>(std::ceil(offset.y() * ratio - 1e-6));
    const int right = static_cast<int>(std::floor((offset.x() + global_rect.width()) * ratio + 1e-6));
    const int bottom = static_cast<int>(std::floor((offset.y() + global_rect.height()) * ratio + 1e-6));
    QRect physical{client_origin.x + left, client_origin.y + top, right - left, bottom - top};
    // A native child filling the widget (the embedded engine's window) is
    // sized by its own rounding, and at 125 % can be a row short of the
    // widget: what shows there is the child, so read the child's pixels.
    const POINT centre{physical.center().x(), physical.center().y()};
    if (const HWND child = WindowFromPoint(centre);
        child != nullptr && child != hwnd && GetAncestor(child, GA_ROOT) == hwnd) {
        RECT child_rect{};
        if (GetWindowRect(child, &child_rect)) {
            const QRect inside = physical.intersected(QRect{
                QPoint{child_rect.left, child_rect.top},
                QPoint{child_rect.right - 1, child_rect.bottom - 1}});
            // Only a child that covers the widget nearly whole: a small
            // native control under the centre is not the widget's picture.
            if (!inside.isEmpty() &&
                qint64{inside.width()} * inside.height() * 10 >=
                    qint64{physical.width()} * physical.height() * 9)
                physical = inside;
        }
    }
    return physical;
}

// The desktop's pixels over a physical rect, read natively: Qt's grab takes
// logical coordinates and, on a scaled screen, lands a row off.
QImage desktopRegion(const QRect &physical) {
    if (physical.isEmpty()) return {};
    HDC screen_dc = GetDC(nullptr);
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, physical.width(), physical.height());
    HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    const bool copied = BitBlt(memory_dc, 0, 0, physical.width(), physical.height(), screen_dc,
                               physical.x(), physical.y(), SRCCOPY | CAPTUREBLT) != FALSE;
    QImage image;
    if (copied) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = physical.width();
        info.bmiHeader.biHeight = -physical.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        image = QImage(physical.width(), physical.height(), QImage::Format_RGB32);
        if (GetDIBits(memory_dc, bitmap, 0, static_cast<UINT>(physical.height()),
                      image.bits(), &info, DIB_RGB_COLORS) == 0)
            image = {};
    }
    SelectObject(memory_dc, previous);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return image;
}

// The window's own composed content, children included -- the embedded
// engine too -- whatever else is on the desktop in front of it. A screen
// grab reads the desktop, so a window of another application lying over the
// viewport ends up in the picture (a scenario on a machine in use went red
// that way twice). Empty when the window cannot be printed.
QImage printWindowRegion(QWidget *top_level, const QRect &global_rect) {
    if (top_level == nullptr) return {};
    const auto hwnd = reinterpret_cast<HWND>(top_level->winId());
    RECT window_rect{};
    if (hwnd == nullptr || !GetWindowRect(hwnd, &window_rect)) return {};
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    if (width <= 0 || height <= 0) return {};
    HDC screen_dc = GetDC(nullptr);
    HDC memory_dc = CreateCompatibleDC(screen_dc);
    HBITMAP bitmap = CreateCompatibleBitmap(screen_dc, width, height);
    HGDIOBJ previous = SelectObject(memory_dc, bitmap);
    constexpr UINT RenderFullContent = 0x00000002; // PW_RENDERFULLCONTENT
    const bool printed = PrintWindow(hwnd, memory_dc, RenderFullContent) != FALSE;
    QImage image;
    if (printed) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        image = QImage(width, height, QImage::Format_RGB32);
        if (GetDIBits(memory_dc, bitmap, 0, static_cast<UINT>(height),
                      image.bits(), &info, DIB_RGB_COLORS) == 0)
            image = {};
    }
    SelectObject(memory_dc, previous);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    if (image.isNull()) return {};
    const QRect physical =
        physicalScreenRect(top_level, global_rect).translated(-window_rect.left, -window_rect.top);
    const QRect clipped = physical.intersected(image.rect());
    if (clipped.isEmpty()) return {};
    return image.copy(clipped);
}

bool isOneColour(const QImage &image) {
    if (image.isNull()) return true;
    const QImage rgb = image.convertToFormat(QImage::Format_RGB32);
    const QRgb first = rgb.pixel(0, 0);
    for (int y = 0; y < rgb.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(rgb.constScanLine(y));
        for (int x = 0; x < rgb.width(); ++x)
            if (row[x] != first) return false;
    }
    return true;
}

// Whether the desktop at the rect shows this window (or a child of it, the
// embedded engine included) rather than another application's window lying
// over it. Samples a grid of points; any point owned by another top-level
// window means a desktop grab would read that window instead.
bool desktopShowsWindow(QWidget *top_level, const QRect &global_rect) {
    if (top_level == nullptr || global_rect.isEmpty()) return false;
    const auto hwnd = reinterpret_cast<HWND>(top_level->winId());
    if (hwnd == nullptr) return false;
    const QRect physical = physicalScreenRect(top_level, global_rect);
    if (physical.isEmpty()) return false;
    constexpr int Steps = 4;
    for (int iy = 0; iy <= Steps; ++iy) {
        for (int ix = 0; ix <= Steps; ++ix) {
            const double fx = (ix + 0.5) / (Steps + 1);
            const double fy = (iy + 0.5) / (Steps + 1);
            const POINT point{
                static_cast<LONG>(physical.x() + fx * physical.width()),
                static_cast<LONG>(physical.y() + fy * physical.height())};
            const HWND at = WindowFromPoint(point);
            if (at == nullptr || GetAncestor(at, GA_ROOT) != hwnd)
                return false;
        }
    }
    return true;
}
#endif

constexpr auto BadRequest = "automation.bad_request@1";
constexpr auto WidgetMissing = "automation.widget_missing@1";
constexpr auto UnknownCommand = "automation.unknown_command@1";
constexpr auto EngineNotReady = "automation.engine_not_ready@1";
constexpr auto RpcFailed = "automation.rpc_failed@1";
constexpr auto WaitTimeout = "automation.wait_timeout@1";
constexpr auto ScreenshotFailed = "automation.screenshot_failed@1";
constexpr auto ProjectFailed = "automation.project_failed@1";

constexpr int DefaultTimeoutMs = 30000;

// Where the scene-item half of a name starts. `pelican.graphCanvasView` is a
// widget; `pelican.graphCanvasView::graph_value_pill#left/gain` is one item
// drawn inside it. Two characters, so an objectName with a single colon in it
// still resolves as a widget.
constexpr auto SceneSeparator = "::";

// --------------------------------------------------------------------------
// Targets

struct Target {
    // The widget events are delivered to: the widget itself, or, for a scene
    // item, the view's viewport.
    QWidget *widget = nullptr;
    QWidget *owner = nullptr; // the named widget, for geometry and text
    QGraphicsView *view = nullptr;
    QGraphicsItem *item = nullptr;
    // OL-1: one row of a list or tree, `<view>::row#<text>`, where it stands
    // in the view's viewport.
    std::optional<QRect> row;

    [[nodiscard]] bool valid() const noexcept { return widget != nullptr; }
    [[nodiscard]] bool isSceneItem() const noexcept { return item != nullptr; }
};

QString itemKind(const QGraphicsItem &item) {
    return item.data(NodeGraphItemKindRole).toString();
}

// The name `list` publishes for one drawn item, and the one `click` accepts
// back. Identity is what the scene already files the item under: the node id,
// plus the port when the item belongs to one port.
QString sceneItemName(const QGraphicsView &view, const QGraphicsItem &item,
                      int fallback_index) {
    const QString kind = itemKind(item);
    if (kind.isEmpty()) return {};
    QString identity = item.data(NodeGraphNameRole).toString();
    QString port = item.data(GraphCanvasPortRole).toString();
    // N-8: a Frame Plan socket belongs to a resource AND to a side -- one pass
    // can read and write the same target -- so the side travels in the port
    // half of the name. A band names the field it retypes.
    // VI-3a: a socket drawn from the shader's declarations is named by its
    // binding ("in@0") or as the variadic band's end ("in:variadic").
    if (port.isEmpty()) port = item.data(FramePlanSocketKeyRole).toString();
    if (port.isEmpty()) {
        const QString resource = item.data(FramePlanPortRole).toString();
        if (!resource.isEmpty()) {
            const QString side =
                item.data(FramePlanSocketDirectionRole).toString();
            port = side.isEmpty()
                       ? resource
                       : (side == QLatin1String{"output"}
                              ? QStringLiteral("out:")
                              : QStringLiteral("in:")) +
                             resource;
        } else {
            port = item.data(FramePlanInlineFieldRole).toString();
        }
    }
    if (!port.isEmpty()) {
        identity = identity.isEmpty() ? port
                                      : identity + QStringLiteral("/") + port;
    }
    // LP-4d1: a thing drawn inside a loop's zone belongs to the body's
    // definition, and its id can be a root node's as well; the definition
    // goes in front (`graph_node#<definition>/<id>`). Neither a definition
    // name nor an id holds a slash, so the first one is where they part.
    const QString zone = item.data(GraphCanvasZoneScopeRole).toString();
    if (!zone.isEmpty() && !identity.isEmpty())
        identity = zone + QStringLiteral("/") + identity;
    if (identity.isEmpty()) {
        const QString from = item.data(NodeGraphFromNameRole).toString();
        const QString to = item.data(NodeGraphToNameRole).toString();
        if (!from.isEmpty() || !to.isEmpty())
            identity = from + QStringLiteral("->") + to;
    }
    if (identity.isEmpty())
        identity = QStringLiteral("@") + QString::number(fallback_index);
    return view.objectName() + QString::fromLatin1(SceneSeparator) + kind +
           QStringLiteral("#") + identity;
}

// VI-3a: the name a declared socket answered to before it was named by its
// binding -- `.../in:<resource>` for the socket that resource is connected
// to -- so a scenario that waits for a read keeps working. `list` does not
// publish it; only resolving a name falls back to it.
QString sceneItemAlias(const QGraphicsView &view, const QGraphicsItem &item) {
    // LP-4d2: a graph node can have an input and an output of one name (a
    // fullscreen pass's `color`), which `graph_socket#<id>/<port>` cannot tell
    // apart; `.../in:<port>` and `.../out:<port>` can. Resolving only.
    if (itemKind(item) == QLatin1String{GraphCanvasSocketItem}) {
        // Q-19: a socket of a render node's shader declarations is named by
        // its binding (`in@0`); `.../in:<port>` finds it by the numbered port
        // connected there, as VI-3a's `in:<resource>` does.
        const QString bound = item.data(GraphCanvasBoundPortRole).toString();
        const QString port =
            bound.isEmpty() ? item.data(GraphCanvasPortRole).toString() : bound;
        const QString direction = item.data(GraphCanvasSocketDirectionRole).toString();
        const QString node = item.data(NodeGraphNameRole).toString();
        if (port.isEmpty() || node.isEmpty() || direction.isEmpty()) return {};
        QString identity = node + QStringLiteral("/") +
                           (direction == QLatin1String{"output"} ? QStringLiteral("out:")
                                                                 : QStringLiteral("in:")) +
                           port;
        const QString zone = item.data(GraphCanvasZoneScopeRole).toString();
        if (!zone.isEmpty()) identity = zone + QStringLiteral("/") + identity;
        return view.objectName() + QString::fromLatin1(SceneSeparator) + itemKind(item) +
               QStringLiteral("#") + identity;
    }
    if (item.data(FramePlanSocketKeyRole).toString().isEmpty()) return {};
    const QString resource = item.data(FramePlanPortRole).toString();
    if (resource.isEmpty() ||
        item.data(FramePlanSocketDirectionRole).toString() !=
            QLatin1String{"input"})
        return {};
    const QString identity = item.data(NodeGraphNameRole).toString();
    return view.objectName() + QString::fromLatin1(SceneSeparator) +
           itemKind(item) + QStringLiteral("#") + identity +
           QStringLiteral("/in:") + resource;
}

std::vector<QWidget *> namedWidgets() {
    std::vector<QWidget *> found;
    for (QWidget *widget : QApplication::allWidgets()) {
        if (widget == nullptr || widget->objectName().isEmpty()) continue;
        found.push_back(widget);
    }
    // A visible widget outranks a hidden namesake: two docks can hold the same
    // object name, and the one on screen is the one a gesture means.
    std::stable_sort(found.begin(), found.end(),
                     [](const QWidget *a, const QWidget *b) {
                         return a->isVisible() && !b->isVisible();
                     });
    return found;
}

// A menu entry is a QAction, not a widget: it has no place on screen until
// the menu is open, so a scenario that "clicks" one means "do what it does".
// Actions are named the way widgets are, so the same `name` field reaches both.
std::vector<QAction *> namedActions(const AutomationHost &host) {
    std::vector<QAction *> found;
    const auto collect = [&found](QWidget *root) {
        if (root == nullptr) return;
        for (QAction *action : root->findChildren<QAction *>())
            if (action != nullptr && !action->objectName().isEmpty())
                found.push_back(action);
    };
    collect(host.window);
    for (QWidget *widget : QApplication::topLevelWidgets())
        if (widget != host.window) collect(widget);
    std::stable_sort(found.begin(), found.end(),
                     [](const QAction *a, const QAction *b) {
                         return a->objectName() < b->objectName();
                     });
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

QAction *findAction(const AutomationHost &host, const QString &name) {
    if (name.isEmpty()) return nullptr;
    for (QAction *action : namedActions(host))
        if (action->objectName() == name) return action;
    return nullptr;
}

QWidget *findWidget(const AutomationHost &host, const QString &name) {
    if (host.window != nullptr && host.window->objectName() == name)
        return host.window;
    for (QWidget *widget : namedWidgets())
        if (widget->objectName() == name) return widget;
    return nullptr;
}

QGraphicsView *findGraphicsView(const AutomationHost &host,
                                const QString &name) {
    return qobject_cast<QGraphicsView *>(findWidget(host, name));
}

std::vector<std::pair<QString, QGraphicsItem *>> sceneItems(
    const QGraphicsView &view) {
    std::vector<std::pair<QString, QGraphicsItem *>> found;
    QGraphicsScene *scene = view.scene();
    if (scene == nullptr) return found;
    int index = 0;
    for (QGraphicsItem *item : scene->items(Qt::AscendingOrder)) {
        const QString name = sceneItemName(view, *item, index++);
        if (name.isEmpty()) continue;
        found.emplace_back(name, item);
    }
    return found;
}

QString itemText(const QGraphicsItem &item);

// OL-1: `<list or tree>::row#<text>` is the row a person reads as <text>,
// among the rows the view shows (a folded branch's rows are not on screen).
// The view is scrolled to it, so a drag can start or end on it.
Target resolveItemViewRow(const AutomationHost &host, const QString &view_name,
                          const QString &row_name) {
    Target target;
    const QString prefix = QStringLiteral("row#");
    auto *view = qobject_cast<QAbstractItemView *>(findWidget(host, view_name));
    if (view == nullptr || view->model() == nullptr || !row_name.startsWith(prefix))
        return target;
    const QString text = row_name.mid(prefix.size());
    auto *tree = qobject_cast<QTreeView *>(view);
    QModelIndex found;
    const std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        const int count = view->model()->rowCount(parent);
        for (int row = 0; row < count && !found.isValid(); ++row) {
            const QModelIndex child = view->model()->index(row, 0, parent);
            if (!child.isValid()) continue;
            if (child.data(Qt::DisplayRole).toString() == text) {
                found = child;
                return;
            }
            if (tree != nullptr && tree->isExpanded(child)) walk(child);
        }
    };
    walk(QModelIndex{});
    if (!found.isValid()) return target;
    view->scrollTo(found);
    const QRect rect = view->visualRect(found);
    if (rect.isEmpty()) return target;
    target.widget = view->viewport();
    target.owner = view;
    target.row = rect;
    return target;
}

Target resolveTarget(const AutomationHost &host, const QString &name) {
    Target target;
    if (name.isEmpty()) return target;
    const int separator =
        static_cast<int>(name.indexOf(QString::fromLatin1(SceneSeparator)));
    if (separator < 0) {
        target.widget = findWidget(host, name);
        target.owner = target.widget;
        target.view = qobject_cast<QGraphicsView *>(target.widget);
        return target;
    }
    QGraphicsView *view = findGraphicsView(host, name.left(separator));
    if (view == nullptr)
        return resolveItemViewRow(
            host, name.left(separator),
            name.mid(separator + static_cast<int>(std::strlen(SceneSeparator))));
    // One drawn thing can be several items: a value pill is a rounded rect with
    // the number as its child, and both carry the same identity. The one that
    // can answer `text` is the one a scenario means, and either of them sits at
    // the same place for a click.
    const auto items = sceneItems(*view);
    for (const auto &[candidate, item] : items) {
        if (candidate != name) continue;
        if (target.item == nullptr || itemText(*item).isEmpty() == false) {
            target.view = view;
            target.item = item;
            target.widget = view->viewport();
            target.owner = view;
            if (!itemText(*item).isEmpty()) break;
        }
    }
    if (target.item != nullptr) return target;
    // VI-3a: the resource-named socket is the declared one it feeds.
    for (const auto &[candidate, item] : items) {
        (void)candidate;
        if (sceneItemAlias(*view, *item) != name) continue;
        target.view = view;
        target.item = item;
        target.widget = view->viewport();
        target.owner = view;
        break;
    }
    return target;
}

// --------------------------------------------------------------------------
// Reading widgets and items

QString widgetText(const QWidget &widget) {
    if (const auto *combo = qobject_cast<const QComboBox *>(&widget))
        return combo->currentText();
    for (const char *property : {"text", "plainText", "title"}) {
        const QVariant value = widget.property(property);
        if (value.isValid() && value.canConvert<QString>())
            return value.toString();
    }
    return {};
}

// An editor the canvas opened over what it edits is a real widget inside a
// QGraphicsProxyWidget: it is not in any window's child chain, so it has to be
// reached through the item that carries it. Keys and text go to the widget --
// QTest delivers straight to it, which does not depend on who holds focus.
QWidget *embeddedWidget(const Target &target) {
    if (target.item == nullptr) return nullptr;
    if (target.item->type() != QGraphicsProxyWidget::Type) return nullptr;
    return static_cast<QGraphicsProxyWidget *>(target.item)->widget();
}

QString itemText(const QGraphicsItem &item) {
    if (item.type() == QGraphicsProxyWidget::Type) {
        const QWidget *embedded =
            static_cast<const QGraphicsProxyWidget &>(item).widget();
        return embedded != nullptr ? widgetText(*embedded) : QString{};
    }
    // What the item *is* outranks what it draws: a value pill files the stored
    // value under a role and draws it with its source appended, and a scenario
    // that asks for the value means the value.
    for (const int role : {GraphCanvasValueTextRole, GraphCanvasLabelRole}) {
        const QVariant value = item.data(role);
        if (value.isValid() && !value.toString().isEmpty())
            return value.toString();
    }
    if (item.type() == QGraphicsSimpleTextItem::Type)
        return static_cast<const QGraphicsSimpleTextItem &>(item).text();
    if (item.type() == QGraphicsTextItem::Type)
        return static_cast<const QGraphicsTextItem &>(item).toPlainText();
    return {};
}

QString targetText(const Target &target) {
    if (target.isSceneItem()) return itemText(*target.item);
    return target.owner != nullptr ? widgetText(*target.owner) : QString{};
}

bool targetVisible(const Target &target) {
    if (target.isSceneItem())
        return target.item->isVisible() && target.view->isVisible();
    if (target.row)
        return target.owner != nullptr && target.owner->isVisible() &&
               target.widget->rect().intersects(*target.row);
    return target.owner != nullptr && target.owner->isVisible();
}

// Q-16: whether a press on the target would reach it. QWidget::isEnabled is
// already false under a switched-off ancestor.
bool targetEnabled(const Target &target) {
    if (target.isSceneItem())
        return target.view->isEnabled() && target.item->isEnabled();
    const QWidget *widget = target.owner != nullptr ? target.owner : target.widget;
    return widget->isEnabled() && target.widget->isEnabled();
}

QRect globalRect(const Target &target) {
    if (target.row)
        return QRect(target.widget->mapToGlobal(target.row->topLeft()), target.row->size());
    if (target.isSceneItem()) {
        const QRect local =
            target.view->mapFromScene(target.item->sceneBoundingRect())
                .boundingRect();
        return QRect(target.view->viewport()->mapToGlobal(local.topLeft()),
                     local.size());
    }
    if (target.owner == nullptr) return {};
    return QRect(target.owner->mapToGlobal(QPoint{0, 0}),
                 target.owner->size());
}

QJsonArray rectJson(const QRect &rect) {
    return QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()};
}

// Where inside `target.widget` a synthetic press lands.
QPoint hotspot(const Target &target) {
    if (target.row) return target.row->center();
    if (target.isSceneItem()) {
        // LP-4d1: an item whose middle is something else (a zone's frame
        // holds its body) names where a press meant for it lands.
        const QVariant spot = target.item->data(NodeGraphHotspotRole);
        if (spot.canConvert<QPointF>() && spot.isValid())
            return target.view->mapFromScene(
                target.item->mapToScene(spot.toPointF()));
        const QPointF centre = target.item->sceneBoundingRect().center();
        return target.view->mapFromScene(centre);
    }
    return target.widget->rect().center();
}

// --------------------------------------------------------------------------
// Request helpers

Qt::KeyboardModifiers parseModifiers(const QJsonValue &value, bool *ok) {
    *ok = true;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
    QStringList names;
    if (value.isUndefined() || value.isNull()) return modifiers;
    if (value.isString())
        names = value.toString().split(QLatin1Char('+'), Qt::SkipEmptyParts);
    else if (value.isArray())
        for (const QJsonValue &entry : value.toArray())
            names.push_back(entry.toString());
    else {
        *ok = false;
        return modifiers;
    }
    for (QString name : names) {
        name = name.trimmed().toLower();
        if (name == QLatin1String("ctrl") || name == QLatin1String("control"))
            modifiers |= Qt::ControlModifier;
        else if (name == QLatin1String("shift"))
            modifiers |= Qt::ShiftModifier;
        else if (name == QLatin1String("alt"))
            modifiers |= Qt::AltModifier;
        else if (name == QLatin1String("meta"))
            modifiers |= Qt::MetaModifier;
        else if (!name.isEmpty())
            *ok = false;
    }
    return modifiers;
}

Qt::MouseButton parseButton(const QJsonValue &value, bool *ok) {
    *ok = true;
    if (value.isUndefined() || value.isNull()) return Qt::LeftButton;
    const QString name = value.toString().trimmed().toLower();
    if (name == QLatin1String("left")) return Qt::LeftButton;
    if (name == QLatin1String("right")) return Qt::RightButton;
    if (name == QLatin1String("middle")) return Qt::MiddleButton;
    *ok = false;
    return Qt::LeftButton;
}

// --------------------------------------------------------------------------
// Synthesis
//
// QTest answers `click` and `key`, but not the three gestures Q-1b adds. Its
// `mouseMove` for widgets takes a shortcut when no button is down -- it moves
// the real cursor and delivers nothing -- and it has no wheel for a widget at
// all. So press / move / release / wheel are built here, on one piece of state:
// which buttons this surface is holding down. That state is what makes a
// held gesture a gesture: the moves between a press and a release carry
// `buttons()`, which is the only thing a panning view looks at.
Qt::MouseButtons &heldButtons() {
    static Qt::MouseButtons held = Qt::NoButton;
    return held;
}

QJsonArray buttonNames(Qt::MouseButtons buttons) {
    QJsonArray names;
    if (buttons & Qt::LeftButton) names.push_back(QStringLiteral("left"));
    if (buttons & Qt::MiddleButton) names.push_back(QStringLiteral("middle"));
    if (buttons & Qt::RightButton) names.push_back(QStringLiteral("right"));
    return names;
}

void sendMouse(QWidget *widget, QEvent::Type type, const QPoint &position,
               Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    if (type == QEvent::MouseButtonPress)
        heldButtons() |= button;
    else if (type == QEvent::MouseButtonRelease)
        heldButtons() &= ~static_cast<Qt::MouseButtons>(button);
    QMouseEvent event(type, QPointF{position},
                      widget->mapToGlobal(QPointF{position}), button,
                      heldButtons(), modifiers,
                      QPointingDevice::primaryPointingDevice());
    QSpontaneKeyEvent::setSpontaneous(&event);
    QApplication::sendEvent(widget, &event);
}

void sendWheel(QWidget *widget, const QPoint &position, const QPoint &angle,
               Qt::KeyboardModifiers modifiers) {
    QWheelEvent event(QPointF{position}, widget->mapToGlobal(QPointF{position}),
                      QPoint{}, angle, heldButtons(), modifiers,
                      Qt::NoScrollPhase, false);
    QSpontaneKeyEvent::setSpontaneous(&event);
    QApplication::sendEvent(widget, &event);
}

// Where a pointer event has to be posted for a scroll area to see it: the
// viewport, never the frame. QAbstractScrollArea leaves mouse and wheel events
// that arrive at the frame alone, because on a real desktop they arrive at the
// viewport under the cursor; from there the area's own filter carries them to
// QGraphicsView::mousePressEvent and ::wheelEvent, which is where a canvas puts
// its rule. Naming the view and naming a drawn item inside it therefore reach
// the same receiver, and `point` is carried across with it.
QWidget *pointerReceiver(const Target &target, QPoint *point) {
    auto *area = qobject_cast<QAbstractScrollArea *>(target.widget);
    if (area == nullptr || area->viewport() == nullptr) return target.widget;
    *point = area->viewport()->mapFrom(area, *point);
    return area->viewport();
}

QString shortcutText(const QKeySequence &sequence) {
    return sequence.isEmpty() ? QString{}
                              : sequence.toString(QKeySequence::PortableText);
}

// What `list` publishes for a widget. Only QAbstractButton and its kin carry a
// shortcut property; everything else answers with nothing rather than with a
// guess.
QString widgetShortcut(const QWidget &widget) {
    const QVariant value = widget.property("shortcut");
    if (!value.isValid() || !value.canConvert<QKeySequence>()) return {};
    return shortcutText(value.value<QKeySequence>());
}

int timeoutOf(const QJsonObject &request, int fallback = DefaultTimeoutMs) {
    const QJsonValue value = request.value(QStringLiteral("timeout_ms"));
    if (!value.isDouble()) return fallback;
    const int requested = value.toInt(fallback);
    return requested > 0 ? requested : fallback;
}

// A point inside `target.widget`, from either a widget-local `point` or a
// scene position that only a QGraphicsView can answer for.
bool pointFor(const QJsonObject &request, const Target &target, QPoint *out,
              AutomationReply *rejected) {
    const QJsonValue scene = request.value(QStringLiteral("scene"));
    const QJsonValue point = request.value(QStringLiteral("point"));
    if (scene.isArray()) {
        if (target.view == nullptr) {
            *rejected = AutomationReply::failure(
                BadRequest,
                QStringLiteral("scene coordinates need a QGraphicsView target"));
            return false;
        }
        const QJsonArray values = scene.toArray();
        if (values.size() != 2) {
            *rejected = AutomationReply::failure(
                BadRequest, QStringLiteral("scene must be [x, y]"));
            return false;
        }
        *out = target.view->mapFromScene(
            QPointF{values.at(0).toDouble(), values.at(1).toDouble()});
        return true;
    }
    if (point.isArray()) {
        const QJsonArray values = point.toArray();
        if (values.size() != 2) {
            *rejected = AutomationReply::failure(
                BadRequest, QStringLiteral("point must be [x, y]"));
            return false;
        }
        *out = QPoint{values.at(0).toInt(), values.at(1).toInt()};
        return true;
    }
    *out = hotspot(target);
    return true;
}

// The target a gesture names, with the "no such widget" answer already shaped.
bool gestureTarget(const AutomationHost &host, const QJsonObject &request,
                   Target *out, AutomationReply *rejected,
                   const QString &key = QStringLiteral("name")) {
    const QString name = request.value(key).toString();
    if (name.isEmpty()) {
        // A bare point with no name lands on whatever the desktop has there.
        if (request.contains(QStringLiteral("point")) &&
            host.window != nullptr) {
            out->widget = host.window;
            out->owner = host.window;
            return true;
        }
        *rejected = AutomationReply::failure(
            BadRequest, QStringLiteral("%1 is required").arg(key));
        return false;
    }
    *out = resolveTarget(host, name);
    if (!out->valid()) {
        *rejected = AutomationReply::failure(
            WidgetMissing,
            QStringLiteral("no widget or drawn item is named \"%1\"").arg(name));
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// Menus
//
// A QAction is not a widget: it has no place on screen until a popup opens over
// it, and `list` cannot see it. These read and fire the actions of the menu bar,
// the toolbars and the context menus without opening any popup -- a popup is a
// modal event loop, which a command that has to answer on this line cannot
// enter.

struct ActionEntry {
    QAction *action = nullptr;
    QString menu;
};

QString withoutMnemonic(QString text) {
    text.remove(QLatin1Char('&'));
    return text.trimmed();
}

// "View/Panels" for an action in the Panels submenu. An action knows the menus
// it was added to; a menu knows the action that opens it; that action knows its
// own menu. Walking that chain names the path a person would read off the menu
// bar.
QString menuPathOf(const QAction *action) {
    QStringList path;
    const QAction *current = action;
    for (int depth = 0; depth < 8 && current != nullptr; ++depth) {
        const QWidget *owner = nullptr;
        for (QObject *object : current->associatedObjects()) {
            if (auto *widget = qobject_cast<QWidget *>(object)) {
                owner = widget;
                break;
            }
        }
        if (owner == nullptr) break;
        QString label;
        if (const auto *menu = qobject_cast<const QMenu *>(owner)) {
            label = withoutMnemonic(menu->title());
            if (label.isEmpty()) label = menu->objectName();
            current = menu->menuAction();
        } else {
            label = owner->windowTitle();
            if (label.isEmpty()) label = owner->objectName();
            label = withoutMnemonic(label);
            current = nullptr;
        }
        if (!label.isEmpty()) path.prepend(label);
    }
    return path.join(QLatin1Char('/'));
}

std::vector<ActionEntry> collectActions(const AutomationHost &host,
                                        QWidget *root) {
    std::vector<QWidget *> roots;
    if (root != nullptr) {
        roots.push_back(root);
    } else {
        if (host.window != nullptr) roots.push_back(host.window);
        for (QWidget *widget : QApplication::topLevelWidgets())
            if (widget != host.window) roots.push_back(widget);
    }
    std::vector<ActionEntry> found;
    QSet<const QAction *> seen;
    const auto take = [&](QAction *action) {
        if (action == nullptr || action->isSeparator()) return;
        if (seen.contains(action)) return;
        seen.insert(action);
        found.push_back(ActionEntry{action, menuPathOf(action)});
    };
    for (QWidget *widget : roots) {
        if (widget == nullptr) continue;
        // `actions()` is what this widget answers shortcuts with; the children
        // are the menus and toolbars it built, which own theirs.
        for (QAction *action : widget->actions()) take(action);
        for (QAction *action : widget->findChildren<QAction *>()) take(action);
    }
    return found;
}

// The name a listing publishes and `action` accepts back. An entry that opens a
// submenu is a QAction nobody names -- what gets a name is the QMenu behind it,
// and that is the name a person grepping the source will find, so it is the one
// the row carries.
QString actionName(const QAction &action) {
    if (!action.objectName().isEmpty()) return action.objectName();
    if (action.menu() != nullptr) return action.menu()->objectName();
    return {};
}

QJsonObject actionJson(const ActionEntry &entry) {
    const QAction &action = *entry.action;
    return QJsonObject{
        {QStringLiteral("name"), actionName(action)},
        {QStringLiteral("text"), withoutMnemonic(action.text())},
        {QStringLiteral("shortcut"), shortcutText(action.shortcut())},
        {QStringLiteral("enabled"), action.isEnabled()},
        {QStringLiteral("visible"), action.isVisible()},
        {QStringLiteral("checkable"), action.isCheckable()},
        {QStringLiteral("checked"), action.isChecked()},
        {QStringLiteral("menu"), entry.menu},
        // An entry that opens a submenu is listed, because a scenario reading
        // the menu bar should see the same rows a person does -- but firing it
        // opens a popup, which is not what `action` is for.
        {QStringLiteral("submenu"), action.menu() != nullptr},
    };
}

QJsonValue parseRpcPayload(const QByteArray &payload) {
    // The viewport serialises a bare JSON value; wrap it so QJsonDocument can
    // parse a scalar reply as readily as an object one.
    QByteArray wrapped;
    wrapped.reserve(payload.size() + 2);
    wrapped.append('[').append(payload).append(']');
    const QJsonDocument document = QJsonDocument::fromJson(wrapped);
    if (!document.isArray() || document.array().isEmpty()) return QJsonValue{};
    return document.array().at(0);
}

// --------------------------------------------------------------------------
// Commands

AutomationReply commandList(const AutomationHost &host,
                            const QJsonObject &request) {
    const QString pattern = request.value(QStringLiteral("pattern")).toString();
    const auto matches = [&pattern](const QString &name) {
        return pattern.isEmpty() ||
               name.contains(pattern, Qt::CaseInsensitive);
    };
    QJsonArray entries;
    for (QWidget *widget : namedWidgets()) {
        const QString name = widget->objectName();
        if (matches(name)) {
            Target target;
            target.widget = widget;
            target.owner = widget;
            QJsonObject entry{
                {QStringLiteral("name"), name},
                {QStringLiteral("class"),
                 QString::fromLatin1(widget->metaObject()->className())},
                {QStringLiteral("visible"), widget->isVisible()},
                {QStringLiteral("enabled"), widget->isEnabled()},
                {QStringLiteral("geometry"), rectJson(globalRect(target))},
                {QStringLiteral("text"), widgetText(*widget)},
                {QStringLiteral("shortcut"), widgetShortcut(*widget)},
            };
            // SK-1: a check box's text is its caption, so whether it is ticked
            // is read here, as an action's is.
            if (const auto *button = qobject_cast<const QAbstractButton *>(widget);
                button != nullptr && button->isCheckable())
                entry.insert(QStringLiteral("checked"), button->isChecked());
            entries.push_back(entry);
        }
        // Drawn items are addressable too: a value pill has no widget of its
        // own, and naming it by node and port is what lets a scenario click it
        // without knowing where the layout put it.
        auto *view = qobject_cast<QGraphicsView *>(widget);
        if (view == nullptr) continue;
        for (const auto &[item_name, item] : sceneItems(*view)) {
            if (!matches(item_name)) continue;
            Target target;
            target.view = view;
            target.item = item;
            target.widget = view->viewport();
            target.owner = view;
            entries.push_back(QJsonObject{
                {QStringLiteral("name"), item_name},
                {QStringLiteral("class"), QStringLiteral("QGraphicsItem")},
                {QStringLiteral("visible"), targetVisible(target)},
                {QStringLiteral("enabled"), true},
                {QStringLiteral("geometry"), rectJson(globalRect(target))},
                {QStringLiteral("scene"),
                 QJsonArray{item->sceneBoundingRect().center().x(),
                            item->sceneBoundingRect().center().y()}},
                {QStringLiteral("text"), itemText(*item)},
                {QStringLiteral("shortcut"), QString{}},
            });
        }
    }
    for (QAction *action : namedActions(host)) {
        if (!matches(action->objectName())) continue;
        entries.push_back(QJsonObject{
            {QStringLiteral("name"), action->objectName()},
            {QStringLiteral("class"), QStringLiteral("QAction")},
            {QStringLiteral("visible"), action->isVisible()},
            {QStringLiteral("enabled"), action->isEnabled()},
            {QStringLiteral("geometry"), rectJson(QRect{})},
            {QStringLiteral("checked"), action->isChecked()},
            {QStringLiteral("text"), action->text()},
        });
    }
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("count"), static_cast<int>(entries.size())},
        {QStringLiteral("widgets"), entries},
    });
}

AutomationReply commandClick(const AutomationHost &host,
                             const QJsonObject &request, bool double_click) {
    // A named action is done, not pressed: the menu it lives in need not be
    // open, and a scenario should not have to drive a popup to reach it.
    const QString named = request.value(QStringLiteral("name")).toString();
    if (!named.isEmpty() && findWidget(host, named) == nullptr) {
        if (QAction *action = findAction(host, named)) {
            if (!action->isEnabled())
                return AutomationReply::failure(
                    BadRequest,
                    QStringLiteral("the action named \"%1\" is disabled")
                        .arg(named));
            action->trigger();
            return AutomationReply::success(QJsonObject{
                {QStringLiteral("action"), named},
                {QStringLiteral("text"), action->text()},
                {QStringLiteral("checked"), action->isChecked()},
            });
        }
    }
    Target target;
    AutomationReply rejected;
    if (!gestureTarget(host, request, &target, &rejected)) return rejected;
    // Q-16: a switched-off widget is refused by name, as a disabled action
    // is. Qt drops the press without a word, so a click that reported
    // success here left the scenario to fail steps later on its consequence.
    if (!targetEnabled(target))
        return AutomationReply::failure(
            BadRequest,
            named.isEmpty()
                ? QStringLiteral("the window under the point is disabled")
                : QStringLiteral("the widget named \"%1\" is disabled").arg(named));
    bool ok = false;
    const Qt::MouseButton button =
        parseButton(request.value(QStringLiteral("button")), &ok);
    if (!ok)
        return AutomationReply::failure(
            BadRequest, QStringLiteral("button must be left, right or middle"));
    const Qt::KeyboardModifiers modifiers =
        parseModifiers(request.value(QStringLiteral("modifiers")), &ok);
    if (!ok)
        return AutomationReply::failure(
            BadRequest, QStringLiteral("unknown keyboard modifier"));
    QPoint point;
    if (!pointFor(request, target, &point, &rejected)) return rejected;
    const QRect where = globalRect(target);
    QWidget *receiver = pointerReceiver(target, &point);
    if (double_click)
        QTest::mouseDClick(receiver, button, modifiers, point);
    else
        QTest::mouseClick(receiver, button, modifiers, point);
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("point"), QJsonArray{point.x(), point.y()}},
        {QStringLiteral("geometry"), rectJson(where)},
        {QStringLiteral("visible"), targetVisible(target)},
    });
}

AutomationReply commandDrag(const AutomationHost &host,
                            const QJsonObject &request) {
    const auto endpoint = [&](const QString &key, Target *target,
                              QPoint *point,
                              AutomationReply *rejected) -> bool {
        const QJsonValue value = request.value(key);
        QJsonObject spec;
        if (value.isString())
            spec.insert(QStringLiteral("name"), value.toString());
        else if (value.isObject())
            spec = value.toObject();
        else {
            *rejected = AutomationReply::failure(
                BadRequest,
                QStringLiteral("%1 must be a name or an object").arg(key));
            return false;
        }
        if (!gestureTarget(host, spec, target, rejected)) return false;
        return pointFor(spec, *target, point, rejected);
    };
    Target from_target;
    Target to_target;
    QPoint from;
    QPoint to;
    AutomationReply rejected;
    if (!endpoint(QStringLiteral("from"), &from_target, &from, &rejected))
        return rejected;
    if (!endpoint(QStringLiteral("to"), &to_target, &to, &rejected))
        return rejected;
    if (from_target.widget != to_target.widget)
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("a drag must start and end in the same widget"));
    bool ok = false;
    const Qt::MouseButton button =
        parseButton(request.value(QStringLiteral("button")), &ok);
    if (!ok)
        return AutomationReply::failure(
            BadRequest, QStringLiteral("button must be left, right or middle"));
    const Qt::KeyboardModifiers modifiers =
        parseModifiers(request.value(QStringLiteral("modifiers")), &ok);
    if (!ok)
        return AutomationReply::failure(
            BadRequest, QStringLiteral("unknown keyboard modifier"));
    const int steps =
        std::clamp(request.value(QStringLiteral("steps")).toInt(8), 1, 200);
    QWidget *widget = pointerReceiver(from_target, &from);
    pointerReceiver(to_target, &to);
    sendMouse(widget, QEvent::MouseButtonPress, from, button, modifiers);
    for (int step = 1; step <= steps; ++step) {
        const QPoint interpolated{
            from.x() + (to.x() - from.x()) * step / steps,
            from.y() + (to.y() - from.y()) * step / steps};
        sendMouse(widget, QEvent::MouseMove, interpolated, Qt::NoButton,
                  modifiers);
    }
    sendMouse(widget, QEvent::MouseButtonRelease, to, button, modifiers);
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("from"), QJsonArray{from.x(), from.y()}},
        {QStringLiteral("to"), QJsonArray{to.x(), to.y()}},
        {QStringLiteral("button"),
         request.value(QStringLiteral("button")).toString(
             QStringLiteral("left"))},
        {QStringLiteral("steps"), steps},
    });
}

// `wheel`, `move`, `press` and `release`: the gestures a canvas is driven with.
// A zoom is a wheel, a pan is a button held across moves, and a hover is a move
// with nothing held -- none of which `click` and `drag` could say before.
AutomationReply commandWheel(const AutomationHost &host,
                             const QJsonObject &request) {
    Target target;
    AutomationReply rejected;
    if (!gestureTarget(host, request, &target, &rejected)) return rejected;
    const QJsonValue delta_value = request.value(QStringLiteral("delta"));
    const QJsonValue delta_x_value = request.value(QStringLiteral("delta_x"));
    if (!delta_value.isDouble() && !delta_x_value.isDouble())
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("delta is required (one notch forward is 120)"));
    bool ok = false;
    const Qt::KeyboardModifiers modifiers =
        parseModifiers(request.value(QStringLiteral("modifiers")), &ok);
    if (!ok)
        return AutomationReply::failure(
            BadRequest, QStringLiteral("unknown keyboard modifier"));
    QPoint point;
    if (!pointFor(request, target, &point, &rejected)) return rejected;
    QWidget *receiver = pointerReceiver(target, &point);
    const QPoint angle{delta_x_value.toInt(0), delta_value.toInt(0)};
    sendWheel(receiver, point, angle, modifiers);
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("point"), QJsonArray{point.x(), point.y()}},
        {QStringLiteral("delta"), angle.y()},
        {QStringLiteral("delta_x"), angle.x()},
        {QStringLiteral("target"), receiver->objectName()},
        {QStringLiteral("class"),
         QString::fromLatin1(receiver->metaObject()->className())},
    });
}

enum class PointerAction { move, press, release };

AutomationReply commandPointer(const AutomationHost &host,
                               const QJsonObject &request,
                               PointerAction action) {
    Target target;
    AutomationReply rejected;
    if (!gestureTarget(host, request, &target, &rejected)) return rejected;
    bool ok = false;
    Qt::MouseButton button = Qt::NoButton;
    if (action != PointerAction::move) {
        button = parseButton(request.value(QStringLiteral("button")), &ok);
        if (!ok)
            return AutomationReply::failure(
                BadRequest,
                QStringLiteral("button must be left, right or middle"));
    }
    const Qt::KeyboardModifiers modifiers =
        parseModifiers(request.value(QStringLiteral("modifiers")), &ok);
    if (!ok)
        return AutomationReply::failure(
            BadRequest, QStringLiteral("unknown keyboard modifier"));
    QPoint point;
    if (!pointFor(request, target, &point, &rejected)) return rejected;
    const QEvent::Type type = action == PointerAction::move
                                  ? QEvent::MouseMove
                                  : (action == PointerAction::press
                                         ? QEvent::MouseButtonPress
                                         : QEvent::MouseButtonRelease);
    QWidget *receiver = pointerReceiver(target, &point);
    sendMouse(receiver, type, point, button, modifiers);
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("point"), QJsonArray{point.x(), point.y()}},
        {QStringLiteral("buttons"), buttonNames(heldButtons())},
        {QStringLiteral("target"), receiver->objectName()},
        {QStringLiteral("visible"), targetVisible(target)},
    });
}

AutomationReply commandViewState(const AutomationHost &host,
                                 const QJsonObject &request) {
    const QString name = request.value(QStringLiteral("name")).toString();
    if (name.isEmpty())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("name is required"));
    const Target target = resolveTarget(host, name);
    if (!target.valid())
        return AutomationReply::failure(
            WidgetMissing,
            QStringLiteral("no widget or drawn item is named \"%1\"").arg(name));
    QGraphicsView *view = target.view;
    if (view == nullptr)
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("\"%1\" is a %2, not a QGraphicsView")
                .arg(name, QString::fromLatin1(
                               target.owner->metaObject()->className())));
    const QTransform transform = view->transform();
    const QScrollBar *horizontal = view->horizontalScrollBar();
    const QScrollBar *vertical = view->verticalScrollBar();
    const QRect viewport = view->viewport()->rect();
    const QPointF centre = view->mapToScene(viewport.center());
    const QRectF visible = view->mapToScene(viewport).boundingRect();
    const QRectF scene_rect =
        view->scene() != nullptr ? view->scene()->sceneRect() : QRectF{};
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("name"), name},
        // What a zoom moves. The two axes are reported apart so a view that
        // scales them differently cannot hide behind one number.
        {QStringLiteral("scale"), transform.m11()},
        {QStringLiteral("scale_y"), transform.m22()},
        {QStringLiteral("transform"),
         QJsonArray{transform.m11(), transform.m12(), transform.m21(),
                    transform.m22(), transform.dx(), transform.dy()}},
        // What a pan moves. `center` is the scene point in the middle of the
        // viewport: it answers for a pan however the view implements one --
        // scrollbars, a translated transform, or a changed scene rect.
        {QStringLiteral("center"), QJsonArray{centre.x(), centre.y()}},
        {QStringLiteral("scroll"),
         QJsonObject{
             {QStringLiteral("x"), horizontal->value()},
             {QStringLiteral("y"), vertical->value()},
             {QStringLiteral("x_min"), horizontal->minimum()},
             {QStringLiteral("x_max"), horizontal->maximum()},
             {QStringLiteral("y_min"), vertical->minimum()},
             {QStringLiteral("y_max"), vertical->maximum()},
         }},
        {QStringLiteral("visible_scene"),
         QJsonArray{visible.x(), visible.y(), visible.width(),
                    visible.height()}},
        {QStringLiteral("scene_rect"),
         QJsonArray{scene_rect.x(), scene_rect.y(), scene_rect.width(),
                    scene_rect.height()}},
        {QStringLiteral("viewport"),
         QJsonArray{viewport.width(), viewport.height()}},
        {QStringLiteral("items"),
         view->scene() != nullptr
             ? static_cast<int>(view->scene()->items().size())
             : 0},
    });
}

// What the automatic placement achieved on the scene a named view shows. The
// scene publishes it once per draw (counting it is free -- the ordering stage
// counts crossings anyway), so a scenario can ask a live window how tangled
// the drawing is instead of a reader having to look at a screenshot.
AutomationReply commandLayoutStats(const AutomationHost &host,
                                   const QJsonObject &request) {
    const QString name = request.value(QStringLiteral("name")).toString();
    if (name.isEmpty())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("name is required"));
    const Target target = resolveTarget(host, name);
    if (!target.valid())
        return AutomationReply::failure(
            WidgetMissing,
            QStringLiteral("no widget or drawn item is named \"%1\"").arg(name));
    QGraphicsView *view = target.view;
    if (view == nullptr || view->scene() == nullptr)
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("\"%1\" is not a QGraphicsView showing a scene")
                .arg(name));
    const QGraphicsScene &scene = *view->scene();
    // A scene that has not drawn yet answers -1 rather than 0: "nothing
    // crosses" and "nobody has asked yet" are different answers.
    const auto number = [](const QVariant &value) {
        return value.isValid() ? static_cast<double>(value.toULongLong())
                               : -1.0;
    };
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("name"), name},
        {QStringLiteral("crossings"),
         number(scene.property("pelicanLayoutCrossings"))},
        {QStringLiteral("overlaps"),
         number(scene.property("pelicanLayoutOverlaps"))},
        {QStringLiteral("nodes"),
         number(scene.property("pelicanLayoutNodes"))},
        {QStringLiteral("reversed_edges"),
         number(scene.property("pelicanLayoutReversedEdges"))},
    });
}

AutomationReply commandMenuList(const AutomationHost &host,
                                const QJsonObject &request) {
    QWidget *root = nullptr;
    const QString name = request.value(QStringLiteral("name")).toString();
    if (!name.isEmpty()) {
        const Target target = resolveTarget(host, name);
        if (!target.valid() || target.owner == nullptr)
            return AutomationReply::failure(
                WidgetMissing,
                QStringLiteral("no widget is named \"%1\"").arg(name));
        root = target.owner;
    }
    const QString pattern = request.value(QStringLiteral("pattern")).toString();
    QJsonArray entries;
    for (const ActionEntry &entry : collectActions(host, root)) {
        if (!pattern.isEmpty() &&
            !actionName(*entry.action).contains(pattern, Qt::CaseInsensitive) &&
            !withoutMnemonic(entry.action->text())
                 .contains(pattern, Qt::CaseInsensitive))
            continue;
        entries.push_back(actionJson(entry));
    }
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("count"), static_cast<int>(entries.size())},
        {QStringLiteral("actions"), entries},
    });
}

AutomationReply commandAction(const AutomationHost &host,
                              const QJsonObject &request) {
    const QString name = request.value(QStringLiteral("name")).toString();
    const QString text = request.value(QStringLiteral("text")).toString();
    const QString menu = request.value(QStringLiteral("menu")).toString();
    if (name.isEmpty() && text.isEmpty())
        return AutomationReply::failure(
            BadRequest, QStringLiteral("name or text is required"));
    const std::vector<ActionEntry> all = collectActions(host, nullptr);
    const auto in_menu = [&menu](const ActionEntry &entry) {
        return menu.isEmpty() || entry.menu.contains(menu, Qt::CaseInsensitive);
    };
    std::vector<ActionEntry> matched;
    for (const ActionEntry &entry : all) {
        if (!in_menu(entry)) continue;
        if (!name.isEmpty()) {
            if (actionName(*entry.action) == name) matched.push_back(entry);
            continue;
        }
        if (withoutMnemonic(entry.action->text())
                .compare(text, Qt::CaseInsensitive) == 0)
            matched.push_back(entry);
    }
    // An exact label is the first question; a person who typed half of one gets
    // the same answer as long as only one row can be meant.
    if (matched.empty() && name.isEmpty()) {
        for (const ActionEntry &entry : all) {
            if (!in_menu(entry)) continue;
            if (withoutMnemonic(entry.action->text())
                    .contains(text, Qt::CaseInsensitive))
                matched.push_back(entry);
        }
    }
    if (matched.empty())
        return AutomationReply::failure(
            WidgetMissing,
            QStringLiteral("no menu action answers to \"%1\"%2")
                .arg(name.isEmpty() ? text : name,
                     menu.isEmpty() ? QString{}
                                    : QStringLiteral(" under \"%1\"").arg(menu)));
    if (matched.size() > 1) {
        QStringList names;
        for (const ActionEntry &entry : matched)
            names.push_back(QStringLiteral("%1 (%2)")
                                .arg(withoutMnemonic(entry.action->text()),
                                     actionName(*entry.action).isEmpty()
                                         ? entry.menu
                                         : actionName(*entry.action)));
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("\"%1\" names %2 actions: %3")
                .arg(name.isEmpty() ? text : name)
                .arg(matched.size())
                .arg(names.join(QStringLiteral(", "))));
    }
    const ActionEntry &entry = matched.front();
    if (!entry.action->isEnabled())
        return AutomationReply::failure(
            BadRequest, QStringLiteral("the action \"%1\" is disabled")
                            .arg(withoutMnemonic(entry.action->text())));
    const bool was_checked = entry.action->isChecked();
    // Fired where it lives, with no popup in between: opening the menu would
    // enter a modal event loop, and this command has a reply to finish writing.
    entry.action->trigger();
    QCoreApplication::processEvents();
    QJsonObject result = actionJson(entry);
    result.insert(QStringLiteral("was_checked"), was_checked);
    result.insert(QStringLiteral("triggered"), true);
    return AutomationReply::success(result);
}

// The widget a keystroke goes to when the request does not name one: whatever
// the window says has focus. QApplication::focusWidget() answers only while the
// window is active, which a test machine cannot promise.
QWidget *focusTarget(const AutomationHost &host) {
    // A widget embedded in a QGraphicsProxyWidget is not in the window's child
    // chain, so the application's own answer comes first.
    if (QWidget *focus = QApplication::focusWidget()) return focus;
    if (host.window != nullptr) {
        if (QWidget *focus = host.window->focusWidget()) return focus;
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (!widget->isVisible()) continue;
            if (QWidget *focus = widget->focusWidget()) return focus;
        }
    }
    return QApplication::focusWidget();
}

AutomationReply commandKey(const AutomationHost &host,
                           const QJsonObject &request) {
    const QString sequence =
        request.value(QStringLiteral("sequence")).toString();
    if (sequence.isEmpty())
        return AutomationReply::failure(
            BadRequest, QStringLiteral("sequence is required"));
    QWidget *widget = nullptr;
    const QString name = request.value(QStringLiteral("name")).toString();
    if (!name.isEmpty()) {
        const Target target = resolveTarget(host, name);
        if (!target.valid())
            return AutomationReply::failure(
                WidgetMissing,
                QStringLiteral("no widget or drawn item is named \"%1\"")
                    .arg(name));
        QWidget *embedded = embeddedWidget(target);
        widget = embedded != nullptr ? embedded : target.widget;
    } else {
        widget = focusTarget(host);
    }
    if (widget == nullptr)
        return AutomationReply::failure(
            WidgetMissing, QStringLiteral("nothing in Studio holds focus"));
    const QKeySequence parsed =
        QKeySequence::fromString(sequence, QKeySequence::PortableText);
    if (parsed.count() == 0)
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("\"%1\" is not a key sequence").arg(sequence));
    // Two routes, because they reach different code. Straight to the widget is
    // what an editor sees, and it does not depend on which window the desktop
    // has activated -- that is why `graph_edit_roundtrip` can press Return into
    // a drawn editor at all. Through the window is the desktop's own route: it
    // is the one that passes the shortcut map, so it is the only one that can
    // fire a QAction by its shortcut.
    const QString route =
        request.value(QStringLiteral("route")).toString(QStringLiteral("widget"));
    if (route != QLatin1String("widget") && route != QLatin1String("window"))
        return AutomationReply::failure(
            BadRequest, QStringLiteral("route must be widget or window"));
    QWindow *handle = nullptr;
    if (route == QLatin1String("window")) {
        QWidget *top = widget->window();
        handle = top != nullptr ? top->windowHandle() : nullptr;
        if (handle == nullptr)
            return AutomationReply::failure(
                WidgetMissing,
                QStringLiteral("\"%1\" is in no window the desktop knows, so "
                               "keys cannot take the shortcut route")
                    .arg(widget->objectName()));
    }
    for (int index = 0; index < parsed.count(); ++index) {
        const QKeyCombination combination = parsed[index];
        if (handle != nullptr)
            QTest::keyClick(handle, combination.key(),
                            combination.keyboardModifiers());
        else
            QTest::keyClick(widget, combination.key(),
                            combination.keyboardModifiers());
    }
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("sequence"), parsed.toString(QKeySequence::PortableText)},
        {QStringLiteral("route"), route},
        {QStringLiteral("target"), widget->objectName()},
        {QStringLiteral("class"),
         QString::fromLatin1(widget->metaObject()->className())},
    });
}

AutomationReply commandType(const AutomationHost &host,
                            const QJsonObject &request) {
    const QJsonValue text_value = request.value(QStringLiteral("text"));
    if (!text_value.isString())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("text is required"));
    QWidget *widget = nullptr;
    const QString name = request.value(QStringLiteral("name")).toString();
    if (!name.isEmpty()) {
        const Target target = resolveTarget(host, name);
        if (!target.valid())
            return AutomationReply::failure(
                WidgetMissing,
                QStringLiteral("no widget or drawn item is named \"%1\"")
                    .arg(name));
        QWidget *embedded = embeddedWidget(target);
        widget = embedded != nullptr ? embedded : target.widget;
        // A view is a conduit, not a destination: its scene already decided
        // which drawn editor has the keyboard, and taking focus for the view
        // would be taking it away from that editor.
        if (qobject_cast<QGraphicsView *>(widget) == nullptr)
            widget->setFocus(Qt::OtherFocusReason);
    } else {
        widget = focusTarget(host);
    }
    if (widget == nullptr)
        return AutomationReply::failure(
            WidgetMissing, QStringLiteral("nothing in Studio holds focus"));
    // An editor that opens over what it edits already holds a value. Typing is
    // meant to replace it, the way a human's first keystroke does over a
    // selection, so the default is to select what is there first.
    const bool select_all =
        request.value(QStringLiteral("select_all")).toBool(true);
    if (select_all) {
        if (auto *line = qobject_cast<QLineEdit *>(widget)) line->selectAll();
        else if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget))
            spin->selectAll();
        else
            QTest::keyClick(widget, Qt::Key_A, Qt::ControlModifier);
    }
    QTest::keyClicks(widget, text_value.toString());
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("target"), widget->objectName()},
        {QStringLiteral("class"),
         QString::fromLatin1(widget->metaObject()->className())},
        {QStringLiteral("text"), widgetText(*widget)},
    });
}

AutomationReply commandText(const AutomationHost &host,
                            const QJsonObject &request) {
    Target target;
    AutomationReply rejected;
    if (!gestureTarget(host, request, &target, &rejected)) return rejected;
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("name"), request.value(QStringLiteral("name"))},
        {QStringLiteral("text"), targetText(target)},
        {QStringLiteral("visible"), targetVisible(target)},
    });
}

// AG-3b. Choosing a row of a list or an entry of a combo box.
//
// For an item view this is a real click: the row's own rectangle is asked for
// and the press lands in it, so the view's selection model, its signals and
// anything watching them run exactly as they do under a finger. For a combo
// box it is not, and cannot be: the entries live in a popup, and a popup is a
// modal event loop, which a command that owes a reply on this line cannot
// enter -- the same reason `action` fires a menu entry rather than opening the
// menu. setCurrentIndex emits the combo's own currentIndexChanged, which is
// the signal a popup choice emits too, so the receiving code cannot tell the
// two apart.
//
// QA-1: many combos and lists are filled by an answer of the engine's
// (list_surfaces, get_material_bindings, ...), and the answer arrives on the
// engine's clock, not the driver's. A select that refused the moment the
// entry was not there lost a race the scenario could not see: the step before
// had waited for the row to be on screen, and its entries came a few
// milliseconds later. So a widget that is not there yet, or an entry it does
// not hold yet, is waited for with the event loop running -- as `wait` does --
// up to timeout_ms (SelectWaitMs by default), and the refusal says how long it
// waited and what the widget held then. A widget that is there and switched
// off is still refused at once (Q-17): waiting for it to come on would let a
// select through at a moment a person could not have made it.
constexpr int SelectWaitMs = 5000;

// What a combo or a list holds, for a refusal: the first entries, in order.
QString entriesOf(const QStringList &entries) {
    constexpr int Shown = 40;
    if (entries.isEmpty()) return QStringLiteral("nothing");
    QStringList quoted;
    for (int at = 0; at < entries.size() && at < Shown; ++at)
        quoted.push_back(QStringLiteral("\"%1\"").arg(entries[at]));
    QString line = quoted.join(QStringLiteral(", "));
    if (entries.size() > Shown)
        line += QStringLiteral(" and %1 more").arg(entries.size() - Shown);
    return line;
}

AutomationReply commandSelect(const AutomationHost &host,
                              const QJsonObject &request) {
    const QString name = request.value(QStringLiteral("name")).toString();
    if (name.isEmpty())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("name is required"));
    const QJsonValue text = request.value(QStringLiteral("text"));
    const QJsonValue index = request.value(QStringLiteral("index"));
    if (!text.isString() && !index.isDouble())
        return AutomationReply::failure(
            BadRequest, QStringLiteral("select needs text or index"));
    const QString wanted_entry =
        text.isString() ? text.toString() : QString::number(index.toInt());
    const int timeout = timeoutOf(request, SelectWaitMs);
    QElapsedTimer elapsed;
    elapsed.start();

    // What the last look found, for the refusal once the time is up.
    bool found = false;
    bool is_combo = false;
    QStringList held;
    for (;;) {
        // waited_ms is the time until the entry was found, not counting what
        // the choice itself then set off in the panel.
        const double waited = static_cast<double>(elapsed.elapsed());
        QWidget *widget = findWidget(host, name);
        found = widget != nullptr;
        if (widget != nullptr) {
            // Q-17: a switched-off widget is refused by name, as click refuses
            // one (Q-16). A person cannot pick in it; setCurrentIndex could,
            // and the panel behind it had said "not now" by switching it off.
            if (!widget->isEnabled())
                return AutomationReply::failure(
                    BadRequest,
                    QStringLiteral("the widget named \"%1\" is disabled").arg(name));

            if (auto *combo = qobject_cast<QComboBox *>(widget)) {
                is_combo = true;
                const int row = index.isDouble()
                                    ? index.toInt(-1)
                                    : combo->findText(text.toString());
                if (row >= 0 && row < combo->count()) {
                    const QString wanted = combo->itemText(row);
                    // The combo is held while its own signal runs: a panel
                    // that rebuilds its rows from here leaves this one to go
                    // when the event loop comes back, and it is read below.
                    const QPointer<QComboBox> kept{combo};
                    combo->setCurrentIndex(row);
                    // Q-17: the choice has to still be there once the panel
                    // has heard it. A panel that refused it puts the value it
                    // holds back, and a select that answered "ok" over that
                    // let the scenario go on as if it had chosen, and fail
                    // steps later on what the choice never did. A picker -- a
                    // list that hands its entry to a field and goes back to
                    // its placeholder, "Pick..." -- holds no entry afterwards
                    // (index -1): it took the choice by using it. Any other
                    // entry is not the one chosen.
                    if (!kept.isNull() && kept->currentIndex() >= 0 &&
                        (kept->currentIndex() != row ||
                         kept->currentText() != wanted))
                        return AutomationReply::failure(
                            BadRequest,
                            QStringLiteral("\"%1\" did not take \"%2\"; it holds \"%3\"")
                                .arg(name, wanted, kept->currentText()));
                    return AutomationReply::success(QJsonObject{
                        {QStringLiteral("name"), name},
                        {QStringLiteral("index"), row},
                        {QStringLiteral("text"), wanted},
                        {QStringLiteral("waited_ms"), waited},
                    });
                }
                held.clear();
                for (int at = 0; at < combo->count(); ++at)
                    held.push_back(combo->itemText(at));
            } else {
                is_combo = false;
                auto *view = qobject_cast<QAbstractItemView *>(widget);
                if (view == nullptr || view->model() == nullptr)
                    return AutomationReply::failure(
                        BadRequest,
                        QStringLiteral("\"%1\" is not a list, a tree or a combo box")
                            .arg(name));
                // Every index the view can show, in the order it shows them,
                // so `index` means the row a reader counts down the panel
                // rather than a row of some one level of the tree. Only a
                // tree has folded rows; a flat view's children are all there
                // is.
                auto *tree = qobject_cast<QTreeView *>(view);
                std::vector<QModelIndex> rows;
                const std::function<void(const QModelIndex &)> walk =
                    [&](const QModelIndex &parent) {
                        const int count = view->model()->rowCount(parent);
                        for (int row = 0; row < count; ++row) {
                            const QModelIndex child =
                                view->model()->index(row, 0, parent);
                            if (!child.isValid()) continue;
                            rows.push_back(child);
                            if (tree != nullptr && tree->isExpanded(child))
                                walk(child);
                        }
                    };
                walk(QModelIndex{});
                QModelIndex chosen;
                if (index.isDouble()) {
                    const int wanted = index.toInt(-1);
                    if (wanted >= 0 && wanted < static_cast<int>(rows.size()))
                        chosen = rows[static_cast<std::size_t>(wanted)];
                } else {
                    for (const QModelIndex &row : rows)
                        if (row.data(Qt::DisplayRole).toString() ==
                            text.toString()) {
                            chosen = row;
                            break;
                        }
                }
                if (chosen.isValid()) {
                    view->scrollTo(chosen);
                    const QRect rect = view->visualRect(chosen);
                    if (rect.isEmpty())
                        return AutomationReply::failure(
                            BadRequest,
                            QStringLiteral("the row \"%1\" of \"%2\" is not on screen")
                                .arg(chosen.data(Qt::DisplayRole).toString(), name));
                    const QPoint point = rect.center();
                    QTest::mouseClick(view->viewport(), Qt::LeftButton,
                                      Qt::NoModifier, point);
                    return AutomationReply::success(QJsonObject{
                        {QStringLiteral("name"), name},
                        {QStringLiteral("text"),
                         chosen.data(Qt::DisplayRole).toString()},
                        {QStringLiteral("point"),
                         QJsonArray{point.x(), point.y()}},
                        {QStringLiteral("selected"),
                         view->currentIndex() == chosen},
                        {QStringLiteral("waited_ms"), waited},
                    });
                }
                held.clear();
                for (const QModelIndex &row : rows)
                    held.push_back(row.data(Qt::DisplayRole).toString());
            }
        }
        if (elapsed.elapsed() >= timeout) break;
        QTest::qWait(25);
    }
    if (!found)
        return AutomationReply::failure(
            WidgetMissing,
            QStringLiteral("no widget is named \"%1\" (waited %2 ms)")
                .arg(name)
                .arg(elapsed.elapsed()));
    return AutomationReply::failure(
        BadRequest,
        QStringLiteral("\"%1\" %2 %3 (waited %4 ms; it holds %5)")
            .arg(name,
                 is_combo ? QStringLiteral("has no entry")
                          : QStringLiteral("holds no row"),
                 wanted_entry)
            .arg(elapsed.elapsed())
            .arg(entriesOf(held)));
}

AutomationReply commandWait(const AutomationHost &host,
                            const QJsonObject &request) {
    const QString name = request.value(QStringLiteral("name")).toString();
    if (name.isEmpty())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("name is required"));
    const int timeout = timeoutOf(request, 10000);
    const QJsonValue visible = request.value(QStringLiteral("visible"));
    const QJsonValue text = request.value(QStringLiteral("text"));
    const QJsonValue contains = request.value(QStringLiteral("contains"));
    const QJsonValue missing = request.value(QStringLiteral("missing"));
    // AG-3b: a button that is on screen but switched off is the ordinary way a
    // panel says "not yet". A scenario that clicks it anyway does nothing and
    // fails three steps later, naming the wrong thing.
    const QJsonValue enabled = request.value(QStringLiteral("enabled"));
    if (!visible.isBool() && !text.isString() && !contains.isString() &&
        !missing.isBool() && !enabled.isBool())
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("wait needs one of visible, enabled, text, contains "
                           "or missing"));

    QString last_text;
    bool last_visible = false;
    bool last_found = false;
    bool last_enabled = false;
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        const Target target = resolveTarget(host, name);
        last_found = target.valid();
        last_visible = last_found && targetVisible(target);
        last_text = last_found ? targetText(target) : QString{};
        last_enabled = last_found && target.owner != nullptr &&
                       target.owner->isEnabled();
        bool satisfied = true;
        if (missing.isBool()) satisfied = satisfied && (last_found != missing.toBool());
        if (visible.isBool()) satisfied = satisfied && last_visible == visible.toBool();
        if (enabled.isBool())
            satisfied = satisfied && last_enabled == enabled.toBool();
        if (text.isString()) satisfied = satisfied && last_found && last_text == text.toString();
        if (contains.isString())
            satisfied = satisfied && last_found &&
                        last_text.contains(contains.toString());
        if (satisfied)
            return AutomationReply::success(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("text"), last_text},
                {QStringLiteral("visible"), last_visible},
                {QStringLiteral("enabled"), last_enabled},
                {QStringLiteral("waited_ms"),
                 static_cast<double>(elapsed.elapsed())},
            });
        if (elapsed.elapsed() >= timeout) break;
        QTest::qWait(25);
    }
    return AutomationReply::failure(
        WaitTimeout,
        QStringLiteral("\"%1\" did not reach the expected state in %2 ms "
                       "(found=%3 visible=%4 enabled=%5 text=\"%6\")")
            .arg(name)
            .arg(timeout)
            .arg(last_found ? QStringLiteral("true") : QStringLiteral("false"),
                 last_visible ? QStringLiteral("true") : QStringLiteral("false"),
                 last_enabled ? QStringLiteral("true") : QStringLiteral("false"),
                 last_text));
}

// One engine RPC, answered synchronously on the automation thread's event
// loop. Returns the parsed result, or an AutomationReply failure.
struct SyncRpcOutcome {
    std::optional<QJsonValue> result;
    std::optional<AutomationReply> failure;
};

SyncRpcOutcome callEngineRpc(const AutomationHost &host, const QString &method,
                             const QJsonObject &params, int timeout_ms) {
    QString error;
    const qint64 id = host.viewport->requestRpc(method, params, &error);
    if (id == 0)
        return {.failure = AutomationReply::failure(
                    EngineNotReady,
                    error.isEmpty() ? QStringLiteral("the engine refused the request")
                                    : error)};

    QEventLoop loop;
    bool answered = false;
    bool succeeded = false;
    QByteArray payload;
    QString message;
    const auto on_result = QObject::connect(
        host.viewport, &EmbeddedViewport::inspectorRpcSucceeded, &loop,
        [&](qint64 reply_id, const QByteArray &result_json) {
            if (reply_id != id) return;
            answered = true;
            succeeded = true;
            payload = result_json;
            loop.quit();
        });
    const auto on_failure = QObject::connect(
        host.viewport, &EmbeddedViewport::inspectorRpcFailed, &loop,
        [&](qint64 reply_id, const QString &reason) {
            if (reply_id != id) return;
            answered = true;
            succeeded = false;
            message = reason;
            loop.quit();
        });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeout_ms);
    loop.exec();
    QObject::disconnect(on_result);
    QObject::disconnect(on_failure);

    if (!answered)
        return {.failure = AutomationReply::failure(
                    RpcFailed, QStringLiteral("%1 did not answer within %2 ms")
                                   .arg(method)
                                   .arg(timeout_ms))};
    if (!succeeded)
        return {.failure = AutomationReply::failure(
                    RpcFailed, QStringLiteral("%1 failed: %2").arg(method, message))};
    return {.result = parseRpcPayload(payload)};
}

// UX-7, decision 10: get_status's render.published_frames, read from the
// viewport's one parse of the reply (engineStatusParsed) rather than parsed
// again here: `wait_engine` asks every 50 ms, and a large graph's status is
// megabytes.
struct PublishedFrames {
    qint64 published = -1;
    std::optional<AutomationReply> failure;
};

PublishedFrames publishedFrames(const AutomationHost &host, int timeout_ms) {
    QString error;
    const qint64 id = host.viewport->requestRpc(QStringLiteral("get_status"), QJsonObject{}, &error);
    if (id == 0)
        return {.failure = AutomationReply::failure(
                    EngineNotReady,
                    error.isEmpty() ? QStringLiteral("the engine refused the request") : error)};
    QEventLoop loop;
    bool answered = false;
    bool succeeded = false;
    qint64 published = -1;
    QString message;
    const auto on_status = QObject::connect(
        host.viewport, &EmbeddedViewport::engineStatusParsed, &loop,
        [&](qint64 reply_id, const nlohmann::json &status) {
            if (reply_id != id) return;
            answered = true;
            succeeded = true;
            const auto render = status.is_object() ? status.find("render") : status.end();
            if (status.is_object() && render != status.end() && render->is_object()) {
                const auto frames = render->find("published_frames");
                if (frames != render->end() && frames->is_number())
                    published = static_cast<qint64>(frames->get<double>());
            }
            loop.quit();
        });
    const auto on_failure = QObject::connect(
        host.viewport, &EmbeddedViewport::inspectorRpcFailed, &loop,
        [&](qint64 reply_id, const QString &reason) {
            if (reply_id != id) return;
            answered = true;
            message = reason;
            loop.quit();
        });
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeout_ms);
    if (!answered) loop.exec();
    QObject::disconnect(on_status);
    QObject::disconnect(on_failure);
    if (!answered)
        return {.failure = AutomationReply::failure(
                    RpcFailed, QStringLiteral("get_status did not answer within %1 ms").arg(timeout_ms))};
    if (!succeeded)
        return {.failure = AutomationReply::failure(
                    RpcFailed, QStringLiteral("get_status failed: %1").arg(message))};
    return {.published = published};
}

// `wait_engine` answers once the engine takes editor RPC. With `frames` it
// also waits until the engine has published that many logical frames
// (get_status render.published_frames): the engine answers RPC before its
// first frame, and capture or a published frame plan need a drawn one.
// Q-9: "the engine" is the one running now, and not one a restart has been
// asked to replace. Between a commit's restart_required and the stop, the old
// engine still answers (Studio asks it about unsaved edits); agreeing with it
// here let the next step find it gone.
AutomationReply commandWaitEngine(const AutomationHost &host,
                                  const QJsonObject &request) {
    if (host.viewport == nullptr)
        return AutomationReply::failure(
            EngineNotReady, QStringLiteral("this window has no engine viewport"));
    const int timeout = timeoutOf(request, 60000);
    const QJsonValue frames_value = request.value(QStringLiteral("frames"));
    if (!frames_value.isUndefined() &&
        (!frames_value.isDouble() || frames_value.toDouble() < 0 ||
         frames_value.toDouble() != std::floor(frames_value.toDouble())))
        return AutomationReply::failure(
            BadRequest, QStringLiteral("frames must be a non-negative integer"));
    const auto frames = static_cast<qint64>(frames_value.toDouble(0));
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        if (host.viewport->currentEngineReady()) break;
        if (elapsed.elapsed() >= timeout)
            return AutomationReply::failure(
                EngineNotReady,
                QStringLiteral("the engine viewport was not ready for editor RPC "
                               "within %1 ms")
                    .arg(timeout));
        QTest::qWait(50);
    }
    qint64 published = -1;
    while (frames > 0) {
        const auto remaining =
            std::max<qint64>(1, timeout - elapsed.elapsed());
        auto status = publishedFrames(host, static_cast<int>(remaining));
        if (status.failure) return *status.failure;
        published = status.published;
        if (published < 0)
            return AutomationReply::failure(
                RpcFailed, QStringLiteral("get_status has no render.published_frames"));
        if (published >= frames) break;
        if (elapsed.elapsed() >= timeout)
            return AutomationReply::failure(
                EngineNotReady,
                QStringLiteral("the engine published %1 of %2 frames within %3 ms")
                    .arg(published)
                    .arg(frames)
                    .arg(timeout));
        QTest::qWait(50);
    }
    QJsonObject reply{
        {QStringLiteral("ready"), true},
        {QStringLiteral("waited_ms"), static_cast<double>(elapsed.elapsed())},
        // MR-1s: which engine process answered, so a scenario can say none
        // was started in between.
        {QStringLiteral("process_id"), static_cast<double>(host.viewport->engineProcessId())},
    };
    if (frames > 0)
        reply.insert(QStringLiteral("published_frames"), static_cast<double>(published));
    return AutomationReply::success(reply);
}

AutomationReply commandRpc(const AutomationHost &host,
                           const QJsonObject &request) {
    if (host.viewport == nullptr)
        return AutomationReply::failure(
            EngineNotReady, QStringLiteral("this window has no engine viewport"));
    const QString method = request.value(QStringLiteral("method")).toString();
    if (method.isEmpty())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("method is required"));
    const QJsonValue params_value = request.value(QStringLiteral("params"));
    if (!params_value.isUndefined() && !params_value.isObject())
        return AutomationReply::failure(
            BadRequest, QStringLiteral("params must be an object"));
    auto outcome = callEngineRpc(host, method, params_value.toObject(),
                                 timeoutOf(request));
    if (outcome.failure) return *outcome.failure;
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("method"), method},
        {QStringLiteral("result"), *outcome.result},
    });
}

// PX-2: give the engine's viewport one size for a whole scenario. A point a
// scenario reads back from the picture is a fraction of its width and height,
// and the camera's vertical field of view is fixed: when the window's panels
// change the viewport's shape, a point written "beside the box" lands on it.
// So the viewport is held at `width` x `height` picture pixels -- what the
// engine draws into, and what a screenshot of it measures -- and the window
// grows or shrinks by the same amount, leaving every panel around it as it
// was. Opening or closing a panel later takes its room from the panels or the
// window, never from the viewport. `free` gives it back to the layout.
AutomationReply commandViewportSize(const AutomationHost &host,
                                    const QJsonObject &request) {
    const QString name = request.value(QStringLiteral("name"))
                             .toString(QStringLiteral("pelican.viewportHost"));
    const Target target = resolveTarget(host, name);
    if (!target.valid() || target.isSceneItem() || target.row)
        return AutomationReply::failure(
            WidgetMissing, QStringLiteral("no widget is named \"%1\"").arg(name));
    QWidget *surface = target.widget;
    QWidget *window = surface->window();
    // What the layout gave it before the first fix, so `free` returns that
    // rather than a guess.
    const char *kept_minimum = "pelicanAutomationFreeMinimumSize";
    const char *kept_maximum = "pelicanAutomationFreeMaximumSize";
    if (!surface->property(kept_minimum).isValid()) {
        surface->setProperty(kept_minimum, surface->minimumSize());
        surface->setProperty(kept_maximum, surface->maximumSize());
    }
    const QSize before = surface->size();
    QSize wanted;
    if (request.value(QStringLiteral("free")).toBool(false)) {
        surface->setMinimumSize(surface->property(kept_minimum).toSize());
        surface->setMaximumSize(surface->property(kept_maximum).toSize());
    } else {
        const int width = request.value(QStringLiteral("width")).toInt(0);
        const int height = request.value(QStringLiteral("height")).toInt(0);
        if (width <= 0 || height <= 0)
            return AutomationReply::failure(
                BadRequest,
                QStringLiteral("width and height are required, in picture "
                               "pixels, above 0 (or free: true)"));
        // Picture pixels are physical; the layout is in Qt's logical pixels.
        const qreal ratio = surface->devicePixelRatioF();
        wanted = QSize{std::max(1, qRound(width / ratio)),
                       std::max(1, qRound(height / ratio))};
        surface->setFixedSize(wanted);
        if (window != surface && !window->isMaximized() &&
            !window->isFullScreen())
            window->resize(window->size() + (wanted - before));
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const int settle = timeoutOf(request, 5000);
    QCoreApplication::processEvents();
    while (wanted.isValid() && surface->size() != wanted &&
           elapsed.elapsed() < settle)
        QTest::qWait(25);
    const QSize logical = surface->size();
    const QSize picture =
        embeddedViewportPixelExtent(logical, surface->devicePixelRatioF());
    const QJsonObject reply{
        {QStringLiteral("name"), name},
        {QStringLiteral("fixed"), wanted.isValid()},
        // The size the engine is given, as embedded_viewport asks for it.
        {QStringLiteral("width"), picture.width()},
        {QStringLiteral("height"), picture.height()},
        {QStringLiteral("logical"), QJsonArray{logical.width(), logical.height()}},
        {QStringLiteral("device_pixel_ratio"), surface->devicePixelRatioF()},
        {QStringLiteral("window"),
         QJsonArray{window->width(), window->height()}},
    };
    if (wanted.isValid() && logical != wanted)
        return AutomationReply::failure(
            WaitTimeout,
            QStringLiteral("the viewport stayed %1x%2 instead of %3x%4")
                .arg(logical.width())
                .arg(logical.height())
                .arg(wanted.width())
                .arg(wanted.height()));
    return AutomationReply::success(reply);
}

AutomationReply commandOpenProject(const AutomationHost &host,
                                   const QJsonObject &request) {
    const QString directory = request.value(QStringLiteral("dir")).toString();
    if (directory.isEmpty())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("dir is required"));
    if (!QFileInfo(directory).isDir())
        return AutomationReply::failure(
            ProjectFailed,
            QStringLiteral("\"%1\" is not a directory").arg(directory));
    if (!host.open_project)
        return AutomationReply::failure(
            ProjectFailed,
            QStringLiteral("this window cannot open a project"));
    host.open_project(directory);
    // Switching projects stops the engine and starts another, and the stop is
    // asynchronous: for a moment the viewport still answers RPC, and it is the
    // old engine answering. Return only once it has let go, so a `wait_engine`
    // after this waits for the new one rather than agreeing with the old one.
    // A project that was already open restarts nothing, and says so.
    const int settle = timeoutOf(request, 20000);
    bool restarted = false;
    if (host.viewport != nullptr && host.viewport->rpcReady()) {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < settle) {
            if (!host.viewport->rpcReady()) {
                restarted = true;
                break;
            }
            QTest::qWait(25);
        }
    }
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("dir"), QDir(directory).absolutePath()},
        // False when the viewport kept the engine it had: opening the project
        // that is already open is a no-op, by EmbeddedViewport's own rule.
        {QStringLiteral("engine_restarting"), restarted},
    });
}

AutomationReply commandScreenshot(const AutomationHost &host,
                                  const QJsonObject &request) {
    const QString path = request.value(QStringLiteral("path")).toString();
    if (path.isEmpty())
        return AutomationReply::failure(BadRequest,
                                        QStringLiteral("path is required"));
    Target target;
    const QString name = request.value(QStringLiteral("name")).toString();
    if (name.isEmpty()) {
        if (host.window == nullptr)
            return AutomationReply::failure(
                WidgetMissing, QStringLiteral("this process has no window"));
        target.widget = host.window;
        target.owner = host.window;
    } else {
        target = resolveTarget(host, name);
        if (!target.valid())
            return AutomationReply::failure(
                WidgetMissing,
                QStringLiteral("no widget or drawn item is named \"%1\"")
                    .arg(name));
    }
    // F-4: points to read back from the picture, each [x, y] as a fraction of
    // its width and height, so a scenario can compare two pixels of what the
    // engine drew -- "the same colour down one stripe" -- without knowing the
    // size of the window it was given.
    std::vector<QPointF> wanted_pixels;
    if (request.contains(QStringLiteral("pixels"))) {
        const QJsonValue pixels = request.value(QStringLiteral("pixels"));
        if (!pixels.isArray())
            return AutomationReply::failure(
                BadRequest, QStringLiteral("pixels must be a list of [x, y]"));
        for (const QJsonValue &entry : pixels.toArray()) {
            const QJsonArray at = entry.toArray();
            const double x = at.size() == 2 ? at.at(0).toDouble(-1.0) : -1.0;
            const double y = at.size() == 2 ? at.at(1).toDouble(-1.0) : -1.0;
            if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
                return AutomationReply::failure(
                    BadRequest,
                    QStringLiteral("each pixel is [x, y], fractions of the "
                                   "picture between 0 and 1"));
            wanted_pixels.emplace_back(x, y);
        }
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    // "widget" renders the Qt widget tree; the embedded engine window is a
    // foreign child and does not appear in it. "screen" reads the desktop
    // where the window is, which does include the engine -- and which needs
    // the window to be unobstructed.
    const QString mode =
        request.value(QStringLiteral("mode")).toString(QStringLiteral("widget"));
    QPixmap pixmap;
    QString capture = mode;
    bool desktop_obstructed = false;
    if (mode == QLatin1String("screen")) {
#ifdef _WIN32
        // The window's own content first; the desktop only when the window
        // could not be printed or printed as one flat colour (a swapchain
        // that the compositor does not hand to PrintWindow). One flat colour
        // is also what a viewport legitimately shows (a cleared target, the
        // albedo of an empty scene), so the desktop replaces it only when
        // the desktop there shows this window: another application's window
        // lying over the viewport is not a better picture of it.
        QWidget *top_level =
            target.owner != nullptr ? target.owner->window() : nullptr;
        const QRect where = globalRect(target);
        const QImage printed = printWindowRegion(top_level, where);
        if (!printed.isNull() &&
            (!isOneColour(printed) ||
             !desktopShowsWindow(top_level, where))) {
            pixmap = QPixmap::fromImage(printed);
            capture = QStringLiteral("print_window");
            desktop_obstructed = isOneColour(printed);
        }
#endif
    }
#ifdef _WIN32
    if (mode == QLatin1String("screen") && pixmap.isNull()) {
        QWidget *top_level = target.owner != nullptr ? target.owner->window() : nullptr;
        const QImage desktop = desktopRegion(physicalScreenRect(top_level, globalRect(target)));
        if (!desktop.isNull()) pixmap = QPixmap::fromImage(desktop);
    }
#endif
    if (mode == QLatin1String("screen") && pixmap.isNull()) {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (target.owner != nullptr && target.owner->window() != nullptr &&
            target.owner->window()->windowHandle() != nullptr)
            screen = target.owner->window()->windowHandle()->screen();
        if (screen == nullptr)
            return AutomationReply::failure(
                ScreenshotFailed, QStringLiteral("no screen to capture"));
        // With no window, grabWindow reads the screen from its own top-left.
        const QRect where = globalRect(target).translated(-screen->geometry().topLeft());
        pixmap = screen->grabWindow(0, where.x(), where.y(), where.width(),
                                    where.height());
    } else if (mode == QLatin1String("widget")) {
        pixmap = target.isSceneItem() ? target.view->grab()
                                      : target.owner->grab();
    } else if (mode != QLatin1String("screen")) {
        return AutomationReply::failure(
            BadRequest, QStringLiteral("mode must be widget or screen"));
    }
    if (pixmap.isNull() || !pixmap.save(path, "PNG"))
        return AutomationReply::failure(
            ScreenshotFailed,
            QStringLiteral("could not write %1").arg(path));
    // F-2: how many different colours the picture holds, so a scenario can
    // ask "did the engine draw something" of a report instead of a reader
    // having to open the PNG. A cleared or never-presented viewport is one
    // colour; a gradient is hundreds.
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_RGB32);
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) colors.insert(row[x]);
    }
    QJsonObject reply{
        {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("capture"), capture},
        {QStringLiteral("width"), pixmap.width()},
        {QStringLiteral("height"), pixmap.height()},
        {QStringLiteral("distinct_colors"), static_cast<int>(colors.size())},
    };
    // The printed picture was one colour and was kept because another window
    // covered the viewport on the desktop; a reader of the report can tell a
    // flat viewport from a flat print this way.
    if (desktop_obstructed)
        reply.insert(QStringLiteral("desktop_obstructed"), true);
    if (!wanted_pixels.empty()) {
        QJsonArray pixels;
        // MT-1s: and each as [r, g, b] (0-255), so a step can ask which way
        // a colour leans ("red above green") without knowing its shade.
        QJsonArray channels;
        for (const QPointF &at : wanted_pixels) {
            const int x = std::clamp(static_cast<int>(at.x() * image.width()),
                                     0, image.width() - 1);
            const int y = std::clamp(static_cast<int>(at.y() * image.height()),
                                     0, image.height() - 1);
            const QColor color{image.pixel(x, y)};
            pixels.append(color.name());
            channels.append(QJsonArray{color.red(), color.green(), color.blue()});
        }
        reply.insert(QStringLiteral("pixels"), pixels);
        reply.insert(QStringLiteral("rgb"), channels);
    }
    return AutomationReply::success(reply);
}

} // namespace

QStringList automationCommandNames() {
    return {QStringLiteral("list"),         QStringLiteral("click"),
            QStringLiteral("double_click"), QStringLiteral("drag"),
            QStringLiteral("wheel"),        QStringLiteral("move"),
            QStringLiteral("press"),        QStringLiteral("release"),
            QStringLiteral("key"),          QStringLiteral("type"),
            QStringLiteral("text"),         QStringLiteral("select"),
            QStringLiteral("action"),
            QStringLiteral("menu_list"),    QStringLiteral("view_state"),
            QStringLiteral("layout_stats"),
            QStringLiteral("wait"),         QStringLiteral("wait_engine"),
            QStringLiteral("rpc"),          QStringLiteral("open_project"),
            QStringLiteral("screenshot"),   QStringLiteral("viewport_size"),
            QStringLiteral("quit")};
}

AutomationReply runAutomationCommand(const AutomationHost &host,
                                     const QJsonObject &request) {
    const QString command = request.value(QStringLiteral("cmd")).toString();
    if (command == QLatin1String("list")) return commandList(host, request);
    if (command == QLatin1String("click"))
        return commandClick(host, request, false);
    if (command == QLatin1String("double_click"))
        return commandClick(host, request, true);
    if (command == QLatin1String("drag")) return commandDrag(host, request);
    if (command == QLatin1String("wheel")) return commandWheel(host, request);
    if (command == QLatin1String("move"))
        return commandPointer(host, request, PointerAction::move);
    if (command == QLatin1String("press"))
        return commandPointer(host, request, PointerAction::press);
    if (command == QLatin1String("release"))
        return commandPointer(host, request, PointerAction::release);
    if (command == QLatin1String("action")) return commandAction(host, request);
    if (command == QLatin1String("menu_list"))
        return commandMenuList(host, request);
    if (command == QLatin1String("view_state"))
        return commandViewState(host, request);
    if (command == QLatin1String("layout_stats"))
        return commandLayoutStats(host, request);
    if (command == QLatin1String("key")) return commandKey(host, request);
    if (command == QLatin1String("type")) return commandType(host, request);
    if (command == QLatin1String("text")) return commandText(host, request);
    if (command == QLatin1String("select")) return commandSelect(host, request);
    if (command == QLatin1String("wait")) return commandWait(host, request);
    if (command == QLatin1String("wait_engine"))
        return commandWaitEngine(host, request);
    if (command == QLatin1String("rpc")) return commandRpc(host, request);
    if (command == QLatin1String("open_project"))
        return commandOpenProject(host, request);
    if (command == QLatin1String("screenshot"))
        return commandScreenshot(host, request);
    if (command == QLatin1String("viewport_size"))
        return commandViewportSize(host, request);
    // `quit` is answered by the server: it has to finish writing this reply
    // before the event loop it is running in goes away.
    return AutomationReply::failure(
        UnknownCommand,
        QStringLiteral("unknown command \"%1\"; known commands: %2")
            .arg(command, automationCommandNames().join(QStringLiteral(", "))));
}

} // namespace PelicanStudio
