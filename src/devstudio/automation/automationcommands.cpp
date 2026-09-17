#include "automationcommands.hpp"
#include "automationserver.hpp"

#include "../view/graphcanvaswidget.hpp"
#include "../view/nodegraphics.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAction>
#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
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
#include <vector>

namespace PelicanStudio {
namespace {

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
    const QString port = item.data(GraphCanvasPortRole).toString();
    if (!port.isEmpty()) {
        identity = identity.isEmpty() ? port
                                      : identity + QStringLiteral("/") + port;
    }
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
    if (view == nullptr) return target;
    // One drawn thing can be several items: a value pill is a rounded rect with
    // the number as its child, and both carry the same identity. The one that
    // can answer `text` is the one a scenario means, and either of them sits at
    // the same place for a click.
    for (const auto &[candidate, item] : sceneItems(*view)) {
        if (candidate != name) continue;
        if (target.item == nullptr || itemText(*item).isEmpty() == false) {
            target.view = view;
            target.item = item;
            target.widget = view->viewport();
            target.owner = view;
            if (!itemText(*item).isEmpty()) break;
        }
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
    return target.owner != nullptr && target.owner->isVisible();
}

QRect globalRect(const Target &target) {
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
    if (target.isSceneItem()) {
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
            entries.push_back(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("class"),
                 QString::fromLatin1(widget->metaObject()->className())},
                {QStringLiteral("visible"), widget->isVisible()},
                {QStringLiteral("enabled"), widget->isEnabled()},
                {QStringLiteral("geometry"), rectJson(globalRect(target))},
                {QStringLiteral("text"), widgetText(*widget)},
                {QStringLiteral("shortcut"), widgetShortcut(*widget)},
            });
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
    if (!visible.isBool() && !text.isString() && !contains.isString() &&
        !missing.isBool())
        return AutomationReply::failure(
            BadRequest,
            QStringLiteral("wait needs one of visible, text, contains or missing"));

    QString last_text;
    bool last_visible = false;
    bool last_found = false;
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        const Target target = resolveTarget(host, name);
        last_found = target.valid();
        last_visible = last_found && targetVisible(target);
        last_text = last_found ? targetText(target) : QString{};
        bool satisfied = true;
        if (missing.isBool()) satisfied = satisfied && (last_found != missing.toBool());
        if (visible.isBool()) satisfied = satisfied && last_visible == visible.toBool();
        if (text.isString()) satisfied = satisfied && last_found && last_text == text.toString();
        if (contains.isString())
            satisfied = satisfied && last_found &&
                        last_text.contains(contains.toString());
        if (satisfied)
            return AutomationReply::success(QJsonObject{
                {QStringLiteral("name"), name},
                {QStringLiteral("text"), last_text},
                {QStringLiteral("visible"), last_visible},
                {QStringLiteral("waited_ms"),
                 static_cast<double>(elapsed.elapsed())},
            });
        if (elapsed.elapsed() >= timeout) break;
        QTest::qWait(25);
    }
    return AutomationReply::failure(
        WaitTimeout,
        QStringLiteral("\"%1\" did not reach the expected state in %2 ms "
                       "(found=%3 visible=%4 text=\"%5\")")
            .arg(name)
            .arg(timeout)
            .arg(last_found ? QStringLiteral("true") : QStringLiteral("false"),
                 last_visible ? QStringLiteral("true") : QStringLiteral("false"),
                 last_text));
}

AutomationReply commandWaitEngine(const AutomationHost &host,
                                  const QJsonObject &request) {
    if (host.viewport == nullptr)
        return AutomationReply::failure(
            EngineNotReady, QStringLiteral("this window has no engine viewport"));
    const int timeout = timeoutOf(request, 60000);
    QElapsedTimer elapsed;
    elapsed.start();
    for (;;) {
        if (host.viewport->rpcReady())
            return AutomationReply::success(QJsonObject{
                {QStringLiteral("ready"), true},
                {QStringLiteral("waited_ms"),
                 static_cast<double>(elapsed.elapsed())},
            });
        if (elapsed.elapsed() >= timeout) break;
        QTest::qWait(50);
    }
    return AutomationReply::failure(
        EngineNotReady,
        QStringLiteral("the engine viewport was not ready for editor RPC "
                       "within %1 ms")
            .arg(timeout));
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
    const QJsonObject params = params_value.toObject();
    QString error;
    const qint64 id = host.viewport->requestRpc(method, params, &error);
    if (id == 0)
        return AutomationReply::failure(
            EngineNotReady,
            error.isEmpty() ? QStringLiteral("the engine refused the request")
                            : error);

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
    timer.start(timeoutOf(request));
    loop.exec();
    QObject::disconnect(on_result);
    QObject::disconnect(on_failure);

    if (!answered)
        return AutomationReply::failure(
            RpcFailed, QStringLiteral("%1 did not answer within %2 ms")
                           .arg(method)
                           .arg(timeoutOf(request)));
    if (!succeeded)
        return AutomationReply::failure(
            RpcFailed, QStringLiteral("%1 failed: %2").arg(method, message));
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("method"), method},
        {QStringLiteral("result"), parseRpcPayload(payload)},
    });
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
    QDir().mkpath(QFileInfo(path).absolutePath());
    // "widget" renders the Qt widget tree; the embedded engine window is a
    // foreign child and does not appear in it. "screen" reads the desktop
    // where the window is, which does include the engine -- and which needs
    // the window to be unobstructed.
    const QString mode =
        request.value(QStringLiteral("mode")).toString(QStringLiteral("widget"));
    QPixmap pixmap;
    if (mode == QLatin1String("screen")) {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (target.owner != nullptr && target.owner->window() != nullptr &&
            target.owner->window()->windowHandle() != nullptr)
            screen = target.owner->window()->windowHandle()->screen();
        if (screen == nullptr)
            return AutomationReply::failure(
                ScreenshotFailed, QStringLiteral("no screen to capture"));
        const QRect where = globalRect(target);
        pixmap = screen->grabWindow(0, where.x(), where.y(), where.width(),
                                    where.height());
    } else if (mode == QLatin1String("widget")) {
        pixmap = target.isSceneItem() ? target.view->grab()
                                      : target.owner->grab();
    } else {
        return AutomationReply::failure(
            BadRequest, QStringLiteral("mode must be widget or screen"));
    }
    if (pixmap.isNull() || !pixmap.save(path, "PNG"))
        return AutomationReply::failure(
            ScreenshotFailed,
            QStringLiteral("could not write %1").arg(path));
    return AutomationReply::success(QJsonObject{
        {QStringLiteral("path"), QFileInfo(path).absoluteFilePath()},
        {QStringLiteral("mode"), mode},
        {QStringLiteral("width"), pixmap.width()},
        {QStringLiteral("height"), pixmap.height()},
    });
}

} // namespace

QStringList automationCommandNames() {
    return {QStringLiteral("list"),         QStringLiteral("click"),
            QStringLiteral("double_click"), QStringLiteral("drag"),
            QStringLiteral("wheel"),        QStringLiteral("move"),
            QStringLiteral("press"),        QStringLiteral("release"),
            QStringLiteral("key"),          QStringLiteral("type"),
            QStringLiteral("text"),         QStringLiteral("action"),
            QStringLiteral("menu_list"),    QStringLiteral("view_state"),
            QStringLiteral("wait"),         QStringLiteral("wait_engine"),
            QStringLiteral("rpc"),          QStringLiteral("open_project"),
            QStringLiteral("screenshot"),   QStringLiteral("quit")};
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
    if (command == QLatin1String("key")) return commandKey(host, request);
    if (command == QLatin1String("type")) return commandType(host, request);
    if (command == QLatin1String("text")) return commandText(host, request);
    if (command == QLatin1String("wait")) return commandWait(host, request);
    if (command == QLatin1String("wait_engine"))
        return commandWaitEngine(host, request);
    if (command == QLatin1String("rpc")) return commandRpc(host, request);
    if (command == QLatin1String("open_project"))
        return commandOpenProject(host, request);
    if (command == QLatin1String("screenshot"))
        return commandScreenshot(host, request);
    // `quit` is answered by the server: it has to finish writing this reply
    // before the event loop it is running in goes away.
    return AutomationReply::failure(
        UnknownCommand,
        QStringLiteral("unknown command \"%1\"; known commands: %2")
            .arg(command, automationCommandNames().join(QStringLiteral(", "))));
}

} // namespace PelicanStudio
