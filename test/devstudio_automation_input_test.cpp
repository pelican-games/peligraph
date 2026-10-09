// Q-1b: the input kinds Studio's automation surface gained -- a wheel, a bare
// move, a held button, a menu action fired by name, a listing of what the menu
// bar holds, and a reading of where a canvas is looking.
//
// Nothing here talks to a socket or to an engine. Each case builds the widget
// that would receive the gesture, sends the command through the same
// `runAutomationCommand` the pipe calls, and asks the widget what it received.
// The offscreen platform is enough for that: these are Qt events, not pixels.

#include "automationcommands.hpp"
#include "graphcanvaswidget.hpp"
#include "frameplanedit.hpp"
#include <cmath>
#include <QGraphicsItem>
#include "../src/devstudio/view/nodegraphics.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "automationserver.hpp"
#define private public
#include "../src/devstudio/viewport/embeddedviewport.hpp"
#undef private

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
#include <QLineEdit>
#include <QLineF>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWheelEvent>
#include <QWidget>

#include <string>
#include <vector>

#include "inspectorwidget.hpp"
#include "outlinertree.hpp"
#include "frameplanwidget.hpp"
#include "../src/devstudio/layoutpreset.hpp"
#include "../src/devstudio/view/renderdocumentwatcher.hpp"
#include "../src/devstudio/toollayoutpreset.hpp"
#include "../src/devstudio/model/renderedithistory.hpp"
#include "../src/devstudio/model/renderdocumentreload.hpp"
#include "../src/devstudio/model/modelimport.hpp"
#include <QDialog>
#include <QFileDialog>
#include <QTableWidget>
#include <QToolBar>
#include <deque>
#define private public
#include "mainwindow.hpp"
#undef private

namespace {

using PelicanStudio::AutomationHost;
using PelicanStudio::AutomationReply;
using PelicanStudio::automationCommandNames;
using PelicanStudio::runAutomationCommand;

QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_automation_input_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

QJsonObject request(const QString &command, const QJsonObject &fields = {}) {
    QJsonObject object = fields;
    object.insert(QStringLiteral("cmd"), command);
    return object;
}

QString describe(const AutomationReply &reply) {
    return reply.ok ? QStringLiteral("ok")
                    : reply.code + QStringLiteral(": ") + reply.message;
}

// One line per mouse event the widget under test was handed, in the shape a
// scenario cares about: what happened, with which button, and -- the question
// a held gesture turns on -- which buttons were down while it happened.
QString describeMouse(const QMouseEvent &event) {
    QString kind;
    switch (event.type()) {
        case QEvent::MouseButtonPress: kind = QStringLiteral("press"); break;
        case QEvent::MouseButtonRelease: kind = QStringLiteral("release"); break;
        case QEvent::MouseMove: kind = QStringLiteral("move"); break;
        default: kind = QStringLiteral("other"); break;
    }
    QStringList held;
    if (event.buttons() & Qt::LeftButton) held.push_back(QStringLiteral("left"));
    if (event.buttons() & Qt::MiddleButton)
        held.push_back(QStringLiteral("middle"));
    if (event.buttons() & Qt::RightButton)
        held.push_back(QStringLiteral("right"));
    QString button = QStringLiteral("none");
    if (event.button() == Qt::LeftButton) button = QStringLiteral("left");
    else if (event.button() == Qt::MiddleButton) button = QStringLiteral("middle");
    else if (event.button() == Qt::RightButton) button = QStringLiteral("right");
    return kind + QStringLiteral("/") + button + QStringLiteral("/[") +
           held.join(QLatin1Char(',')) + QStringLiteral("]");
}

// A canvas that writes down what it was handed, and zooms on a wheel the way a
// real canvas does -- so `view_state` has a number that can move.
class RecordingView final : public QGraphicsView {
  public:
    explicit RecordingView(QWidget *parent = nullptr)
        : QGraphicsView{parent} {
        setScene(new QGraphicsScene(this));
        scene()->setSceneRect(-500, -500, 1000, 1000);
        setMouseTracking(true);
        viewport()->setMouseTracking(true);
    }

    QStringList mouse;
    int wheels = 0;
    int last_delta = 0;
    Qt::KeyboardModifiers last_wheel_modifiers = Qt::NoModifier;
    bool zoom_on_wheel = true;

  protected:
    void wheelEvent(QWheelEvent *event) override {
        ++wheels;
        last_delta = event->angleDelta().y();
        last_wheel_modifiers = event->modifiers();
        if (zoom_on_wheel && last_delta != 0) {
            const qreal factor = last_delta > 0 ? 1.25 : 1.0 / 1.25;
            scale(factor, factor);
        }
        event->accept();
    }
    void mousePressEvent(QMouseEvent *event) override {
        mouse.push_back(describeMouse(*event));
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        mouse.push_back(describeMouse(*event));
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        mouse.push_back(describeMouse(*event));
        event->accept();
    }
};

// Everything one case needs: a shown window with a canvas, a button that owns a
// shortcut, and a menu bar whose entries are named the way Studio's are.
struct Fixture {
    // First, and on purpose: a QWidget cannot be built before there is a
    // QApplication, and members are built in the order they are declared --
    // before the constructor body gets a chance to say anything.
    QApplication &app = application();
    QMainWindow window;
    RecordingView *canvas = nullptr;
    QPushButton *button = nullptr;
    QAction *ping = nullptr;
    QAction *toggle = nullptr;
    QAction *disabled = nullptr;
    QAction *nested_ping = nullptr;
    // AG-3b: the two shapes `select` reaches -- a tree of rows, and a combo
    // whose entries live in a popup this surface cannot open.
    QTreeWidget *tree = nullptr;
    QComboBox *combo = nullptr;
    QPushButton *locked = nullptr;
    QStringList tree_selections;
    QStringList combo_selections;
    int pings = 0;
    int nested_pings = 0;

    Fixture() {
        window.setObjectName(QStringLiteral("pelican.testWindow"));
        auto *central = new QWidget(&window);
        central->setObjectName(QStringLiteral("pelican.testCentral"));
        auto *layout = new QVBoxLayout(central);
        canvas = new RecordingView(central);
        canvas->setObjectName(QStringLiteral("pelican.testCanvas"));
        canvas->setMinimumSize(320, 240);
        layout->addWidget(canvas);
        button = new QPushButton(QStringLiteral("Refresh"), central);
        button->setObjectName(QStringLiteral("pelican.testRefresh"));
        button->setShortcut(QKeySequence{QStringLiteral("Ctrl+R")});
        layout->addWidget(button);
        // A panel that is on screen but not yet usable: what `wait enabled`
        // is for.
        locked = new QPushButton(QStringLiteral("Attach..."), central);
        locked->setObjectName(QStringLiteral("pelican.testAttach"));
        locked->setEnabled(false);
        layout->addWidget(locked);
        // Two rows under one scene, the shape the Outliner has.
        tree = new QTreeWidget(central);
        tree->setObjectName(QStringLiteral("pelican.testOutliner"));
        tree->setHeaderLabel(QStringLiteral("Scene / Object"));
        auto *scene_row = new QTreeWidgetItem(
            tree, QStringList{QStringLiteral("default_scene")});
        new QTreeWidgetItem(scene_row, QStringList{QStringLiteral("Anchor")});
        new QTreeWidgetItem(scene_row, QStringList{QStringLiteral("Camera")});
        tree->expandAll();
        QObject::connect(tree, &QTreeWidget::currentItemChanged,
                         [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
                             tree_selections.push_back(
                                 current == nullptr ? QString{}
                                                    : current->text(0));
                         });
        layout->addWidget(tree);
        combo = new QComboBox(central);
        combo->setObjectName(QStringLiteral("pelican.testScopes"));
        combo->addItem(QStringLiteral("root"), QString{});
        combo->addItem(QStringLiteral("gain_curve"),
                       QStringLiteral("gain_curve"));
        QObject::connect(combo, &QComboBox::currentIndexChanged,
                         [this](int index) {
                             combo_selections.push_back(combo->itemText(index));
                         });
        layout->addWidget(combo);
        window.setCentralWidget(central);

        QMenu *tools = window.menuBar()->addMenu(QStringLiteral("&Tools"));
        tools->setObjectName(QStringLiteral("pelican.test.toolsMenu"));
        ping = tools->addAction(QStringLiteral("&Ping"));
        ping->setObjectName(QStringLiteral("pelican.test.ping"));
        QObject::connect(ping, &QAction::triggered, [this] { ++pings; });
        toggle = tools->addAction(QStringLiteral("&Grid"));
        toggle->setObjectName(QStringLiteral("pelican.test.grid"));
        toggle->setCheckable(true);
        toggle->setShortcut(QKeySequence{QStringLiteral("Ctrl+T")});
        // Application context, so the check does not turn on which window the
        // offscreen desktop decided to call active.
        toggle->setShortcutContext(Qt::ApplicationShortcut);
        window.addAction(toggle);
        disabled = tools->addAction(QStringLiteral("&Unavailable"));
        disabled->setObjectName(QStringLiteral("pelican.test.unavailable"));
        disabled->setEnabled(false);

        QMenu *nested = tools->addMenu(QStringLiteral("&More"));
        nested->setObjectName(QStringLiteral("pelican.test.moreMenu"));
        // The same label twice, in two menus: what `menu` is for.
        nested_ping = nested->addAction(QStringLiteral("&Ping"));
        nested_ping->setObjectName(QStringLiteral("pelican.test.more.ping"));
        QObject::connect(nested_ping, &QAction::triggered,
                         [this] { ++nested_pings; });

        window.resize(640, 480);
        window.show();
        QApplication::processEvents();
    }

    AutomationHost host() {
        AutomationHost value;
        value.window = &window;
        return value;
    }

    AutomationReply run(const QString &command,
                        const QJsonObject &fields = {}) {
        return runAutomationCommand(host(), request(command, fields));
    }

    QJsonObject result(const QString &command, const QJsonObject &fields = {}) {
        const AutomationReply reply = run(command, fields);
        INFO("command " << command.toStdString() << " -> "
                        << describe(reply).toStdString());
        REQUIRE(reply.ok);
        return reply.result.toObject();
    }
};

QJsonObject centreOfCanvas() {
    return QJsonObject{{QStringLiteral("name"),
                        QStringLiteral("pelican.testCanvas")}};
}

QJsonArray actionsNamed(const QJsonObject &listing) {
    return listing.value(QStringLiteral("actions")).toArray();
}

QJsonObject onlyAction(const QJsonObject &listing) {
    const QJsonArray actions = actionsNamed(listing);
    REQUIRE(actions.size() == 1);
    return actions.at(0).toObject();
}

} // namespace

TEST_CASE("Q-1b: every Q-1b command is a name the surface answers to",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;
    const QStringList names = automationCommandNames();
    for (const char *added : {"wheel", "move", "press", "release", "action",
                              "menu_list", "view_state", "select"}) {
        INFO("command " << added);
        CHECK(names.contains(QString::fromLatin1(added)));
    }

    // The control the acceptance asks for: a name this surface does not carry
    // is refused with the code a driver reads, and the message says what it
    // does carry. Take any one of the commands above out of the dispatch and
    // this is the answer its own case gets.
    const AutomationReply unknown = fixture.run(QStringLiteral("wheel_zoom"));
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.code == QStringLiteral("automation.unknown_command@1"));
    CHECK(unknown.message.contains(QStringLiteral("wheel")));
}

TEST_CASE("Q-1b: wheel reaches the canvas and moves what a zoom moves",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;

    const QJsonObject before =
        fixture.result(QStringLiteral("view_state"), centreOfCanvas());
    CHECK(before.value(QStringLiteral("scale")).toDouble() == 1.0);

    QJsonObject forward = centreOfCanvas();
    forward.insert(QStringLiteral("delta"), 120);
    const QJsonObject sent = fixture.result(QStringLiteral("wheel"), forward);
    CHECK(sent.value(QStringLiteral("delta")).toInt() == 120);
    CHECK(fixture.canvas->wheels == 1);
    CHECK(fixture.canvas->last_delta == 120);

    const QJsonObject after =
        fixture.result(QStringLiteral("view_state"), centreOfCanvas());
    CHECK(after.value(QStringLiteral("scale")).toDouble() > 1.0);

    // A wheel the other way undoes it, so the reading is the view's and not a
    // number this surface made up.
    QJsonObject back = centreOfCanvas();
    back.insert(QStringLiteral("delta"), -120);
    fixture.result(QStringLiteral("wheel"), back);
    const QJsonObject returned =
        fixture.result(QStringLiteral("view_state"), centreOfCanvas());
    CHECK(returned.value(QStringLiteral("scale")).toDouble() ==
          Catch::Approx(1.0).epsilon(0.001));

    // Modifiers ride along: Ctrl+wheel is a different gesture from a wheel.
    QJsonObject with_ctrl = centreOfCanvas();
    with_ctrl.insert(QStringLiteral("delta"), 120);
    with_ctrl.insert(QStringLiteral("modifiers"), QStringLiteral("Ctrl"));
    fixture.result(QStringLiteral("wheel"), with_ctrl);
    CHECK(fixture.canvas->last_wheel_modifiers == Qt::ControlModifier);
}

TEST_CASE("Q-1b: wheel says what it cannot do", "[devstudio][automation][wp-q1b]") {
    Fixture fixture;

    const AutomationReply no_delta =
        fixture.run(QStringLiteral("wheel"), centreOfCanvas());
    CHECK_FALSE(no_delta.ok);
    CHECK(no_delta.code == QStringLiteral("automation.bad_request@1"));

    QJsonObject nowhere{{QStringLiteral("name"),
                         QStringLiteral("pelican.noSuchCanvas")},
                        {QStringLiteral("delta"), 120}};
    const AutomationReply missing = fixture.run(QStringLiteral("wheel"), nowhere);
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == QStringLiteral("automation.widget_missing@1"));
    CHECK(fixture.canvas->wheels == 0);
}

TEST_CASE("Q-1b: a bare move is delivered, with nothing held",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;
    QJsonObject where = centreOfCanvas();
    where.insert(QStringLiteral("point"), QJsonArray{40, 30});
    const QJsonObject moved = fixture.result(QStringLiteral("move"), where);
    CHECK(moved.value(QStringLiteral("buttons")).toArray().isEmpty());
    REQUIRE(fixture.canvas->mouse.size() == 1);
    CHECK(fixture.canvas->mouse.at(0) == QStringLiteral("move/none/[]"));
}

TEST_CASE("Q-1b: press and release hold a button across the moves between them",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;

    QJsonObject press = centreOfCanvas();
    press.insert(QStringLiteral("point"), QJsonArray{40, 30});
    press.insert(QStringLiteral("button"), QStringLiteral("middle"));
    const QJsonObject pressed = fixture.result(QStringLiteral("press"), press);
    CHECK(pressed.value(QStringLiteral("buttons")).toArray().at(0).toString() ==
          QStringLiteral("middle"));

    QJsonObject move = centreOfCanvas();
    move.insert(QStringLiteral("point"), QJsonArray{90, 70});
    fixture.result(QStringLiteral("move"), move);

    QJsonObject release = centreOfCanvas();
    release.insert(QStringLiteral("point"), QJsonArray{90, 70});
    release.insert(QStringLiteral("button"), QStringLiteral("middle"));
    const QJsonObject released =
        fixture.result(QStringLiteral("release"), release);
    CHECK(released.value(QStringLiteral("buttons")).toArray().isEmpty());

    REQUIRE(fixture.canvas->mouse.size() == 3);
    CHECK(fixture.canvas->mouse.at(0) ==
          QStringLiteral("press/middle/[middle]"));
    // The point of the whole command pair: the move in the middle carries the
    // button. A pan reads exactly this, and QTest's own mouseMove does not
    // deliver one at all when nothing is held.
    CHECK(fixture.canvas->mouse.at(1) == QStringLiteral("move/none/[middle]"));
    CHECK(fixture.canvas->mouse.at(2) == QStringLiteral("release/middle/[]"));
}

TEST_CASE("Q-1b: drag carries the button it was given",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;
    const QJsonObject dragged = fixture.result(
        QStringLiteral("drag"),
        QJsonObject{
            {QStringLiteral("from"),
             QJsonObject{{QStringLiteral("name"),
                          QStringLiteral("pelican.testCanvas")},
                         {QStringLiteral("point"), QJsonArray{30, 30}}}},
            {QStringLiteral("to"),
             QJsonObject{{QStringLiteral("name"),
                          QStringLiteral("pelican.testCanvas")},
                         {QStringLiteral("point"), QJsonArray{130, 90}}}},
            {QStringLiteral("button"), QStringLiteral("middle")},
            {QStringLiteral("steps"), 4},
        });
    CHECK(dragged.value(QStringLiteral("button")).toString() ==
          QStringLiteral("middle"));
    REQUIRE(fixture.canvas->mouse.size() == 6);
    CHECK(fixture.canvas->mouse.front() ==
          QStringLiteral("press/middle/[middle]"));
    for (int index = 1; index <= 4; ++index)
        CHECK(fixture.canvas->mouse.at(index) ==
              QStringLiteral("move/none/[middle]"));
    CHECK(fixture.canvas->mouse.back() == QStringLiteral("release/middle/[]"));

    const AutomationReply bad = fixture.run(
        QStringLiteral("press"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testCanvas")},
                    {QStringLiteral("button"), QStringLiteral("thumb")}});
    CHECK_FALSE(bad.ok);
    CHECK(bad.code == QStringLiteral("automation.bad_request@1"));
}

TEST_CASE("Q-1b: view_state reads where the canvas is looking",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;
    const QJsonObject state =
        fixture.result(QStringLiteral("view_state"), centreOfCanvas());
    CHECK(state.value(QStringLiteral("name")).toString() ==
          QStringLiteral("pelican.testCanvas"));
    CHECK(state.value(QStringLiteral("scale")).toDouble() == 1.0);
    CHECK(state.value(QStringLiteral("transform")).toArray().size() == 6);
    CHECK(state.value(QStringLiteral("center")).toArray().size() == 2);
    CHECK(state.value(QStringLiteral("scroll")).toObject().contains(
        QStringLiteral("y_max")));
    CHECK(state.value(QStringLiteral("viewport")).toArray().at(0).toInt() > 0);

    // A pan is a moved centre, whatever moved it. Scrolling the view by hand is
    // the cheapest proof that the reading follows the view rather than a
    // remembered number.
    const double before =
        state.value(QStringLiteral("center")).toArray().at(1).toDouble();
    fixture.canvas->verticalScrollBar()->setValue(
        fixture.canvas->verticalScrollBar()->value() + 40);
    const QJsonObject panned =
        fixture.result(QStringLiteral("view_state"), centreOfCanvas());
    CHECK(panned.value(QStringLiteral("center")).toArray().at(1).toDouble() >
          before);

    const AutomationReply not_a_view = fixture.run(
        QStringLiteral("view_state"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("pelican.testRefresh")}});
    CHECK_FALSE(not_a_view.ok);
    CHECK(not_a_view.code == QStringLiteral("automation.bad_request@1"));

    const AutomationReply nowhere = fixture.run(
        QStringLiteral("view_state"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.nothing")}});
    CHECK_FALSE(nowhere.ok);
    CHECK(nowhere.code == QStringLiteral("automation.widget_missing@1"));
}

TEST_CASE("Q-1b: menu_list publishes what the menu bar holds",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;
    const QJsonObject grid = onlyAction(fixture.result(
        QStringLiteral("menu_list"),
        QJsonObject{{QStringLiteral("pattern"),
                     QStringLiteral("pelican.test.grid")}}));
    CHECK(grid.value(QStringLiteral("text")).toString() ==
          QStringLiteral("Grid"));
    // Decision 3's other half: a shortcut is part of the entry, so a scenario
    // can check the binding without pressing anything.
    CHECK(grid.value(QStringLiteral("shortcut")).toString() ==
          QStringLiteral("Ctrl+T"));
    CHECK(grid.value(QStringLiteral("checkable")).toBool());
    CHECK_FALSE(grid.value(QStringLiteral("checked")).toBool());
    CHECK(grid.value(QStringLiteral("enabled")).toBool());
    CHECK(grid.value(QStringLiteral("menu")).toString().contains(
        QStringLiteral("Tools")));

    const QJsonObject unavailable = onlyAction(fixture.result(
        QStringLiteral("menu_list"),
        QJsonObject{{QStringLiteral("pattern"),
                     QStringLiteral("pelican.test.unavailable")}}));
    CHECK_FALSE(unavailable.value(QStringLiteral("enabled")).toBool());

    // A submenu is a row a person sees, and it says so rather than pretending
    // to be something `action` can fire.
    const QJsonObject more = onlyAction(fixture.result(
        QStringLiteral("menu_list"),
        QJsonObject{{QStringLiteral("pattern"),
                     QStringLiteral("pelican.test.moreMenu")}}));
    CHECK(more.value(QStringLiteral("submenu")).toBool());

    // Scoped to one widget: the canvas holds no actions of its own.
    const QJsonObject scoped = fixture.result(
        QStringLiteral("menu_list"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("pelican.testCanvas")}});
    CHECK(scoped.value(QStringLiteral("count")).toInt() == 0);

    const AutomationReply nowhere = fixture.run(
        QStringLiteral("menu_list"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.nothing")}});
    CHECK_FALSE(nowhere.ok);
    CHECK(nowhere.code == QStringLiteral("automation.widget_missing@1"));
}

TEST_CASE("Q-1b: action fires a menu entry without opening the menu",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;

    const QJsonObject fired = fixture.result(
        QStringLiteral("action"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.test.ping")}});
    CHECK(fired.value(QStringLiteral("triggered")).toBool());
    CHECK(fixture.pings == 1);
    // No popup was entered: a QMenu that had been opened would be visible, and
    // this command would still be inside its event loop.
    CHECK_FALSE(fixture.window.findChild<QMenu *>(
                             QStringLiteral("pelican.test.toolsMenu"))
                    ->isVisible());

    // By label, and by label within one menu when the label is not unique.
    const AutomationReply ambiguous = fixture.run(
        QStringLiteral("action"),
        QJsonObject{{QStringLiteral("text"), QStringLiteral("Ping")}});
    CHECK_FALSE(ambiguous.ok);
    CHECK(ambiguous.code == QStringLiteral("automation.bad_request@1"));
    CHECK(ambiguous.message.contains(QStringLiteral("2 actions")));
    CHECK(fixture.pings == 1);

    fixture.result(QStringLiteral("action"),
                   QJsonObject{{QStringLiteral("text"), QStringLiteral("Ping")},
                               {QStringLiteral("menu"), QStringLiteral("More")}});
    CHECK(fixture.nested_pings == 1);
    CHECK(fixture.pings == 1);

    const AutomationReply nowhere = fixture.run(
        QStringLiteral("action"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.test.nope")}});
    CHECK_FALSE(nowhere.ok);
    CHECK(nowhere.code == QStringLiteral("automation.widget_missing@1"));

    const AutomationReply off = fixture.run(
        QStringLiteral("action"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("pelican.test.unavailable")}});
    CHECK_FALSE(off.ok);
    CHECK(off.code == QStringLiteral("automation.bad_request@1"));
    CHECK(off.message.contains(QStringLiteral("disabled")));
}

TEST_CASE("Q-1b: a shortcut pressed as a key changes what menu_list reports",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;
    const auto checked = [&fixture]() {
        return onlyAction(fixture.result(
                              QStringLiteral("menu_list"),
                              QJsonObject{{QStringLiteral("pattern"),
                                           QStringLiteral("pelican.test.grid")}}))
            .value(QStringLiteral("checked"))
            .toBool();
    };
    REQUIRE_FALSE(checked());

    // Decision 2's answer to "is the binding real": press the keys the desktop
    // would press, through the window, which is the route the shortcut map
    // watches. `action` would have proved the handler, not the binding.
    fixture.result(QStringLiteral("key"),
                   QJsonObject{{QStringLiteral("name"),
                                QStringLiteral("pelican.testCanvas")},
                               {QStringLiteral("sequence"),
                                QStringLiteral("Ctrl+T")},
                               {QStringLiteral("route"),
                                QStringLiteral("window")}});
    CHECK(checked());

    const AutomationReply bad_route = fixture.run(
        QStringLiteral("key"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testCanvas")},
                    {QStringLiteral("sequence"), QStringLiteral("Ctrl+T")},
                    {QStringLiteral("route"), QStringLiteral("telepathy")}});
    CHECK_FALSE(bad_route.ok);
    CHECK(bad_route.code == QStringLiteral("automation.bad_request@1"));
}

TEST_CASE("Q-1b: list carries the shortcut a widget owns",
          "[devstudio][automation][wp-q1b]") {
    Fixture fixture;
    const QJsonObject listing = fixture.result(
        QStringLiteral("list"),
        QJsonObject{{QStringLiteral("pattern"),
                     QStringLiteral("pelican.testRefresh")}});
    const QJsonArray widgets =
        listing.value(QStringLiteral("widgets")).toArray();
    REQUIRE(widgets.size() == 1);
    CHECK(widgets.at(0).toObject().value(QStringLiteral("shortcut")).toString() ==
          QStringLiteral("Ctrl+R"));

    const QJsonObject canvas_listing = fixture.result(
        QStringLiteral("list"),
        QJsonObject{{QStringLiteral("pattern"),
                     QStringLiteral("pelican.testCanvas")}});
    const QJsonArray canvases =
        canvas_listing.value(QStringLiteral("widgets")).toArray();
    REQUIRE(canvases.size() == 1);
    // Every entry answers the question, even when the answer is "none": a
    // reader of a listing should not have to know which classes have one.
    CHECK(canvases.at(0).toObject().contains(QStringLiteral("shortcut")));
    CHECK(canvases.at(0).toObject().value(QStringLiteral("shortcut")).toString() ==
          QString{});
}

// SK-1: a check box's text is its caption, so a scenario reads whether it is
// ticked from list's `checked`; a widget that cannot be ticked has none.
TEST_CASE("SK-1: list says whether a check box is ticked",
          "[devstudio][automation][sk1]") {
    Fixture fixture;
    auto *box = new QCheckBox(QStringLiteral("Double sided"),
                              fixture.window.centralWidget());
    box->setObjectName(QStringLiteral("pelican.testDoubleSided"));
    fixture.window.centralWidget()->layout()->addWidget(box);
    box->show();
    QApplication::processEvents();
    const auto checkedOf = [&fixture](const QString &name) {
        const QJsonArray widgets =
            fixture
                .result(QStringLiteral("list"),
                        QJsonObject{{QStringLiteral("pattern"), name}})
                .value(QStringLiteral("widgets"))
                .toArray();
        REQUIRE(widgets.size() == 1);
        return widgets.at(0).toObject().value(QStringLiteral("checked"));
    };
    CHECK(checkedOf(QStringLiteral("pelican.testDoubleSided")) == QJsonValue{false});
    box->setChecked(true);
    CHECK(checkedOf(QStringLiteral("pelican.testDoubleSided")) == QJsonValue{true});
    CHECK(checkedOf(QStringLiteral("pelican.testRefresh")).isUndefined());
}

// N-4: what a canvas says about its own automatic placement. The scene
// publishes the numbers once per draw; the command is the reading, so a
// scenario can ask a live window how tangled the drawing is rather than
// leaving a reader to judge a screenshot.
TEST_CASE("N-4: layout_stats reads what the placement achieved",
          "[devstudio][automation][n4]") {
    Fixture fixture;
    CHECK(automationCommandNames().contains(QStringLiteral("layout_stats")));

    // Nothing published yet: "nobody has drawn" is not "nothing crosses".
    const QJsonObject empty =
        fixture.result(QStringLiteral("layout_stats"), centreOfCanvas());
    CHECK(empty.value(QStringLiteral("crossings")).toDouble() == -1.0);

    fixture.canvas->scene()->setProperty("pelicanLayoutCrossings",
                                         static_cast<qulonglong>(7));
    fixture.canvas->scene()->setProperty("pelicanLayoutOverlaps",
                                         static_cast<qulonglong>(0));
    fixture.canvas->scene()->setProperty("pelicanLayoutNodes",
                                         static_cast<qulonglong>(12));
    const QJsonObject drawn =
        fixture.result(QStringLiteral("layout_stats"), centreOfCanvas());
    CHECK(drawn.value(QStringLiteral("crossings")).toDouble() == 7.0);
    CHECK(drawn.value(QStringLiteral("overlaps")).toDouble() == 0.0);
    CHECK(drawn.value(QStringLiteral("nodes")).toDouble() == 12.0);

    // A widget that draws no scene is not a canvas, and says so.
    const AutomationReply refused = fixture.run(
        QStringLiteral("layout_stats"),
        QJsonObject{{QStringLiteral("name"),
                     QStringLiteral("pelican.testRefresh")}});
    CHECK_FALSE(refused.ok);
    CHECK(refused.code == QStringLiteral("automation.bad_request@1"));
}

// --------------------------------------------------------------------------
// AG-3b: picking a row, picking an entry, and waiting for a panel to be usable.

TEST_CASE("AG-3b: select picks a row of a tree with a real click",
          "[devstudio][automation][ag3b]") {
    Fixture fixture;

    const QJsonObject picked = fixture.result(
        QStringLiteral("select"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testOutliner")},
                    {QStringLiteral("text"), QStringLiteral("Camera")}});
    CHECK(picked.value(QStringLiteral("text")).toString() ==
          QStringLiteral("Camera"));
    // The view's own selection moved, through its own signal: everything
    // watching a person's click sees exactly what it would have seen.
    CHECK(picked.value(QStringLiteral("selected")).toBool());
    REQUIRE(fixture.tree->currentItem() != nullptr);
    CHECK(fixture.tree->currentItem()->text(0) == QStringLiteral("Camera"));
    CHECK(fixture.tree_selections ==
          QStringList{QStringLiteral("Camera")});

    // `index` counts the rows the way a reader counts them down the panel,
    // parents included, not the rows of one level.
    const QJsonObject by_index = fixture.result(
        QStringLiteral("select"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testOutliner")},
                    {QStringLiteral("index"), 1}});
    CHECK(by_index.value(QStringLiteral("text")).toString() ==
          QStringLiteral("Anchor"));

    // A row this tree does not hold is refused by code, with the name in it
    // (after the wait QA-1 gave select, kept short here).
    const AutomationReply missing = fixture.run(
        QStringLiteral("select"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testOutliner")},
                    {QStringLiteral("text"), QStringLiteral("Hero")},
                    {QStringLiteral("timeout_ms"), 50}});
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == QStringLiteral("automation.bad_request@1"));
    CHECK(missing.message.contains(QStringLiteral("Hero")));
}

TEST_CASE("AG-3b: select picks a combo entry without opening its popup",
          "[devstudio][automation][ag3b]") {
    Fixture fixture;

    const QJsonObject picked = fixture.result(
        QStringLiteral("select"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testScopes")},
                    {QStringLiteral("text"), QStringLiteral("gain_curve")}});
    CHECK(picked.value(QStringLiteral("index")).toInt() == 1);
    CHECK(fixture.combo->currentText() == QStringLiteral("gain_curve"));
    // The combo emitted its own currentIndexChanged, which is the signal a
    // choice made in the popup emits: the receiving code cannot tell the two
    // apart, and no modal event loop was entered to get here.
    CHECK(fixture.combo_selections ==
          QStringList{QStringLiteral("gain_curve")});

    const AutomationReply missing = fixture.run(
        QStringLiteral("select"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testScopes")},
                    {QStringLiteral("text"), QStringLiteral("nowhere")},
                    {QStringLiteral("timeout_ms"), 50}});
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == QStringLiteral("automation.bad_request@1"));

    // Something that is neither is refused rather than silently doing nothing.
    const AutomationReply wrong = fixture.run(
        QStringLiteral("select"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testRefresh")},
                    {QStringLiteral("text"), QStringLiteral("Refresh")}});
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.code == QStringLiteral("automation.bad_request@1"));
}

TEST_CASE("AG-3b: wait answers for whether a thing is usable, not only on screen",
          "[devstudio][automation][ag3b]") {
    Fixture fixture;

    // On screen and switched off: the ordinary way a panel says "not yet".
    const AutomationReply not_yet = fixture.run(
        QStringLiteral("wait"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testAttach")},
                    {QStringLiteral("enabled"), true},
                    {QStringLiteral("timeout_ms"), 50}});
    CHECK_FALSE(not_yet.ok);
    CHECK(not_yet.code == QStringLiteral("automation.wait_timeout@1"));
    CHECK(not_yet.message.contains(QStringLiteral("enabled=false")));
    // And it is on screen, so "visible" would have said yes and the scenario
    // would have clicked a button that does nothing.
    CHECK(fixture.result(
              QStringLiteral("wait"),
              QJsonObject{{QStringLiteral("name"),
                           QStringLiteral("pelican.testAttach")},
                          {QStringLiteral("visible"), true},
                          {QStringLiteral("timeout_ms"), 50}})
              .value(QStringLiteral("visible"))
              .toBool());

    fixture.locked->setEnabled(true);
    QApplication::processEvents();
    CHECK(fixture.result(
              QStringLiteral("wait"),
              QJsonObject{{QStringLiteral("name"),
                           QStringLiteral("pelican.testAttach")},
                          {QStringLiteral("enabled"), true},
                          {QStringLiteral("timeout_ms"), 2000}})
              .value(QStringLiteral("enabled"))
              .toBool());
}

// QD-1 (GL-5g's rule 5): hidden and switched off are states of a thing that is
// there. A wait for "not visible" or "not enabled" on a name that resolves to
// nothing used to be satisfied at once, so a misspelt name passed.
TEST_CASE("QD-1: wait for hidden or switched off needs the thing to be there; gone is missing",
          "[devstudio][automation][qd1]") {
    Fixture fixture;
    for (const char *state : {"visible", "enabled"}) {
        INFO("state " << state);
        const AutomationReply misspelt = fixture.run(
            QStringLiteral("wait"),
            QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testAtach")},
                        {QString::fromLatin1(state), false},
                        {QStringLiteral("timeout_ms"), 50}});
        CHECK_FALSE(misspelt.ok);
        CHECK(misspelt.code == QStringLiteral("automation.wait_timeout@1"));
        CHECK(misspelt.message.contains(QStringLiteral("found=false")));
    }
    // The thing that is there and switched off still answers.
    const QJsonObject off = fixture.result(
        QStringLiteral("wait"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testAttach")},
                    {QStringLiteral("enabled"), false},
                    {QStringLiteral("timeout_ms"), 50}});
    CHECK(off.value(QStringLiteral("found")).toBool());
    // And "gone" is said with missing.
    CHECK(fixture.run(QStringLiteral("wait"),
                      QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testAtach")},
                                  {QStringLiteral("missing"), true},
                                  {QStringLiteral("timeout_ms"), 50}})
              .ok);
}

// QD-1: `history` reads each of Studio's Undo controls by the name it already
// has, and presses one the way the control is pressed; `project_state` needs
// an engine viewport.
TEST_CASE("QD-1: history reads and presses the Undo controls by kind; project_state needs a viewport",
          "[devstudio][automation][qd1]") {
    Fixture fixture;
    const QJsonObject empty = fixture.result(QStringLiteral("history"));
    const QJsonObject kinds = empty.value(QStringLiteral("kinds")).toObject();
    for (const char *kind : {"scene", "render", "graph", "materials", "material_row", "assets"}) {
        INFO("kind " << kind);
        REQUIRE(kinds.contains(QString::fromLatin1(kind)));
        CHECK_FALSE(kinds.value(QString::fromLatin1(kind))
                        .toObject()
                        .value(QStringLiteral("undo"))
                        .toObject()
                        .value(QStringLiteral("found"))
                        .toBool());
    }
    CHECK(kinds.value(QStringLiteral("materials")).toObject().value(QStringLiteral("redo")).isNull());

    // The scene's are buttons (the Inspector's), the render edits' are actions.
    auto *scene_undo = new QPushButton(QStringLiteral("Undo"), fixture.window.centralWidget());
    scene_undo->setObjectName(QStringLiteral("pelican.inspectorUndo"));
    int scene_undos = 0;
    QObject::connect(scene_undo, &QPushButton::clicked, [&scene_undos] { ++scene_undos; });
    auto *render_undo = new QAction(QStringLiteral("Undo Render Edit"), &fixture.window);
    render_undo->setObjectName(QStringLiteral("pelican.edit.undoRenderEdit"));
    render_undo->setToolTip(QStringLiteral("Undo Edit pass present"));
    int render_undos = 0;
    QObject::connect(render_undo, &QAction::triggered, [&render_undos] { ++render_undos; });
    fixture.window.addAction(render_undo);
    scene_undo->show();
    QApplication::processEvents();

    const QJsonObject read = fixture.result(QStringLiteral("history"))
                                 .value(QStringLiteral("kinds"))
                                 .toObject();
    const QJsonObject render = read.value(QStringLiteral("render"))
                                   .toObject()
                                   .value(QStringLiteral("undo"))
                                   .toObject();
    CHECK(render.value(QStringLiteral("found")).toBool());
    CHECK(render.value(QStringLiteral("tooltip")).toString() == QStringLiteral("Undo Edit pass present"));

    fixture.result(QStringLiteral("history"),
                   QJsonObject{{QStringLiteral("kind"), QStringLiteral("scene")},
                               {QStringLiteral("step"), QStringLiteral("undo")}});
    fixture.result(QStringLiteral("history"),
                   QJsonObject{{QStringLiteral("kind"), QStringLiteral("render")},
                               {QStringLiteral("step"), QStringLiteral("undo")}});
    CHECK(scene_undos == 1);
    CHECK(render_undos == 1);

    // Switched off is refused, as click refuses it; a history with no Redo
    // says so; a kind that is not one is named.
    render_undo->setEnabled(false);
    const AutomationReply off = fixture.run(
        QStringLiteral("history"),
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("render")},
                    {QStringLiteral("step"), QStringLiteral("undo")}});
    CHECK_FALSE(off.ok);
    CHECK(off.message.contains(QStringLiteral("disabled")));
    CHECK(render_undos == 1);
    const AutomationReply no_redo = fixture.run(
        QStringLiteral("history"),
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("materials")},
                    {QStringLiteral("step"), QStringLiteral("redo")}});
    CHECK_FALSE(no_redo.ok);
    CHECK(no_redo.message.contains(QStringLiteral("no Redo")));
    CHECK_FALSE(fixture.run(QStringLiteral("history"),
                            QJsonObject{{QStringLiteral("kind"), QStringLiteral("journal")},
                                        {QStringLiteral("step"), QStringLiteral("undo")}})
                    .ok);

    const AutomationReply state = fixture.run(QStringLiteral("project_state"));
    CHECK_FALSE(state.ok);
    CHECK(state.code == QStringLiteral("automation.engine_not_ready@1"));
}

// Q-16: Qt drops a press on a switched-off widget without a word. A click that
// answered "ok" anyway let a scenario go on as if it had pressed, and fail
// steps later on what the press never did.
TEST_CASE("Q-16: click refuses a disabled widget by name",
          "[devstudio][automation][q16]") {
    Fixture fixture;
    int attaches = 0;
    QObject::connect(fixture.locked, &QPushButton::clicked,
                     [&attaches] { ++attaches; });
    const QJsonObject attach{
        {QStringLiteral("name"), QStringLiteral("pelican.testAttach")}};

    const AutomationReply refused = fixture.run(QStringLiteral("click"), attach);
    CHECK_FALSE(refused.ok);
    CHECK(refused.code == QStringLiteral("automation.bad_request@1"));
    CHECK(refused.message.contains(QStringLiteral("pelican.testAttach")));
    CHECK(refused.message.contains(QStringLiteral("disabled")));
    CHECK(attaches == 0);
    // A double click is the same press twice.
    CHECK_FALSE(fixture.run(QStringLiteral("double_click"), attach).ok);
    CHECK(attaches == 0);

    // Switched off by the panel it sits in, not by itself.
    auto *panel = new QWidget(fixture.window.centralWidget());
    auto *panel_layout = new QVBoxLayout(panel);
    auto *inner = new QPushButton(QStringLiteral("Undo"), panel);
    inner->setObjectName(QStringLiteral("pelican.testInnerUndo"));
    panel_layout->addWidget(inner);
    fixture.window.centralWidget()->layout()->addWidget(panel);
    panel->setEnabled(false);
    QApplication::processEvents();
    int undos = 0;
    QObject::connect(inner, &QPushButton::clicked, [&undos] { ++undos; });
    const AutomationReply under_off = fixture.run(
        QStringLiteral("click"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testInnerUndo")}});
    CHECK_FALSE(under_off.ok);
    CHECK(under_off.message.contains(QStringLiteral("pelican.testInnerUndo")));
    CHECK(undos == 0);

    // The control: the same buttons, switched on, are pressed.
    fixture.locked->setEnabled(true);
    panel->setEnabled(true);
    QApplication::processEvents();
    fixture.result(QStringLiteral("click"), attach);
    CHECK(attaches == 1);
    fixture.result(QStringLiteral("click"),
                   QJsonObject{{QStringLiteral("name"),
                                QStringLiteral("pelican.testInnerUndo")}});
    CHECK(undos == 1);
}

// Q-17: setCurrentIndex works on a switched-off combo, where a person cannot
// pick; and a select that answered "ok" while the panel put its value back
// let a scenario go on as if it had chosen, and fail steps later on what the
// choice never did.
TEST_CASE("Q-17: select refuses a disabled widget, and fails when the choice does not take",
          "[devstudio][automation][q17]") {
    Fixture fixture;
    const auto choose = [&fixture](const char *name, const char *text) {
        return fixture.run(QStringLiteral("select"),
                           QJsonObject{{QStringLiteral("name"), QString::fromLatin1(name)},
                                       {QStringLiteral("text"), QString::fromLatin1(text)}});
    };

    // Switched off by the panel it sits in, not by itself.
    auto *panel = new QWidget(fixture.window.centralWidget());
    auto *panel_layout = new QVBoxLayout(panel);
    auto *body = new QComboBox(panel);
    body->setObjectName(QStringLiteral("pelican.testBody"));
    body->addItems({QStringLiteral("(nothing)"), QStringLiteral("draw")});
    body->setCurrentIndex(1);
    panel_layout->addWidget(body);
    fixture.window.centralWidget()->layout()->addWidget(panel);
    int bodies = 0;
    QObject::connect(body, &QComboBox::currentIndexChanged, [&bodies] { ++bodies; });
    panel->setEnabled(false);
    QApplication::processEvents();
    const AutomationReply under_off = choose("pelican.testBody", "(nothing)");
    CHECK_FALSE(under_off.ok);
    CHECK(under_off.code == QStringLiteral("automation.bad_request@1"));
    CHECK(under_off.message.contains(QStringLiteral("pelican.testBody")));
    CHECK(under_off.message.contains(QStringLiteral("disabled")));
    CHECK(bodies == 0);
    CHECK(body->currentText() == QStringLiteral("draw"));
    // A tree switched off itself: the same refusal, no press.
    fixture.tree->setEnabled(false);
    const auto tree_before = fixture.tree_selections.size();
    const AutomationReply tree_off = choose("pelican.testOutliner", "Camera");
    CHECK_FALSE(tree_off.ok);
    CHECK(tree_off.message.contains(QStringLiteral("disabled")));
    CHECK(fixture.tree_selections.size() == tree_before);
    fixture.tree->setEnabled(true);

    // Switched on: a panel that refuses the choice puts its value back.
    panel->setEnabled(true);
    QApplication::processEvents();
    bool refuse = true;
    QObject::connect(body, &QComboBox::currentIndexChanged, [&refuse, body] {
        if (!refuse) return;
        const QSignalBlocker block{body};
        body->setCurrentIndex(1);
    });
    const AutomationReply refused = choose("pelican.testBody", "(nothing)");
    CHECK_FALSE(refused.ok);
    CHECK(refused.code == QStringLiteral("automation.bad_request@1"));
    CHECK(refused.message.contains(QStringLiteral("\"pelican.testBody\" did not take \"(nothing)\"")));
    CHECK(bodies == 1);

    // The control: a panel that keeps the choice, and a picker that uses its
    // entry and goes back to its placeholder, both took it.
    refuse = false;
    CHECK(choose("pelican.testBody", "(nothing)").ok);
    CHECK(body->currentText() == QStringLiteral("(nothing)"));
    auto *picker = new QComboBox(panel);
    picker->setObjectName(QStringLiteral("pelican.testPicker"));
    picker->setPlaceholderText(QStringLiteral("Pick..."));
    picker->addItems({QStringLiteral("blur"), QStringLiteral("tonemap")});
    picker->setCurrentIndex(-1);
    panel_layout->addWidget(picker);
    QString picked;
    QObject::connect(picker, &QComboBox::currentIndexChanged, [&picked, picker](int index) {
        if (index < 0) return;
        picked = picker->itemText(index);
        const QSignalBlocker block{picker};
        picker->setCurrentIndex(-1);
    });
    CHECK(choose("pelican.testPicker", "tonemap").ok);
    CHECK(picked == QStringLiteral("tonemap"));
}

// QA-1: a combo or a list filled by an engine's answer gets its entries a few
// milliseconds after the row it sits in is on screen. A select that refused
// the moment the entry was not there failed the scenario now and then, and
// passed when it was run again.
TEST_CASE("QA-1: select waits for an entry, a row or a widget that comes late, and says what it held when "
          "none comes",
          "[devstudio][automation][qa1]") {
    Fixture fixture;
    const auto select = [&fixture](const QString &name, const QString &text, int timeout_ms = 0) {
        QJsonObject fields{{QStringLiteral("name"), name}, {QStringLiteral("text"), text}};
        if (timeout_ms > 0) fields.insert(QStringLiteral("timeout_ms"), timeout_ms);
        return fixture.run(QStringLiteral("select"), fields);
    };

    // An entry the panel adds when its answer comes, 200 ms later.
    QTimer::singleShot(200, fixture.combo, [&fixture] {
        fixture.combo->addItem(QStringLiteral("late_curve"), QStringLiteral("late_curve"));
    });
    const AutomationReply late = select(QStringLiteral("pelican.testScopes"), QStringLiteral("late_curve"));
    INFO(describe(late).toStdString());
    REQUIRE(late.ok);
    CHECK(fixture.combo->currentText() == QStringLiteral("late_curve"));
    CHECK(fixture.combo_selections == QStringList{QStringLiteral("late_curve")});
    CHECK(late.result.toObject().value(QStringLiteral("waited_ms")).toDouble() >= 150.0);

    // A row of a tree, the same.
    QTimer::singleShot(200, fixture.tree, [&fixture] {
        new QTreeWidgetItem(fixture.tree->topLevelItem(0), QStringList{QStringLiteral("Sphere")});
    });
    const AutomationReply row = select(QStringLiteral("pelican.testOutliner"), QStringLiteral("Sphere"));
    INFO(describe(row).toStdString());
    REQUIRE(row.ok);
    REQUIRE(fixture.tree->currentItem() != nullptr);
    CHECK(fixture.tree->currentItem()->text(0) == QStringLiteral("Sphere"));

    // A combo whose panel is built when its answer comes.
    QTimer::singleShot(200, &fixture.window, [&fixture] {
        auto *built = new QComboBox(fixture.window.centralWidget());
        built->setObjectName(QStringLiteral("pelican.testLateCombo"));
        built->addItems({QStringLiteral("draw"), QStringLiteral("skip")});
        fixture.window.centralWidget()->layout()->addWidget(built);
    });
    const AutomationReply built = select(QStringLiteral("pelican.testLateCombo"), QStringLiteral("skip"));
    INFO(describe(built).toStdString());
    CHECK(built.ok);

    // None comes: refused once the time is up, saying how long it waited and
    // what the combo held -- the two things a reader of the failure needs.
    QElapsedTimer clock;
    clock.start();
    const AutomationReply never = select(QStringLiteral("pelican.testScopes"), QStringLiteral("never"), 300);
    CHECK(clock.elapsed() >= 290);
    CHECK_FALSE(never.ok);
    CHECK(never.code == QStringLiteral("automation.bad_request@1"));
    INFO(never.message.toStdString());
    CHECK(never.message.contains(QStringLiteral("\"pelican.testScopes\" has no entry never")));
    CHECK(never.message.contains(QStringLiteral("waited 3")));
    CHECK(never.message.contains(QStringLiteral("\"root\", \"gain_curve\", \"late_curve\"")));
    const AutomationReply no_row = select(QStringLiteral("pelican.testOutliner"), QStringLiteral("Hero"), 100);
    CHECK_FALSE(no_row.ok);
    CHECK(no_row.message.contains(QStringLiteral("holds no row Hero")));
    CHECK(no_row.message.contains(QStringLiteral("\"Camera\"")));
    const AutomationReply nobody = select(QStringLiteral("pelican.testNobody"), QStringLiteral("draw"), 100);
    CHECK_FALSE(nobody.ok);
    CHECK(nobody.code == QStringLiteral("automation.widget_missing@1"));

    // Switched off is not waited for (Q-17): a person could not pick in it
    // now, and a wait that let the pick through once it came on would hide
    // that. Refused at once, though its entry is on the way.
    fixture.combo->setEnabled(false);
    QTimer::singleShot(100, fixture.combo, [&fixture] {
        fixture.combo->addItem(QStringLiteral("later"));
        fixture.combo->setEnabled(true);
    });
    clock.restart();
    const AutomationReply off = select(QStringLiteral("pelican.testScopes"), QStringLiteral("later"));
    CHECK(clock.elapsed() < 90);
    CHECK_FALSE(off.ok);
    CHECK(off.message.contains(QStringLiteral("disabled")));
    QTest::qWait(150);
}

// --------------------------------------------------------------------------
// PX-2: a scenario holds the viewport at one size, so a point it reads back as
// a fraction of the picture stays on the same thing however the panels move.

TEST_CASE("PX-2: viewport_size holds the viewport while panels come and go",
          "[devstudio][automation][px2]") {
    (void)application();
    CHECK(automationCommandNames().contains(QStringLiteral("viewport_size")));

    // Studio's shape: the viewport stretches in the middle of a main window
    // and takes whatever the panels leave it.
    QMainWindow window;
    auto *centre = new QWidget(&window);
    auto *centre_layout = new QVBoxLayout(centre);
    auto *surface = new QWidget(centre);
    surface->setObjectName(QStringLiteral("pelican.viewportHost"));
    surface->setMinimumSize(1, 1);
    centre_layout->addWidget(surface, 1);
    window.setCentralWidget(centre);
    auto *side = new QDockWidget(QStringLiteral("Side"), &window);
    side->setWidget(new QWidget(side));
    side->widget()->setMinimumSize(120, 60);
    window.addDockWidget(Qt::LeftDockWidgetArea, side);
    window.resize(640, 480);
    window.show();
    QApplication::processEvents();
    AutomationHost host;
    host.window = &window;
    const auto run = [&host](const QJsonObject &fields) {
        return runAutomationCommand(host, request(QStringLiteral("viewport_size"), fields));
    };
    const QSize free_size = surface->size();
    const int side_width = side->width();
    REQUIRE(free_size != QSize(300, 200));

    const AutomationReply fixed = run(QJsonObject{{QStringLiteral("width"), 300},
                                                  {QStringLiteral("height"), 200}});
    INFO(describe(fixed).toStdString());
    REQUIRE(fixed.ok);
    // The offscreen platform has a ratio of 1: picture and layout pixels agree.
    CHECK(fixed.result[QStringLiteral("width")].toInt() == 300);
    CHECK(fixed.result[QStringLiteral("height")].toInt() == 200);
    CHECK(fixed.result[QStringLiteral("fixed")].toBool());
    CHECK(surface->size() == QSize(300, 200));
    // The window took the difference; the panel beside it kept its width.
    CHECK(side->width() == side_width);

    // A panel opened below takes its room from the window, not the viewport;
    // and the viewport is the same size once it is closed again.
    auto *below = new QDockWidget(QStringLiteral("Below"), &window);
    below->setWidget(new QWidget(below));
    below->widget()->setMinimumSize(60, 150);
    window.addDockWidget(Qt::BottomDockWidgetArea, below);
    below->show();
    QApplication::processEvents();
    CHECK(surface->size() == QSize(300, 200));
    window.resize(window.width() + 90, window.height() - 40);
    below->hide();
    QApplication::processEvents();
    CHECK(surface->size() == QSize(300, 200));

    // Refusals: no size, or no such widget.
    const AutomationReply empty = run(QJsonObject{});
    CHECK_FALSE(empty.ok);
    CHECK(empty.code == QStringLiteral("automation.bad_request@1"));
    const AutomationReply missing = run(QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.noSuchViewport")},
                                                    {QStringLiteral("width"), 300},
                                                    {QStringLiteral("height"), 200}});
    CHECK_FALSE(missing.ok);
    CHECK(missing.code == QStringLiteral("automation.widget_missing@1"));

    // The control: freed, it follows the window again.
    const AutomationReply freed = run(QJsonObject{{QStringLiteral("free"), true}});
    INFO(describe(freed).toStdString());
    REQUIRE(freed.ok);
    CHECK_FALSE(freed.result[QStringLiteral("fixed")].toBool());
    window.resize(window.width() + 50, window.height() + 30);
    QApplication::processEvents();
    CHECK(surface->size() != QSize(300, 200));
    CHECK(surface->minimumSize() == QSize(1, 1));
}

TEST_CASE("QD-1: a fixed viewport remains inside its collapsible live container",
          "[devstudio][automation][qd1][px2]") {
    (void)application();
    QMainWindow window;
    auto *centre = new QWidget(&window);
    auto *layout = new QVBoxLayout(centre);
    auto *viewport = new PelicanStudio::EmbeddedViewport(centre);
    layout->addWidget(viewport);
    window.setCentralWidget(centre);
    auto *side = new QDockWidget(QStringLiteral("Inspector"), &window);
    side->setWidget(new QWidget(side));
    side->widget()->setMinimumSize(250, 60);
    window.addDockWidget(Qt::RightDockWidgetArea, side);
    window.resize(900, 600);
    window.show();
    QApplication::processEvents();
    auto *surface = viewport->findChild<QWidget *>(QStringLiteral("pelican.viewportHost"));
    REQUIRE(surface);
    const int original_minimum = surface->parentWidget()->minimumWidth();
    AutomationHost host;
    host.window = &window;
    REQUIRE(runAutomationCommand(host, request(QStringLiteral("viewport_size"),
        {{QStringLiteral("width"), 500}, {QStringLiteral("height"), 250}})).ok);
    window.resize(550, 500);
    QApplication::processEvents();
    CHECK(surface->width() == 500);
    CHECK(surface->parentWidget()->rect().contains(surface->geometry()));
    REQUIRE(runAutomationCommand(host, request(QStringLiteral("viewport_size"),
        {{QStringLiteral("free"), true}})).ok);
    CHECK(surface->parentWidget()->minimumWidth() == original_minimum);
}

TEST_CASE("DL-1 automation paced drag and both trace recovery paths use real paints",
          "[devstudio][automation][dl1]") {
    (void)application();
    using namespace PelicanStudio;
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const QByteArray previous = qgetenv("PELICAN_STUDIO_DRAG_TRACE");
    const QString automatic_path = temporary.filePath("automatic.json");
    qputenv("PELICAN_STUDIO_DRAG_TRACE", automatic_path.toUtf8());
    const auto restore = qScopeGuard([&] {
        if (previous.isNull()) qunsetenv("PELICAN_STUDIO_DRAG_TRACE");
        else qputenv("PELICAN_STUDIO_DRAG_TRACE", previous);
    });
    QMainWindow window;
    QGraphicsScene scene;
    auto *view = new NodeCanvasView(&scene, &window);
    view->setObjectName("pelican.graphCanvasView");
    window.setCentralWidget(view);
    scene.setSceneRect(-500, -500, 1500, 1500);
    auto *node = new MovableNodeItem(nodePath(false), [](const QPointF &) {});
    node->setData(NodeGraphItemKindRole, "graph_node");
    node->setData(NodeGraphNameRole, "trace_node");
    node->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemSendsGeometryChanges);
    scene.addItem(node);
    window.resize(800, 600);
    window.show();
    QApplication::processEvents();
    AutomationHost host;
    host.window = &window;
    REQUIRE(automationCommandNames().contains("drag_paced"));
    REQUIRE(automationCommandNames().contains("trace_dump"));
    const auto drag = runAutomationCommand(host, request("drag_paced", {
        {"node", "trace_node"}, {"duration_ms", 160}, {"interval_ms", 8}, {"delta", QJsonArray{80, 20}}}));
    INFO(describe(drag).toStdString());
    REQUIRE(drag.ok);
    CHECK(drag.result.toObject().value("steps").toInt() == 20);
    CHECK(drag.result.toObject().value("nominal_ms").toInt() == 160);
    CHECK(drag.result.toObject().value("scheduled_ms").toInt() == 160);
    CHECK(drag.result.toObject().value("interval_ms").toInt() == 8);
    CHECK(drag.result.toObject().value("hold_ms").toInt() == 0);
    CHECK(drag.result.toObject().value("delta").toArray() == QJsonArray{80, 20});
    CHECK(drag.result.toObject().value("node") == "trace_node");
    CHECK(drag.result.toObject().value("pick") == "node");
    CHECK((node->pos() - QPointF{80, 20}).manhattanLength() < 2);
    QFile automatic{automatic_path};
    REQUIRE(automatic.open(QIODevice::ReadOnly));
    const auto file_trace = QJsonDocument::fromJson(automatic.readAll()).object();
    CHECK(file_trace.value("moves").toInt() == 20);
    CHECK(file_trace.value("painted_moves").toInt() > 0);
    REQUIRE(file_trace.value("records").toArray().size() > 20);
    const QString requested_path = temporary.filePath("requested.json");
    const auto dump = runAutomationCommand(host, request("trace_dump", {{"records", false}, {"path", requested_path}}));
    REQUIRE(dump.ok);
    CHECK_FALSE(dump.result.toObject().contains("records"));
    CHECK(dump.result.toObject().value("moves").toInt() == file_trace.value("moves").toInt());
    CHECK(dump.result.toObject().value("summary") == file_trace.value("summary"));
    CHECK(QFile::exists(requested_path));
}


TEST_CASE("DL-1 paint coverage rejects an actual mid-gesture painting blackout", "[devstudio][automation][dl1]") {
    (void)application();
    using namespace PelicanStudio;
    struct Blackout : QObject {
        int paints = 0;
        bool suppress = false, restore_on_release = true;
        bool eventFilter(QObject *object, QEvent *event) override {
            if (event->type() == QEvent::MouseButtonRelease && restore_on_release) {
                suppress = false;
                static_cast<QWidget *>(object)->update();
            }
            return event->type() == QEvent::Paint && suppress && ++paints > 2;
        }
    } filter;
    QGraphicsScene scene;
    NodeCanvasView view{&scene};
    scene.setSceneRect(-500, -500, 1500, 1500);
    auto *node = new MovableNodeItem(nodePath(false), [](const QPointF &) {});
    node->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemSendsGeometryChanges);
    scene.addItem(node);
    view.resize(800, 600);
    view.show();
    view.viewport()->installEventFilter(&filter);
    for (int mode : {0, 1, 2}) {
        const bool blackout = mode != 0;
        filter.suppress = false;
        filter.restore_on_release = mode != 2;
        node->setPos(0, 0);
        view.centerOn(node);
        QApplication::processEvents();
        view.dragTrace().setEnabled(blackout); // check restoration in both directions
        filter.suppress = blackout;
        filter.paints = 0;
        const auto result = view.dragPaced(view.mapFromScene(QPointF{60, 15}), QPoint{80, 20}, 240, 8);
        CHECK(result.value("steps").toInt() == 30);
        CHECK(view.dragTrace().enabled() == blackout);
        const auto trace = view.dragTrace().dump();
        INFO(QJsonDocument{trace.value("summary").toObject()}.toJson().toStdString());
        CHECK(trace.value("final_position_painted").toBool() == (mode != 2));
        CHECK(trace.value("acceptance").toObject().value("paint_coverage").toBool() == (mode != 2));
        CHECK(trace.value("acceptance").toObject().value("passed").toBool() == !blackout);
        if (blackout) {
            CHECK(trace.value("painted_moves").toInt() > 0);
            CHECK(trace.value("unpainted_moves").toInt() > 10);
            CHECK(trace.value("unpainted_tail_ms").toDouble() > 100);
        } else CHECK(trace.value("unpainted_tail_ms").toDouble() <= 33.3);
    }
}

TEST_CASE("DL-2 distinguishes blackout final latency painted hold and coalescing", "[devstudio][automation][dl2]") {
    (void)application();
    using namespace PelicanStudio;
    QGraphicsScene scene;
    auto *node = new MovableNodeItem(nodePath(false), [](const QPointF &) {});
    node->setFlag(QGraphicsItem::ItemIsMovable);
    scene.addItem(node);
    for (int mode : {0, 1, 2, 3, 4, 5}) {
        INFO("mode: " << mode);
        NodeDragTrace trace;
        trace.setEnabled(true);
        trace.begin(scene, node);
        // A long healthy prefix keeps one final outlier below latency p95.
        const qint64 base = NodeDragTrace::now() - (mode == 1 || mode == 5 ? 2'008'000'000LL : 3'000'000'000LL);
        const auto ns = [&](double ms) { return base + qint64(ms * 1e6); };
        for (int i = 1; i <= 250; ++i) {
            const auto move = trace.nextMove();
            trace.append("mouse_move", ns(i * 8), ns(i * 8 + .1), move);
            trace.append("position", ns(i * 8 + .1), ns(i * 8 + .1), move);
            const bool blackout = mode == 0 && i >= 100 && i < 110;
            const bool coalesced = mode == 3 && i % 2 != 0;
            if (!blackout && !coalesced) {
                const double delay = i == 250 ? (mode == 1 ? 24 : mode == 5 ? 300 : 1) : 1;
                trace.append("paint", ns(i * 8 + delay - .1), ns(i * 8 + delay), move);
            }
        }
        if (mode == 4) { // A no-op final move still refers to the last changed position.
            trace.append("mouse_move", ns(2008), ns(2008.1), trace.nextMove());
        }
        // Modes 1/5 place the final paint after release; others hold >100 ms on a painted position.
        trace.release();
        const auto result = trace.dump();
        const auto acceptance = result.value("acceptance").toObject();
        INFO(QJsonDocument{result.value("summary").toObject()}.toJson().toStdString());
        CHECK(acceptance.value("measured").toBool());
        CHECK(acceptance.value("paint_coverage").toBool());
        CHECK(result.value("final_position_painted").toBool());
        CHECK(result.value("summary").toObject().value("input_to_paint").toObject().value("p95_ms").toDouble() <= 16.7);
        CHECK(acceptance.value("passed").toBool() == (mode >= 2 && mode <= 4));
        CHECK(result.value("final_position_latency_ms").toDouble() == Catch::Approx(mode == 1 ? 24 : mode == 5 ? 300 : 1));
        if (mode == 0) CHECK(result.value("unpainted_tail_ms").toDouble() == Catch::Approx(81));
        if (mode == 2 || mode == 4) CHECK(result.value("unpainted_tail_ms").toDouble() == Catch::Approx(1));
        if (mode == 3) {
            CHECK(result.value("unpainted_moves").toInt() == 125);
            CHECK(result.value("unpainted_tail_ms").toDouble() == Catch::Approx(9));
        }
        const auto intervals = result.value("unpainted_intervals").toArray();
        REQUIRE_FALSE(intervals.isEmpty());
        CHECK(intervals.last().toObject().value("resolved").toBool());
    }
}

TEST_CASE("DL-2 final paint remains monitored beyond the release collection timer", "[devstudio][automation][dl2]") {
    (void)application();
    using namespace PelicanStudio;
    struct FinalDelay : QObject {
        int moves = 0;
        bool suppress = false;
        bool eventFilter(QObject *object, QEvent *event) override {
            if (event->type() == QEvent::MouseMove && ++moves == 20) suppress = true;
            if (event->type() == QEvent::MouseButtonRelease) {
                auto *port = static_cast<QWidget *>(object);
                QTimer::singleShot(550, this, [this, port] {
                    suppress = false;
                    port->update();
                });
            }
            return event->type() == QEvent::Paint && suppress;
        }
    } filter;
    QGraphicsScene scene;
    NodeCanvasView view{&scene};
    scene.setSceneRect(-500, -500, 1500, 1500);
    auto *node = new MovableNodeItem(nodePath(false), [](const QPointF &) {});
    node->setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemSendsGeometryChanges);
    scene.addItem(node);
    view.resize(800, 600);
    view.show();
    view.centerOn(node);
    QApplication::processEvents();
    view.viewport()->installEventFilter(&filter);
    view.dragPaced(view.mapFromScene(QPointF{60, 15}), QPoint{80, 20}, 160, 8);
    REQUIRE(filter.moves == 20);
    CHECK(view.dragTrace().recording());
    CHECK_FALSE(view.dragTrace().finalPositionPainted());
    QElapsedTimer wait;
    wait.start();
    while (view.dragTrace().recording() && wait.elapsed() < 1500) QTest::qWait(10);
    CHECK_FALSE(view.dragTrace().recording());
    CHECK(view.dragTrace().finalPositionPainted());
    const auto trace = view.dragTrace().dump();
    CHECK(trace.value("final_position_painted").toBool());
    CHECK(trace.value("final_position_latency_ms").toDouble() >= 550);
    CHECK(trace.value("summary").toObject().value("paint_interval").toObject().value("max_ms").toDouble() >= 550);
    CHECK_FALSE(trace.value("acceptance").toObject().value("passed").toBool());
}

TEST_CASE("DL-1 paced request refuses wrong types ranges and ambiguous selection", "[devstudio][automation][dl1]") {
    (void)application();
    AutomationHost host;
    const std::vector<std::pair<QJsonObject, QString>> cases{
        {{{"node", "n"}, {"delta", QJsonArray{"bad",40}}}, "delta"},
        {{{"node", "n"}, {"delta", QJsonArray{1}}}, "delta"},
        {{{"node", "n"}, {"delta", QJsonArray{1.5,2}}}, "delta"},
        {{{"node", "n"}, {"duration_ms", -1}}, "duration_ms"},
        {{{"node", "n"}, {"duration_ms", 10001}}, "duration_ms"},
        {{{"node", "n"}, {"duration_ms", "2000"}}, "duration_ms"},
        {{{"node", "n"}, {"interval_ms", 0}}, "interval_ms"},
        {{{"node", "n"}, {"interval_ms", 101}}, "interval_ms"},
        {{{"node", "n"}, {"hold_ms", true}}, "hold_ms"},
        {{{"node", "n"}, {"hold_ms", 2001}}, "hold_ms"},
        {{{"node", "n"}, {"duration_ms", 4}, {"interval_ms", 8}}, "duration_ms"},
        {{{"node", "n"}, {"pick", "most_connected_derived"}}, "selection"},
        {{{"pick", "invalid"}}, "pick"},
        {{{"node", 7}}, "node"},
        {{}, "selection"}
    };
    for (const auto &[fields, field] : cases) {
        const auto result = runAutomationCommand(host, request("drag_paced", fields));
        CHECK_FALSE(result.ok);
        CHECK(result.code == "automation.drag_paced.invalid_" + field + "@1");
    }
}

TEST_CASE("FX-1 fold scenario waits for the automatic plan read before Refresh",
          "[devstudio][automation][fx1]") {
    Fixture fixture;
    fixture.button->setObjectName(QStringLiteral("pelican.framePlanRefresh"));
    int clicks = 0;
    QObject::connect(fixture.button, &QPushButton::clicked, &fixture.window,
                     [&] { ++clicks; });
    QFile scenario{QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(
        QStringLiteral("studio_scenarios/graph_fold_decisions.json"))};
    REQUIRE(scenario.open(QIODevice::ReadOnly));
    const auto steps = QJsonDocument::fromJson(scenario.readAll()).object()
                           .value(QStringLiteral("steps")).toArray();
    REQUIRE_FALSE(steps.isEmpty());
    int visits = 0;
    for (const auto &value : steps) {
        auto step = value.toObject();
        const auto name = step.value(QStringLiteral("name")).toString();
        const auto command = step.value(QStringLiteral("cmd")).toString();
        if (name == QStringLiteral("pelican.view.renderTools.frame_plan")) {
            ++visits;
            // Showing the stale plan starts a read. The pipe's next command
            // can arrive before its reply; no engine or GPU is needed here.
            fixture.button->setEnabled(false);
            QTimer::singleShot(100, fixture.button, [button = fixture.button] {
                button->setEnabled(true);
            });
        } else if (name == QStringLiteral("pelican.framePlanRefresh")) {
            INFO("scenario step: " << step.value(QStringLiteral("label")).toString().toStdString());
            // Use the scenario's actual commands, with only a shorter CPU
            // timeout. Removing its wait must expose the disabled click.
            if (command == QStringLiteral("wait")) step.insert(QStringLiteral("timeout_ms"), 1000);
            const auto reply = fixture.run(command, step);
            INFO(describe(reply).toStdString());
            REQUIRE(reply.ok);
        }
    }
    CHECK(visits == 3);
    CHECK(clicks == visits);
}

TEST_CASE("UD-5 fix2 named click reveals an off-viewport loop and updates Layers",
          "[devstudio][automation][ud5][ud5-fix2]") {
    (void)application();
    using namespace PelicanStudio;
    using Json = nlohmann::json;
    GraphCanvasWidget widget{GraphCanvasReadCapability{}};
    widget.resize(1100, 800);
    widget.show();
    auto &model = widget.model();
    model.setConnected(true);
    for (const auto &call : model.takeRpcRequests()) {
        if (call.method != "get_graph_instances") continue;
        const Json document{
            {"source_reference", "project://passes/main.json"},
            {"source_digest", {{"hex", "loop-selection"}}},
            {"root_schema", "pelican.graph"},
            {"nodes", Json::array({
                {{"id", "render_fullscreen_4"}, {"op", "render.fullscreen"}},
                {{"id", "loop"}, {"op", "graph.loop"},
                 {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                  {"branches", Json::array({{{"body", "body"}}})}}}}})},
            {"definitions", {{"body", {{"nodes", Json::array()}, {"outputs", Json::object()}}}}},
            {"layout", {{"nodes", {{"loop", {{"x", 4000}, {"y", -1500}}},
                                     {"render_fullscreen_4", {{"x", 0}, {"y", 0}}}}}}}};
        model.receiveRpcResult(call.request_id, document.dump());
    }
    widget.scene().repopulate();
    widget.scene().selectNode("render_fullscreen_4");
    auto *view = widget.findChild<QGraphicsView *>("pelican.graphCanvasView");
    auto *layers = widget.findChild<QPushButton *>("pelican.graphCanvasLayersToggle");
    REQUIRE(view);
    REQUIRE(layers);
    QApplication::processEvents();
    view->centerOn(0, 0);
    QGraphicsItem *loop = nullptr;
    for (auto *item : widget.scene().items())
        if (item->data(NodeGraphItemKindRole).toString() == "graph_node" &&
            item->data(NodeGraphNameRole).toString() == "loop") loop = item;
    REQUIRE(loop);
    const auto spot = loop->data(NodeGraphHotspotRole).toPointF();
    REQUIRE_FALSE(view->viewport()->rect().contains(view->mapFromScene(loop->mapToScene(spot))));
    REQUIRE(layers->text().contains("Layers: render_fullscreen_4"));
    const auto transform = view->transform();
    AutomationHost host;
    host.window = &widget;
    const auto clicked = runAutomationCommand(host, request("click", {
        {"name", "pelican.graphCanvasView::graph_node#loop"}}));
    INFO(describe(clicked).toStdString());
    REQUIRE(clicked.ok);
    // Selection is immediate; the detail pane deliberately waits until the
    // press/release gesture has finished before rebuilding its editors.
    QElapsedTimer settled;
    settled.start();
    while (!layers->text().contains("Layers: loop") && settled.elapsed() < 1000)
        QTest::qWait(5);
    const auto point = clicked.result.toObject().value("point").toArray();
    CHECK(view->viewport()->rect().contains(QPoint{point[0].toInt(), point[1].toInt()}));
    CHECK(widget.scene().selectedNode() == std::optional<std::string>{"loop"});
    CHECK(layers->text().contains("Layers: loop"));
    CHECK(view->transform() == transform);
    CHECK_FALSE(widget.scene().dragging());
    // Once in view, another named click must not pan again.
    const auto centre = view->mapToScene(view->viewport()->rect().center());
    REQUIRE(runAutomationCommand(host, request("click", {
        {"name", "pelican.graphCanvasView::graph_node#loop"}})).ok);
    CHECK(view->mapToScene(view->viewport()->rect().center()) == centre);
    // Explicit positions retain their meaning and must never auto-pan.
    REQUIRE(runAutomationCommand(host, request("click", {
        {"name", "pelican.graphCanvasView::graph_node#loop"},
        {"scene", QJsonArray{0, 0}}})).ok);
    CHECK(view->mapToScene(view->viewport()->rect().center()) == centre);
}


TEST_CASE("FX-8 a named click finds the loop heading under an overlapping body node",
          "[devstudio][automation][fx8][fx8-fix3]") {
    (void)application();
    using namespace PelicanStudio;
    using Json = nlohmann::json;
    GraphCanvasWidget widget{GraphCanvasReadCapability{}};
    widget.resize(1100, 800);
    widget.show();
    auto &model = widget.model();
    model.setConnected(true);
    for (const auto &call : model.takeRpcRequests()) {
        if (call.method != "get_graph_instances") continue;
        const Json document{
            {"source_reference", "project://passes/main.json"},
            {"source_digest", {{"hex", "loop-selection"}}},
            {"root_schema", "pelican.graph"},
            {"nodes", Json::array({
                {{"id", "render_fullscreen_4"}, {"op", "render.fullscreen"}},
                {{"id", "loop"}, {"op", "graph.loop"},
                 {"parameters", {{"over", {{"range", {{"count", 2}}}}},
                                  {"branches", Json::array({{{"body", "body"}}})}}}}})},
            {"definitions", {{"body", {{"nodes", Json::array({{{"id", "render_fullscreen_4"}, {"op", "render.fullscreen"}}})}, {"outputs", Json::object()}}}}},
            {"layout", {{"nodes", {{"loop", {{"x", 4000}, {"y", -1500}}},
                                     {"render_fullscreen_4", {{"x", 0}, {"y", 0}}}}}}}};
        model.receiveRpcResult(call.request_id, document.dump());
    }
    widget.scene().repopulate();
    widget.scene().selectNode("render_fullscreen_4");
    auto *view = widget.findChild<QGraphicsView *>("pelican.graphCanvasView");
    auto *layers = widget.findChild<QPushButton *>("pelican.graphCanvasLayersToggle");
    REQUIRE(view);
    REQUIRE(layers);
    QApplication::processEvents();
    view->centerOn(0, 0);
    QGraphicsItem *body = nullptr;
    QGraphicsItem *loop = nullptr;
    for (auto *item : widget.scene().items()) {
        if (item->data(NodeGraphItemKindRole).toString() == "graph_node" &&
            item->data(GraphCanvasZoneScopeRole).toString() == "body") body = item;
        if (item->data(NodeGraphItemKindRole).toString() == "graph_node" &&
            item->data(NodeGraphNameRole).toString() == "loop") loop = item;
    }
    REQUIRE(loop);
    REQUIRE(body);
    REQUIRE(loop->isAncestorOf(body));
    const auto spot = loop->data(NodeGraphHotspotRole).toPointF();
    // Move the real body node over the heading without asking the automatic
    // zone sizing callback to relocate that heading away from the overlap.
    const QPointF offset = body->mapFromScene(loop->mapToScene(spot)) -
                           body->boundingRect().center();
    body->setTransform(QTransform::fromTranslate(offset.x(), offset.y()));
    view->centerOn(loop->mapToScene(spot));
    const QPoint original = view->mapFromScene(loop->mapToScene(spot));
    auto *hit = view->itemAt(original);
    REQUIRE(hit);
    REQUIRE((hit == body || body->isAncestorOf(hit)));
    // Reproduce what an ordinary user click on the hidden heading does.
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, original);
    REQUIRE(widget.scene().selectedNode() == std::optional<std::string>{"render_fullscreen_4"});
    REQUIRE(widget.scene().selectedScope() == "body");
    QTest::qWait(10);
    REQUIRE(layers->text().contains("Layers: render_fullscreen_4"));
    const auto transform = view->transform();
    AutomationHost host;
    host.window = &widget;
    const auto clicked = runAutomationCommand(host, request("click", {
        {"name", "pelican.graphCanvasView::graph_node#loop"}}));
    INFO(describe(clicked).toStdString());
    REQUIRE(clicked.ok);
    CHECK(clicked.result.toObject().value("point_reselected").toBool());
    // Selection is immediate; the detail pane deliberately waits until the
    // press/release gesture has finished before rebuilding its editors.
    QElapsedTimer settled;
    settled.start();
    while (!layers->text().contains("Layers: loop") && settled.elapsed() < 1000)
        QTest::qWait(5);
    const auto point = clicked.result.toObject().value("point").toArray();
    CHECK(view->viewport()->rect().contains(QPoint{point[0].toInt(), point[1].toInt()}));
    CHECK(widget.scene().selectedNode() == std::optional<std::string>{"loop"});
    CHECK(widget.scene().selectedScope().empty());
    CHECK(layers->text().contains("Layers: loop"));
    CHECK(view->transform() == transform);
    CHECK_FALSE(widget.scene().dragging());
}

TEST_CASE("FX-8 named click and double click reject full coverage before delivery",
          "[devstudio][automation][fx8][fx8-fix3]") {
    Fixture fixture;
    auto *target = fixture.canvas->scene()->addRect(0, 0, 120, 80);
    target->setData(PelicanStudio::NodeGraphItemKindRole, "graph_node");
    target->setData(PelicanStudio::NodeGraphNameRole, "target");
    target->setFlag(QGraphicsItem::ItemIsSelectable);
    auto *cover = fixture.canvas->scene()->addRect(30, -5, 100, 90);
    cover->setData(PelicanStudio::NodeGraphItemKindRole, "graph_node");
    cover->setData(PelicanStudio::NodeGraphNameRole, "cover");
    cover->setZValue(2);
    fixture.canvas->centerOn(60, 40);
    const QString name = fixture.canvas->objectName() + "::graph_node#target";
    for (const QString command : {QStringLiteral("click"), QStringLiteral("double_click")}) {
        INFO(command.toStdString());
        cover->setRect(30, -5, 100, 90);
        fixture.canvas->mouse.clear();
        const auto partial = fixture.run(command, {{"name", name}});
        INFO(describe(partial).toStdString());
        REQUIRE(partial.ok);
        CHECK(partial.result.toObject().value("point_reselected").toBool());
        const auto xy = partial.result.toObject().value("point").toArray();
        CHECK(fixture.canvas->itemAt(QPoint{xy[0].toInt(), xy[1].toInt()}) == target);
        // The same search order returns the same point on each call.
        const auto again = fixture.run(command, {{"name", name}});
        REQUIRE(again.ok);
        CHECK(again.result.toObject().value("point") == partial.result.toObject().value("point"));
        cover->setRect(-5, -5, 140, 100);
        fixture.canvas->mouse.clear();
        const auto covered = fixture.run(command, {{"name", name}});
        REQUIRE_FALSE(covered.ok);
        CHECK(covered.message.contains(name));
        CHECK(covered.message.contains("is covered by"));
        CHECK(covered.message.contains("graph_node#cover"));
        CHECK(fixture.canvas->mouse.empty());
        // Explicit coordinates retain their meaning, even over another item.
        REQUIRE(fixture.run(command, {{"name", name}, {"scene", QJsonArray{60, 40}}}).ok);
        cover->setVisible(false);
        // A passive overlay must not hide the target's selectable surface.
        auto *decoration = fixture.canvas->scene()->addRect(0, 0, 120, 80);
        decoration->setAcceptedMouseButtons(Qt::NoButton);
        decoration->setZValue(3);
        const auto plain = fixture.run(command, {{"name", name}});
        REQUIRE(plain.ok);
        CHECK_FALSE(plain.result.toObject().value("point_reselected").toBool());
        delete decoration;
        cover->setVisible(true);
    }
}



TEST_CASE("FX-5 history distinguishes Graph items with identical Undo labels",
          "[devstudio][automation][fx5]") {
    (void)application();
    using namespace PelicanStudio;
    using Json = nlohmann::json;
    GraphCanvasWidget widget{GraphCanvasReadCapability{}};
    auto &model = widget.model();
    AutomationHost host;
    host.window = &widget;
    const auto control = [&](const QString &direction) {
        const auto reply = runAutomationCommand(host, request(QStringLiteral("history")));
        REQUIRE(reply.ok);
        return reply.result.toObject().value("kinds").toObject()
            .value("graph").toObject().value(direction).toObject();
    };
    CHECK(control("undo").value("graph_serial").toString() == QStringLiteral("0"));
    CHECK(control("redo").value("graph_serial").toString() == QStringLiteral("0"));
    Json document{{"source_reference", "project://passes/main.json"},
                  {"source_digest", {{"hex", "initial"}}},
                  {"root_schema", "pelican.graph"},
                  {"definitions", Json::object()},
                  {"nodes", Json::array({{{"id", "node"}, {"op", "value.literal"},
                                         {"parameters", {{"value", 1}}}}})}};
    const auto deliver = [&](const std::string &method, const Json &result) {
        bool found = false;
        for (const auto &call : model.takeRpcRequests()) {
            if (call.method != method) continue;
            REQUIRE_FALSE(found);
            found = true;
            model.receiveRpcResult(call.request_id, result.dump());
        }
        REQUIRE(found);
    };
    model.setConnected(true);
    deliver("get_graph_instances", document);
    const auto commit = [&](const std::string &digest, const std::string &label) {
        deliver("edit_graph_document", {{"status", "accepted"}, {"ticket", "edit"}});
        deliver("get_edit_result", {{"status", "committed"}, {"source_digest", {{"hex", digest}}}});
        document["source_digest"]["hex"] = digest;
        document["nodes"][0]["label"] = label;
        deliver("get_graph_instances", document);
    };
    REQUIRE(model.setNodeLabel("node", "first"));
    commit("first", "first");
    const auto first = control("undo");
    REQUIRE(first.value("graph_serial").toString() != QStringLiteral("0"));
    REQUIRE(model.setNodeLabel("node", "second"));
    commit("second", "second");
    const auto second = control("undo");
    CHECK(second.value("text") == first.value("text"));
    CHECK(second.value("tooltip") == first.value("tooltip"));
    CHECK(second.value("enabled") == first.value("enabled"));
    CHECK(second.value("graph_serial") != first.value("graph_serial"));
    // Reading unchanged documents (as an asset-only reload does) adds none.
    model.refresh();
    deliver("get_graph_instances", document);
    CHECK(control("undo") == second);
    REQUIRE(model.undo());
    commit("first", "first");
    CHECK(control("undo").value("graph_serial") == first.value("graph_serial"));
    CHECK(control("redo").value("graph_serial") == second.value("graph_serial"));
    model.forgetHistory();
    CHECK(control("undo").value("graph_serial").toString() == QStringLiteral("0"));
    CHECK(control("redo").value("graph_serial").toString() == QStringLiteral("0"));
}


TEST_CASE("FX-8 disabled drag and named typing refuse before any input is delivered",
          "[devstudio][automation][fx8]") {
    Fixture fixture;
    fixture.canvas->setEnabled(false);
    const auto drag_request = QJsonObject{
        {"from", QJsonObject{{"name", "pelican.testCanvas"}, {"point", QJsonArray{30, 30}}}},
        {"to", QJsonObject{{"name", "pelican.testCanvas"}, {"point", QJsonArray{130, 90}}}}};
    const auto refused = fixture.run(QStringLiteral("drag"), drag_request);
    CHECK_FALSE(refused.ok);
    CHECK(refused.message == QStringLiteral("the widget named \"pelican.testCanvas\" is disabled"));
    CHECK(fixture.canvas->mouse.isEmpty());
    fixture.canvas->setEnabled(true);
    REQUIRE(fixture.run(QStringLiteral("drag"), drag_request).ok);
    CHECK(fixture.canvas->mouse.size() == 10);

    QLineEdit editor{&fixture.window};
    editor.setObjectName(QStringLiteral("pelican.testEditor"));
    editor.setText(QStringLiteral("1"));
    editor.setEnabled(false);
    int commits = 0;
    QObject::connect(&editor, &QLineEdit::returnPressed, [&] { ++commits; });
    const auto typed = fixture.run(QStringLiteral("type"), {{"name", editor.objectName()}, {"text", "4"}});
    CHECK_FALSE(typed.ok);
    CHECK(typed.message.contains(QStringLiteral("pelican.testEditor")));
    const auto keyed = fixture.run(QStringLiteral("key"), {{"name", editor.objectName()}, {"sequence", "Return"}});
    CHECK_FALSE(keyed.ok);
    CHECK(keyed.message.contains(QStringLiteral("disabled")));
    CHECK(editor.text() == QStringLiteral("1"));
    CHECK(commits == 0);
    editor.setEnabled(true);
    REQUIRE(fixture.run(QStringLiteral("type"), {{"name", editor.objectName()}, {"text", "4"}}).ok);
    REQUIRE(fixture.run(QStringLiteral("key"), {{"name", editor.objectName()}, {"sequence", "Return"}}).ok);
    CHECK(editor.text() == QStringLiteral("4"));
    CHECK(commits == 1);
}


TEST_CASE("FX-8 named socket drags frame both endpoints before delivering input",
          "[devstudio][automation][fx8]") {
    Fixture fixture;
    QGraphicsScene scene;
    fixture.canvas->setScene(&scene);
    scene.setSceneRect(-1000, -1000, 4000, 4000);
    for (auto [name, position] : {std::pair{QStringLiteral("from"), QPointF{-700, -700}},
                                  std::pair{QStringLiteral("to"), QPointF{2000, 2000}}}) {
        auto *item = scene.addRect(-5, -5, 10, 10);
        item->setPos(position);
        item->setData(PelicanStudio::NodeGraphItemKindRole, QStringLiteral("fx8_socket"));
        item->setData(PelicanStudio::NodeGraphNameRole, name);
    }
    const auto reply = fixture.run(QStringLiteral("drag"),
        {{"from", "pelican.testCanvas::fx8_socket#from"},
         {"to", "pelican.testCanvas::fx8_socket#to"}});
    INFO(describe(reply).toStdString());
    REQUIRE(reply.ok);
    CHECK(reply.result.toObject().value("path").toString() == "framed");
    CHECK(reply.result.toObject().value("scale").toDouble() == fixture.canvas->transform().m11());
    for (const auto *key : {"from", "to"}) {
        const auto point = reply.result.toObject().value(QLatin1String{key}).toArray();
        CHECK(fixture.canvas->viewport()->rect().contains(QPoint{point[0].toInt(), point[1].toInt()}));
    }
    CHECK(fixture.canvas->mouse.size() == 10);
}

namespace {
using Json = nlohmann::json;
using PelicanStudio::GraphCanvasModel;
using PelicanStudio::GraphCanvasScene;
Json fx8Pass(const std::string &name, std::size_t order, const std::string &source,
              const Json &reads, const Json &writes) {
    return {{"name", name},     {"kind", "render"}, {"order", order},   {"level", order},
            {"source", source}, {"reads", reads},   {"writes", writes}, {"reads_history", Json::array()},
            {"declaration_index", order}};
}

Json fx8Read() {
    Json document{
        {"nodes",
         Json::array({{{"id", "shade__color"}, {"op", "render.image"},
                       {"parameters", {{"format", "R8G8B8A8_UNORM"}}},
                       {"outputs", Json::array({"resource"})}},
                      {{"id", "shade"}, {"op", "render.fullscreen"},
                       {"parameters", Json::object()},
                       {"inputs", {{"color", {{"node", "shade__color"}, {"port", "resource"}}}}},
                       {"outputs", Json::array({"color"})}}})},
        {"definitions", Json::object()},
        {"layout", Json::object()}};
    auto read = document;
    read["instances"] = Json::array();
    read["source_digest"]["hex"] = "fx8-root";
    read["root_schema"] = "pelican.graph";
    read["source_reference"] = "fixture";
    return read;
}

// engine://passthrough as get_frame_plan publishes it: eight variadic inputs.
Json fx8Passthrough() {
    auto declarations = Json::array();
    for (std::size_t slot = 0; slot < 8; ++slot)
        declarations.push_back({{"binding", slot},
                                {"type", "sampled_image"},
                                {"name", "pelican_variadic_input_" + std::to_string(slot)},
                                {"variadic", true}});
    return {{"state", "declared"},
            {"fragment", "engine://passthrough"},
            {"declarations", std::move(declarations)},
            {"variadic_range", {{"first", 0}, {"max", 8}}},
            {"pass_texture_inputs", 0},
            {"pass_buffer_inputs", 0},
            {"buffer_binding_base", 8}};
}

// The graph's pass first (RS-3 puts it before the pass that reads it), then
// the rendering section's present reading what its `input` names.
Json fx8Plan(const std::vector<std::string> &inputs) {
    auto present = fx8Pass("present", 1, "project", Json(inputs), Json::array({"display"}));
    present["pass_inputs"] = fx8Passthrough();
    return {{"schema", "pelican.frame_plan"},
            {"version", 1},
            {"graph", "main_render"},
            {"loops", Json::array()},
            {"resources", Json::array({{{"name", "shade__color"}, {"kind", "image"}},
                                       {{"name", "old_image"}, {"kind", "image"}},
                                       {{"name", "display"}, {"kind", "image"}}})},
            {"nodes", Json::array({fx8Pass("shade", 0, "project", Json::array(),
                                            Json::array({"shade__color"})),
                                   present})}};
}

Json fx8Present(const std::vector<std::string> &inputs) {
    return {{"type", "fullscreen"},
            {"input", inputs},
            {"output", {{"color", "display"}}},
            {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://passthrough"}}}};
}

// get_render_authoring_context of a Blank project: present is the root's own.
Json fx8Authoring(const std::vector<std::string> &inputs) {
    const auto present = fx8Present(inputs);
    return {{"source_digest", {{"hex", "fx8-root"}}},
            {"config_kind", "direct"},
            {"source_reference", "project://passes/main_rendering_config.json"},
            {"graphs",
             Json::array({{{"name", "main_render"},
                           {"passes", Json::array({{{"name", "present"},
                                                    {"declaration_index", 0},
                                                    {"editable_root_pass", true},
                                                    {"declaration", present},
                                                    {"root_declaration", present}}})}}})}};
}

// The document, its plan and the context, answered in the order the model
// asks for them.
void openGl1e(GraphCanvasModel &model, GraphCanvasScene &scene,
              const std::vector<std::string> &inputs) {
    model.setConnected(true);
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_graph_instances")
            model.receiveRpcResult(request.request_id, fx8Read().dump());
    bool planned = false;
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_frame_plan") {
            model.receiveRpcResult(request.request_id, fx8Plan(inputs).dump());
            planned = true;
        }
    REQUIRE(planned);
    // Decision 1: a plan with a pass of the rendering section asks for the
    // authoring context, which says what the pass's `input` is.
    bool authored = false;
    for (const auto &request : model.takeRpcRequests())
        if (request.method == "get_render_authoring_context") {
            model.receiveRpcResult(request.request_id, fx8Authoring(inputs).dump());
            authored = true;
        }
    REQUIRE(authored);
    scene.setModel(&model);
}


}
TEST_CASE("FX-8 a named drag replaces present input at a subpixel zoom",
          "[devstudio][automation][fx8]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    QMainWindow window;
    auto *view = new PelicanStudio::NodeCanvasView(&scene, &window);
    view->setObjectName(QStringLiteral("pelican.graphCanvasView"));
    view->setScene(&scene);
    window.setCentralWidget(view);
    window.resize(900, 450);
    window.show();
    openGl1e(model, scene, {"old_image"});
    std::vector<PelicanStudio::FramePlanEditRequest> sent;
    scene.passInputEditRequested = [&](const PelicanStudio::FramePlanEditRequest &edit,
                                       std::function<PelicanStudio::FramePlanEditRequest()>) {
        sent.push_back(edit);
    };
    QApplication::processEvents();
    QGraphicsItem *from = nullptr;
    for (auto *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() == "graph_socket" &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == "shade" &&
            item->data(PelicanStudio::GraphCanvasSocketDirectionRole).toString() == "output")
            from = item;
    REQUIRE(from != nullptr);
    view->setTransform(QTransform::fromScale(0.02, 0.02));
    view->centerOn(from->sceneBoundingRect().center());
    // Both endpoints are on screen, but rounding the tiny socket's centre
    // to an integer viewport pixel can miss its entire shape.
    const auto screen = view->viewportTransform().map(from->sceneBoundingRect().center());
    from->moveBy((std::floor(screen.x()) + 0.45 - screen.x()) / 0.02,
                 (std::floor(screen.y()) + 0.45 - screen.y()) / 0.02);
    const auto point = view->mapFromScene(from->sceneBoundingRect().center());
    REQUIRE(view->viewport()->rect().contains(point));
    REQUIRE_FALSE(from->contains(from->mapFromScene(view->mapToScene(point))));
    AutomationHost host;
    host.window = &window;
    const auto reply = runAutomationCommand(host, request("drag", {
        {"from", "pelican.graphCanvasView::graph_socket#shade/out:color"},
        {"to", "pelican.graphCanvasView::graph_derived_socket#present/old_image"}}));
    INFO(describe(reply).toStdString());
    REQUIRE(reply.ok);
    CHECK(reply.result.toObject().value("path").toString() == "framed");
    CHECK(reply.result.toObject().value("scale").toDouble() == view->transform().m11());
    REQUIRE(sent.size() == 1);
    CHECK(sent.front().method == "edit_preset_pass");
    CHECK(sent.front().params.at("pass_name") == "present");
    CHECK(sent.front().params.at("input") == Json::array({"shade__color"}));
    CHECK_FALSE(scene.dragging());
}

TEST_CASE("FX-8 a named drag pans between distant sockets at a usable zoom",
          "[devstudio][automation][fx8]") {
    (void)application();
    GraphCanvasModel model;
    GraphCanvasScene scene;
    QMainWindow window;
    auto *view = new PelicanStudio::NodeCanvasView(&scene, &window);
    view->setObjectName(QStringLiteral("pelican.graphCanvasView"));
    view->setScene(&scene);
    window.setCentralWidget(view);
    window.resize(900, 450);
    window.show();
    openGl1e(model, scene, {"old_image"});
    std::vector<PelicanStudio::FramePlanEditRequest> sent;
    scene.passInputEditRequested = [&](const PelicanStudio::FramePlanEditRequest &edit,
                                       std::function<PelicanStudio::FramePlanEditRequest()>) {
        sent.push_back(edit);
    };
    QApplication::processEvents();
    QGraphicsItem *from = nullptr;
    for (auto *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() == "graph_socket" &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == "shade" &&
            item->data(PelicanStudio::GraphCanvasSocketDirectionRole).toString() == "output")
            from = item;
    REQUIRE(from != nullptr);
    QGraphicsItem *to = nullptr;
    for (auto *item : scene.items())
        if (item->data(PelicanStudio::NodeGraphItemKindRole).toString() == "graph_derived_socket" &&
            item->data(PelicanStudio::NodeGraphNameRole).toString() == "present" &&
            item->data(PelicanStudio::GraphCanvasPortRole).toString() == "old_image")
            to = item;
    REQUIRE(to != nullptr);
    from->moveBy(-100000, -80000);
    to->moveBy(100000, 80000);
    const QPointF scene_from = from->sceneBoundingRect().center();
    const QPointF scene_to = to->sceneBoundingRect().center();
    scene.setSceneRect(QRectF{scene_from, scene_to}.normalized().adjusted(-2000, -2000, 2000, 2000));
    view->setSceneRect(scene.sceneRect());
    view->fitInView(scene.itemsBoundingRect(), Qt::KeepAspectRatio);
    const auto hits = [&](QGraphicsItem *item) {
        const auto point = view->mapFromScene(item->sceneBoundingRect().center());
        return view->viewport()->rect().contains(point) &&
               item->contains(item->mapFromScene(view->mapToScene(point)));
    };
    // Even framing only the endpoints leaves a socket below one pixel.
    view->fitInView(QRectF{scene_from, scene_to}.normalized().adjusted(-40, -40, 40, 40),
                    Qt::KeepAspectRatio);
    REQUIRE_FALSE((hits(from) && hits(to)));
    qreal expected_scale = 1;
    SECTION("after fitting the whole graph") {
        REQUIRE(view->transform().m11() < 0.01);
    }
    SECTION("preserves a larger starting zoom") {
        expected_scale = 2;
        view->setTransform(QTransform::fromScale(2, 2));
        view->centerOn(from);
        REQUIRE(hits(from));
        REQUIRE_FALSE(hits(to));
    }
    // Observe actual delivered events, without replacing the canvas handlers:
    // every held move must stay on screen as the connection crosses the scene.
    struct PathProbe final : QObject {
        PelicanStudio::NodeCanvasView *view = nullptr;
        GraphCanvasScene *scene = nullptr;
        std::vector<QPointF> positions;
        std::vector<QPointF> centres;
        int presses = 0;
        int releases = 0;
        bool all_visible = true;
        bool connection_held = true;
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::MouseButtonPress) ++presses;
            if (event->type() == QEvent::MouseButtonRelease) ++releases;
            if (event->type() == QEvent::MouseMove) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                const QPoint point = mouse->position().toPoint();
                all_visible &= view->viewport()->rect().contains(point);
                connection_held &= scene->dragging() && (mouse->buttons() & Qt::LeftButton);
                positions.push_back(view->mapToScene(point));
                centres.push_back(view->mapToScene(view->viewport()->rect().center()));
            }
            return false;
        }
    } probe;
    probe.view = view;
    probe.scene = &scene;
    view->viewport()->installEventFilter(&probe);
    AutomationHost host;
    host.window = &window;
    constexpr int steps = 8;
    const auto reply = runAutomationCommand(host, request("drag", {
        {"from", "pelican.graphCanvasView::graph_socket#shade/out:color"},
        {"to", "pelican.graphCanvasView::graph_derived_socket#present/old_image"},
        {"steps", steps}}));
    INFO(describe(reply).toStdString());
    REQUIRE(reply.ok);
    CHECK(reply.result.toObject().value("path").toString() == "panned");
    CHECK(reply.result.toObject().value("scale").toDouble() == expected_scale);
    CHECK(view->transform().m11() == expected_scale);
    REQUIRE(sent.size() == 1);
    CHECK(sent.front().method == "edit_preset_pass");
    CHECK(sent.front().params.at("pass_name") == "present");
    CHECK(sent.front().params.at("input") == Json::array({"shade__color"}));
    CHECK_FALSE(scene.dragging());
    CHECK(probe.presses == 1);
    CHECK(probe.releases == 1);
    CHECK(probe.all_visible);
    CHECK(probe.connection_held);
    REQUIRE(probe.positions.size() == steps + 1);
    for (int step = 1; step <= steps; ++step) {
        const QPointF expected = scene_from + (scene_to - scene_from) * (qreal(step) / steps);
        CHECK(QLineF{probe.positions[step - 1], expected}.length() < 2);
        CHECK(QLineF{probe.centres[step - 1], expected}.length() < 2);
    }
    CHECK(QLineF{probe.positions.back(), scene_to}.length() < 2);
}

TEST_CASE("FX-8 an unpressable named endpoint refuses before a panned drag",
          "[devstudio][automation][fx8]") {
    Fixture fixture;
    QGraphicsScene scene;
    fixture.canvas->setScene(&scene);
    scene.setSceneRect(-200000, -200000, 400000, 400000);
    for (auto [name, position] : {std::pair{QStringLiteral("from"), QPointF{-100000, -80000}},
                                  std::pair{QStringLiteral("to"), QPointF{100000, 80000}}}) {
        auto *item = scene.addRect(-5, -5, 10, 10);
        item->setPos(position);
        item->setData(PelicanStudio::NodeGraphItemKindRole, QStringLiteral("fx8_socket"));
        item->setData(PelicanStudio::NodeGraphNameRole, name);
        if (name == "to")
            item->setData(PelicanStudio::NodeGraphHotspotRole, QPointF{30, 30});
    }
    const auto reply = fixture.run(QStringLiteral("drag"),
        {{"from", "pelican.testCanvas::fx8_socket#from"},
         {"to", "pelican.testCanvas::fx8_socket#to"}});
    REQUIRE_FALSE(reply.ok);
    CHECK(reply.message.contains("cannot target named drag endpoints"));
    CHECK(reply.message.contains("pelican.testCanvas::fx8_socket#from"));
    CHECK(reply.message.contains("pelican.testCanvas::fx8_socket#to"));
    CHECK(fixture.canvas->mouse.isEmpty());
}

namespace {

// Production MainWindow, InspectorModel and RPC transport. The small engine
// keeps an object document and undo/redo snapshots; no GPU or project launch.
struct FixtureChildExit {
    PelicanStudio::EngineProcess &process;
    void finish() {
        if (!process.isRunning()) return;
        process.process_.closeWriteChannel();
        process.waitForFinished(5000);
    }
    ~FixtureChildExit() { finish(); }
};

struct SceneShortcutRig {
    using Json = nlohmann::json;
    QApplication &app = application();
    QTemporaryDir config;
    PelicanStudio::MainWindow window{config.path()};
    PelicanStudio::EmbeddedViewport &viewport = *window.engineViewport();
    Json objects = Json::array({{{"name", "Cube"}, {"id", 41}, {"x", 1.0}}});
    std::vector<Json> undo, redo;
    std::deque<Json> requests;
    QString buffer;
    std::uint64_t revision = 1;
    int undo_calls = 0, redo_calls = 0;
    // Also closes stdin if a REQUIRE aborts construction.
    FixtureChildExit exit_child{viewport.process_};

    SceneShortcutRig() {
        window.resize(1280, 800);
        window.show();
        window.activateWindow();
        REQUIRE(QTest::qWaitForWindowActive(&window));
        window.project_model_.emplace();
        window.project_model_->updateSceneOutline(outline());
        window.selection_model_.bindProject(&*window.project_model_);
        QObject::connect(&viewport, &PelicanStudio::EmbeddedViewport::engineStandardErrorReceived,
                         &window, [this](const QString &text) {
            buffer += text;
            while (buffer.contains('\n')) {
                auto end = buffer.indexOf('\n');
                auto line = buffer.left(end).trimmed();
                buffer.remove(0, end + 1);
                const QString prefix = QStringLiteral("rpc-hold-request:");
                if (line.startsWith(prefix)) requests.push_back(Json::parse(line.mid(prefix.size()).toStdString()));
            }
        });
        QString error;
        REQUIRE(viewport.process_.start({QString::fromUtf8(PELICAN_PROCESS_FIXTURE_PATH),
                                         {QStringLiteral("rpc-hold")}, {}}, &error));
        REQUIRE(viewport.process_.waitForStarted(5000));
        viewport.rpc_ready_ = true;
        emit viewport.engineRpcBecameAvailable();
        window.selection_model_.selectFromOutliner(PelicanStudio::OutlinerObjectKey{"main", 0, 41});
        window.inspector_->setSelection(PelicanStudio::OutlinerObjectKey{"main", 0, 41}, "Cube");
        focus("pelican.outliner");
        focus("pelican.viewportHost");
        await([this] { return window.inspector_->undoAction()->isEnabled(); });
    }
    ~SceneShortcutRig() {
        // EOF ends this fixture normally; no process termination is needed.
        exit_child.finish();
        window.project_model_.reset();
    }
    Json outline() const {
        Json rows = Json::array();
        for (const auto &object : objects)
            rows.push_back({{"name", object.at("name")}, {"authoring_object_id", object.at("id")},
                            {"declaration_index", rows.size()}, {"parent_authoring_object_id", nullptr}});
        return {{"scene_id", "main"}, {"scene_revision", revision}, {"objects", rows}};
    }
    Json answer(const std::string &method, const Json &params) {
        if (method == "open_editor_session") return {{"actor_id", 9}};
        if (method == "scene_outline" || method == "scene_tree") return outline();
        if (method == "get_scene_revision") return {{"scene_revision", revision}, {"preview_epoch", 0}};
        if (method == "get_components") {
            const auto id = params.value("authoring_object_id", 41);
            for (const auto &object : objects) if (object.at("id") == id)
                return {{"scene_revision", revision}, {"authoring_object_id", id}, {"declaration_index", 0},
                        {"name", object.at("name")}, {"components", Json::array({{
                            {"name", "transform"}, {"component_index", 0},
                            {"authored_json", {{"pos", Json::array({object.at("x"), 2.0, 3.0})}}},
                            {"editable", true}, {"pending", false},
                            {"codec", {{"state", "registered"}}},
                            {"schema", {{"state", "available"}, {"fields", Json::array({{{"name", "pos"}, {"type", "vec3"}}})}}}
                        }})}};
            return Json{};
        }
        if (method == "edit" || method == "undo" || method == "redo") {
            CHECK(params.at("actor_id") == 9);
            CHECK(params.at("base_revision") == revision);
            if (method == "undo") {
                ++undo_calls;
                REQUIRE_FALSE(undo.empty());
                redo.push_back(objects); objects = undo.back(); undo.pop_back();
            } else if (method == "redo") {
                ++redo_calls;
                REQUIRE_FALSE(redo.empty());
                undo.push_back(objects); objects = redo.back(); redo.pop_back();
            } else {
                undo.push_back(objects); redo.clear();
                for (const auto &op : params.at("operations")) {
                    if (op.at("op") == "destroy") objects.clear();
                    else if (op.at("op") == "duplicate") {
                        auto copy = objects.front(); copy["id"] = 42; copy["name"] = "Cube_copy"; objects.push_back(copy);
                    } else if (op.at("op") == "set_component_value") {
                        CHECK(op.at("object_id") == 41);
                        CHECK(op.at("component_slot") == "transform");
                        CHECK(op.at("field_path") == "/pos");
                        objects.front()["x"] = op.at("value").at(0);
                    }
                    else FAIL("unexpected scene operation: " << op.dump());
                }
            }
            return {{"status", "committed"}, {"operation", method}, {"committed_revision", ++revision},
                    {"transaction_id", "txn-" + std::to_string(revision)}};
        }
        return Json{};
    }
    void pump() {
        QApplication::processEvents();
        while (!requests.empty()) {
            const auto request = std::move(requests.front()); requests.pop_front();
            const auto result = answer(request.at("method"), request.value("params", Json::object()));
            const auto id = request.at("id").get<qint64>();
            if (result.is_null()) emit viewport.inspectorRpcFailed(id, QStringLiteral("unsupported fixture method"));
            else emit viewport.inspectorRpcSucceeded(id, QByteArray::fromStdString(result.dump()));
        }
        QTest::qWait(5);
    }
    void await(const std::function<bool()> &condition) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < 5000) pump();
        REQUIRE(condition());
    }
    QWidget *focus(const char *name) {
        auto *widget = window.findChild<QWidget *>(QString::fromLatin1(name));
        REQUIRE(widget);
        for (auto *parent = widget->parentWidget(); parent; parent = parent->parentWidget())
            if (auto *dock = qobject_cast<QDockWidget *>(parent)) { dock->show(); dock->raise(); }
        widget->setFocusPolicy(Qt::StrongFocus);
        widget->setFocus();
        QApplication::processEvents();
        REQUIRE(widget->hasFocus());
        return widget;
    }
    AutomationReply command(const QString &name, const QJsonObject &fields = {}) {
        AutomationHost host; host.window = &window;
        return runAutomationCommand(host, request(name, fields));
    }
    void key(const char *name, const char *sequence) {
        focus(name);
        const auto reply = command("key", {{"name", name}, {"sequence", sequence}, {"route", "window"}});
        INFO(describe(reply).toStdString()); REQUIRE(reply.ok);
    }
    void move() {
        QString error;
        REQUIRE(window.inspector_->submitSceneEdit(PelicanStudio::SceneEditKind::Reparent, 41,
            {{"op", "set_component_value"}, {"object_id", 41},
             {"component_slot", "transform"}, {"field_path", "/pos"}, {"value", Json::array({7.0, 2.0, 3.0})}},
            "Move Cube", &error) == PelicanStudio::SpawnOutcome::Sent);
        await([this] { return objects.front().at("x") == 7.0 && !window.inspector_->sceneEditPending(); });
        focus("pelican.viewportHost");
        await([this] { return window.inspector_->undoAction()->isEnabled(); });
    }
};

} // namespace

TEST_CASE("KB-1 T1 T2 scene shortcut routes by focus and labels the Edit menu", "[devstudio][automation_input][shortcut][kb1]") {
    SceneShortcutRig rig;
    for (const char *surface : {"pelican.viewportHost", "pelican.outliner", "pelican.problems.list", "pelican.inspectorUndo"}) {
        INFO(surface);
        rig.focus(surface);
        rig.move();
        auto *action = rig.window.inspector_->undoAction();
        REQUIRE(action->text() == "Undo Move Cube");
        auto *menu = rig.window.findChild<QMenu *>("pelican.editMenu");
        REQUIRE(menu);
        REQUIRE(menu->actions().front() == action);
        menu->popup(rig.window.mapToGlobal(QPoint{20, 40}));
        QApplication::processEvents();
        CHECK(action->isEnabled());
        menu->hide();
        QApplication::processEvents();
        const int before = rig.undo_calls;
        rig.key(surface, "Ctrl+Z");
        rig.await([&] { return rig.undo_calls == before + 1 && !rig.window.inspector_->sceneEditPending(); });
        CHECK(rig.objects.front().at("x") == 1.0);
        CHECK(rig.window.inspector_->redoAction()->text() == "Redo Move Cube");
        rig.await([&] { return rig.window.inspector_->redoAction()->isEnabled(); });
        rig.key(surface, std::string{surface} == "pelican.viewportHost" ? "Ctrl+Y" : "Ctrl+Shift+Z");
        rig.await([&] { return rig.objects.front().at("x") == 7.0 && !rig.window.inspector_->sceneEditPending(); });
        rig.await([&] { return rig.window.inspector_->undoAction()->isEnabled(); });
        rig.key(surface, "Ctrl+Z");
        rig.await([&] { return rig.objects.front().at("x") == 1.0 && !rig.window.inspector_->sceneEditPending(); });
        rig.await([&] { return rig.window.inspector_->undoAction()->isEnabled(); });
    }
    for (const char *tab : {"frame_plan", "graph"}) {
        INFO(tab);
        REQUIRE(rig.window.showRenderToolsTab(QString::fromLatin1(tab)));
        rig.window.render_tabs_->currentWidget()->setFocusPolicy(Qt::StrongFocus);
        rig.window.render_tabs_->currentWidget()->setFocus();
        QApplication::processEvents();
        CHECK_FALSE(rig.window.inspector_->undoAction()->isEnabled());
        CHECK_FALSE(rig.window.inspector_->redoAction()->isEnabled());
        CHECK_FALSE(rig.window.outliner_->deleteAction()->isEnabled());
    }
}

TEST_CASE("KB-1 T3 viewport and Problems share object edits while text keeps Delete", "[devstudio][automation_input][shortcut][kb1]") {
    SceneShortcutRig rig;
    rig.key("pelican.viewportHost", "Delete");
    rig.await([&] { return rig.objects.empty() && !rig.window.inspector_->sceneEditPending(); });
    rig.await([&] { return rig.window.inspector_->undoAction()->isEnabled(); });
    rig.key("pelican.viewportHost", "Ctrl+Z");
    rig.await([&] { return rig.objects.size() == 1 && !rig.window.inspector_->sceneEditPending(); });
    // As after the Outliner's refresh, select the restored identity.
    rig.window.project_model_->updateSceneOutline(rig.outline());
    rig.window.selection_model_.selectFromOutliner(PelicanStudio::OutlinerObjectKey{"main", 0, 41});
    rig.window.inspector_->setSelection(PelicanStudio::OutlinerObjectKey{"main", 0, 41}, "Cube");
    // The fixture selected directly; enter the surface as a real tree selection does.
    rig.focus("pelican.outliner");
    rig.await([&] { return rig.window.inspector_->undoAction()->isEnabled(); });
    REQUIRE(rig.window.outliner_->duplicateAction()->isEnabled());
    rig.key("pelican.viewportHost", "Ctrl+D");
    rig.await([&] { return rig.objects.size() == 2 && !rig.window.inspector_->sceneEditPending(); });
    rig.await([&] { return rig.window.inspector_->undoAction()->isEnabled(); });
    rig.key("pelican.viewportHost", "Ctrl+Z");
    rig.await([&] { return rig.objects.size() == 1 && !rig.window.inspector_->sceneEditPending(); });
    rig.await([&] { return rig.window.inspector_->undoAction()->isEnabled(); });
    rig.key("pelican.problems.list", "Ctrl+D");
    rig.await([&] { return rig.objects.size() == 2 && !rig.window.inspector_->sceneEditPending(); });
    auto *line = new QLineEdit(rig.window.inspector_);
    line->setObjectName("kb1.text"); line->setText("abc"); line->show(); line->setCursorPosition(0);
    rig.key("kb1.text", "Delete");
    CHECK(line->text() == "bc");
    CHECK(rig.objects.size() == 2);
    line->selectAll(); QTest::keyClicks(line, "replacement");
    rig.key("kb1.text", "Ctrl+Z");
    CHECK(line->text() == "bc");
    CHECK(rig.undo_calls == 2);
}

TEST_CASE("KB-1 T4 New and Open project window shortcuts open dismissible dialogs", "[devstudio][automation_input][shortcut][kb1]") {
    application();
    QTemporaryDir config;
    PelicanStudio::MainWindow window{config.path()};
    window.show(); window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    AutomationHost host; host.window = &window;
    auto key = [&](const char *sequence) {
        const auto reply = runAutomationCommand(host, request("key", {{"name", "pelican.viewportHost"}, {"sequence", sequence}, {"route", "window"}}));
        REQUIRE(reply.ok);
    };
    key("Ctrl+N"); QApplication::processEvents();
    auto *dialog = window.findChild<QDialog *>("pelican.newProjectDialog");
    REQUIRE(dialog); REQUIRE(dialog->isVisible());
    auto reply = runAutomationCommand(host, request("key", {{"name", "pelican.newProjectDialog"}, {"sequence", "Escape"}}));
    REQUIRE(reply.ok);
    QApplication::processEvents(); window.activateWindow();
    bool opened = false;
    // QFileDialog's exec loop is closed by automation on its next event turn.
    QTimer::singleShot(100, &window, [&] {
        auto *file = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
        opened = file != nullptr;
        if (file) { file->setObjectName("kb1.openDialog");
            auto closed = runAutomationCommand(host, request("key", {{"name", "kb1.openDialog"}, {"sequence", "Escape"}}));
            CHECK(closed.ok);
        }
    });
    key("Ctrl+O");
    CHECK(opened);
}

TEST_CASE("KB-1 T5 Help opens a read-only table of keyboard shortcuts", "[devstudio][automation_input][shortcut][kb1]") {
    application();
    QTemporaryDir config;
    PelicanStudio::MainWindow window{config.path()};
    window.show(); window.activateWindow();
    AutomationHost host; host.window = &window;
    const auto reply = runAutomationCommand(host, request("action", {{"name", "pelican.help.shortcuts"}}));
    REQUIRE(reply.ok);
    auto *dialog = window.findChild<QDialog *>("pelican.help.shortcutsDialog");
    REQUIRE(dialog); REQUIRE(dialog->isVisible());
    auto *table = dialog->findChild<QTableWidget *>("pelican.help.shortcutsTable");
    REQUIRE(table); CHECK(table->editTriggers() == QAbstractItemView::NoEditTriggers);
    for (const auto *key : {"Ctrl+Z", "Ctrl+S", "Ctrl+P", "Delete", "Ctrl+D", "F", "Escape"}) {
        bool found = false;
        for (int row = 0; row < table->rowCount(); ++row) found |= table->item(row, 1)->text().contains(key);
        INFO(key); CHECK(found);
    }
    dialog->reject();
}

TEST_CASE("KB-1 native viewport keys use the scene actions and respect modal dialogs", "[devstudio][automation_input][shortcut][kb1]") {
    SceneShortcutRig rig;
    rig.move();
    emit rig.viewport.studioShortcutPressed(Qt::Key_Z, Qt::ControlModifier);
    CHECK(rig.undo_calls == 0);
    rig.viewport.engine_input_active_ = true;
    emit rig.viewport.engineInputActiveChanged(true);
    emit rig.viewport.studioShortcutPressed(Qt::Key_Z, Qt::ControlModifier);
    rig.await([&] { return rig.undo_calls == 1 && !rig.window.inspector_->sceneEditPending(); });
    CHECK(rig.objects.front().at("x") == 1.0);
    rig.await([&] { return rig.window.inspector_->redoAction()->isEnabled(); });
    QDialog dialog(&rig.window); dialog.open(); QApplication::processEvents();
    emit rig.viewport.studioShortcutPressed(Qt::Key_Y, Qt::ControlModifier);
    CHECK(rig.redo_calls == 0);
    dialog.reject(); QApplication::processEvents();
    emit rig.viewport.studioShortcutPressed(Qt::Key_Y, Qt::ControlModifier);
    rig.await([&] { return rig.redo_calls == 1; });
    CHECK(rig.objects.front().at("x") == 7.0);
    rig.viewport.engine_input_active_ = false;
}
