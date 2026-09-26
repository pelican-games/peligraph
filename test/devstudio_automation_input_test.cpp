// Q-1b: the input kinds Studio's automation surface gained -- a wheel, a bare
// move, a held button, a menu action fired by name, a listing of what the menu
// bar holds, and a reading of where a canvas is looking.
//
// Nothing here talks to a socket or to an engine. Each case builds the widget
// that would receive the gesture, sends the command through the same
// `runAutomationCommand` the pipe calls, and asks the widget what it received.
// The offscreen platform is enough for that: these are Qt events, not pixels.

#include "automationcommands.hpp"
#include "automationserver.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QApplication>
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
#include <QComboBox>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWheelEvent>
#include <QWidget>

#include <string>
#include <vector>

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

    // A row this tree does not hold is refused by code, with the name in it.
    const AutomationReply missing = fixture.run(
        QStringLiteral("select"),
        QJsonObject{{QStringLiteral("name"), QStringLiteral("pelican.testOutliner")},
                    {QStringLiteral("text"), QStringLiteral("Hero")}});
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
                    {QStringLiteral("text"), QStringLiteral("nowhere")}});
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
