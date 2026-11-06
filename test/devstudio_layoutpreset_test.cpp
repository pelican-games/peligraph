#include "layoutpreset.hpp"
#include "docktitlebar.hpp"
#include "embeddedviewport.hpp"
#include "frameplanwidget.hpp"
#include "graphcanvaswidget.hpp"
#include "mainwindow.hpp"
#include "inspectorwidget.hpp"
#include "studiotheme.hpp"
#include "toollayoutpreset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPoint>
#include <QLayout>
#include <QLabel>
#include <QIcon>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSet>
#include <QRect>
#include <QSettings>
#include <QShortcut>
#include <QSignalSpy>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

#include <cstdlib>

namespace {

using PelicanStudio::LayoutPresetManager;
using PelicanStudio::LayoutRestoreResult;
using PelicanStudio::LayoutSnapshot;
using PelicanStudio::ToolLayoutPresetManager;
using PelicanStudio::ToolLayoutRestoreResult;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_layout_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

QString onlyPresetPath(const QTemporaryDir &directory) {
    const QStringList files = QDir(directory.path()).entryList(
        {QStringLiteral("*.layout.json")}, QDir::Files);
    REQUIRE(files.size() == 1);
    return QDir(directory.path()).filePath(files.front());
}

QJsonObject readPreset(const QString &path) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    REQUIRE(document.isObject());
    return document.object();
}

void writePreset(const QString &path, const QJsonObject &root) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    REQUIRE(file.write(contents) == contents.size());
}

QDockWidget *addDock(
    QMainWindow &window, const QString &object_name, Qt::DockWidgetArea area) {
    auto *dock = new QDockWidget(&window);
    dock->setObjectName(object_name);
    dock->setWidget(new QWidget(dock));
    window.addDockWidget(area, dock);
    return dock;
}

// UI-2: the dock a panel is shown in.
QDockWidget *dockHolding(QWidget *widget) {
    for (QWidget *at = widget; at != nullptr; at = at->parentWidget())
        if (auto *dock = qobject_cast<QDockWidget *>(at)) return dock;
    return nullptr;
}

// OSC-3c: which tab of the Output stack is current, by its title. The title
// is what Qt's dock tab bar shows; comparing it before and after a restore
// says the saved selection was kept.
QString currentOutputTab(QMainWindow &window) {
    for (QTabBar *bar : window.findChildren<QTabBar *>()) {
        bool has_output = false;
        for (int tab = 0; tab < bar->count(); ++tab) {
            if (bar->tabText(tab) == QStringLiteral("Output")) {
                has_output = true;
                break;
            }
        }
        if (has_output && bar->currentIndex() >= 0)
            return bar->tabText(bar->currentIndex());
    }
    return {};
}

} // namespace

TEST_CASE("Devstudio layout presets round-trip named versioned state", "[devstudio][layout]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    LayoutPresetManager manager(directory.path());
    const LayoutSnapshot expected{QByteArrayLiteral("window geometry"), QByteArrayLiteral("dock state")};

    QString error;
    REQUIRE(manager.savePreset(QStringLiteral("Editing"), expected, &error));
    REQUIRE(error.isEmpty());
    REQUIRE(manager.presetNames() == QStringList{QStringLiteral("Editing")});

    const QJsonObject root = readPreset(onlyPresetPath(directory));
    REQUIRE(root.value(QStringLiteral("format")).toString() == QStringLiteral("pelican.devstudio.layout"));
    REQUIRE(root.value(QStringLiteral("version")).toInt() == LayoutPresetManager::FileVersion);
    REQUIRE(root.value(QStringLiteral("main_window_state_version")).toInt() ==
            LayoutPresetManager::WindowStateVersion);
    REQUIRE(root.value(QStringLiteral("name")).toString() == QStringLiteral("Editing"));

    bool default_applied = false;
    LayoutSnapshot restored;
    const LayoutRestoreResult result = manager.restorePreset(
        QStringLiteral("Editing"),
        [&restored](const LayoutSnapshot &snapshot) {
            restored = snapshot;
            return true;
        },
        [&default_applied]() { default_applied = true; }, &error);

    REQUIRE(result == LayoutRestoreResult::Restored);
    REQUIRE(restored == expected);
    REQUIRE_FALSE(default_applied);
    REQUIRE(error.isEmpty());
}

TEST_CASE("Saved devstudio layouts remain restorable as later docks are added",
           "[devstudio][layout][compatibility]") {
    (void)application();

    QMainWindow six_dock_window;
    QDockWidget *legacy_project = addDock(
        six_dock_window, QStringLiteral("pelican.projectDock"),
        Qt::LeftDockWidgetArea);
    QDockWidget *legacy_outliner = addDock(
        six_dock_window, QStringLiteral("pelican.outlinerDock"),
        Qt::LeftDockWidgetArea);
    QDockWidget *legacy_inspector = addDock(
        six_dock_window, QStringLiteral("pelican.inspectorDock"),
        Qt::RightDockWidgetArea);
    legacy_project->setWindowTitle(QStringLiteral("Project"));
    legacy_outliner->setWindowTitle(QStringLiteral("Outliner"));
    legacy_inspector->setWindowTitle(QStringLiteral("Inspector"));
    QDockWidget *legacy_output = addDock(
        six_dock_window, QStringLiteral("pelican.outputDock"),
        Qt::BottomDockWidgetArea);
    QDockWidget *legacy_engine_log = addDock(
        six_dock_window, QStringLiteral("pelican.engineLogDock"),
        Qt::BottomDockWidgetArea);
    QDockWidget *legacy_frame_plan = addDock(
        six_dock_window, QStringLiteral("pelican.framePlanDock"),
        Qt::BottomDockWidgetArea);
    legacy_output->setWindowTitle(QStringLiteral("Output"));
    legacy_engine_log->setWindowTitle(QStringLiteral("Engine Log"));
    legacy_frame_plan->setWindowTitle(QStringLiteral("Frame Plan"));
    six_dock_window.tabifyDockWidget(legacy_project, legacy_outliner);
    six_dock_window.tabifyDockWidget(legacy_output, legacy_engine_log);
    six_dock_window.tabifyDockWidget(legacy_engine_log,
                                    legacy_frame_plan);
    legacy_inspector->hide();
    six_dock_window.show();
    application().processEvents();
    legacy_frame_plan->raise();
    application().processEvents();
    const QString legacy_tab = currentOutputTab(six_dock_window);
    REQUIRE_FALSE(legacy_tab.isEmpty());

    const QByteArray six_dock_state =
        six_dock_window.saveState(
            LayoutPresetManager::WindowStateVersion);
    REQUIRE_FALSE(six_dock_state.isEmpty());

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    LayoutPresetManager manager(directory.path());
    REQUIRE(manager.savePreset(
        QStringLiteral("Six docks"),
        {QByteArrayLiteral("geometry"), six_dock_state}));

    QTemporaryDir application_config;
    REQUIRE(application_config.isValid());
    PelicanStudio::MainWindow current_window{application_config.path()};
    const auto dock = [&current_window](const char *name) {
        auto *result = current_window.findChild<QDockWidget *>(
            QString::fromLatin1(name));
        REQUIRE(result != nullptr);
        return result;
    };
    QDockWidget *project = dock("pelican.projectDock");
    QDockWidget *outliner = dock("pelican.outlinerDock");
    QDockWidget *inspector = dock("pelican.inspectorDock");
    QDockWidget *output = dock("pelican.outputDock");
    QDockWidget *engine_log = dock("pelican.engineLogDock");
    QDockWidget *frame_plan = dock("pelican.framePlanDock");
    QDockWidget *fullscreen_pass = dock("pelican.fullscreenPassDock");
    // HW-2s: a dock no saved layout knows yet.
    QDockWidget *assets = dock("pelican.assetsDock");
    // MT-1s: and another.
    QDockWidget *materials = dock("pelican.materialsDock");
    // PB-1s: and Problems.
    QDockWidget *problems = dock("pelican.problemsDock");
    // OSC-3c: and Live Inputs, likewise added late.
    QDockWidget *live_inputs = dock("pelican.liveInputsDock");

    bool default_applied = false;
    QString error;
    LayoutRestoreResult result = manager.restorePreset(
        QStringLiteral("Six docks"),
        [&current_window](const LayoutSnapshot &snapshot) {
            return current_window.restoreState(
                snapshot.window_state, LayoutPresetManager::WindowStateVersion);
        },
        [&default_applied]() { default_applied = true; }, &error);

    REQUIRE(result == LayoutRestoreResult::Restored);
    REQUIRE_FALSE(default_applied);
    REQUIRE(error.isEmpty());
    REQUIRE(current_window.dockWidgetArea(project) ==
            Qt::LeftDockWidgetArea);
    REQUIRE(current_window.dockWidgetArea(outliner) ==
            Qt::LeftDockWidgetArea);
    REQUIRE(current_window.dockWidgetArea(inspector) ==
            Qt::RightDockWidgetArea);
    for (QDockWidget *bottom :
         {output, engine_log, frame_plan}) {
        REQUIRE(current_window.dockWidgetArea(bottom) ==
                Qt::BottomDockWidgetArea);
    }
    REQUIRE(current_window.tabifiedDockWidgets(project).contains(outliner));
    REQUIRE(current_window.tabifiedDockWidgets(output).contains(engine_log));
    REQUIRE(current_window.tabifiedDockWidgets(output).contains(frame_plan));
    REQUIRE_FALSE(project->isHidden());
    REQUIRE_FALSE(outliner->isHidden());
    REQUIRE(inspector->isHidden());
    REQUIRE_FALSE(output->isHidden());
    REQUIRE_FALSE(engine_log->isHidden());
    REQUIRE_FALSE(frame_plan->isHidden());
    // OSC-3c added its tab last; the six-dock state still selects the same
    // tab it selected before the new tab existed.
    current_window.show();
    application().processEvents();
    REQUIRE(currentOutputTab(current_window) == legacy_tab);
    CHECK(currentOutputTab(current_window) != QStringLiteral("Live Inputs"));
    CHECK(live_inputs->visibleRegion().isEmpty());
    REQUIRE(current_window.tabifiedDockWidgets(output).contains(live_inputs));
    REQUIRE(current_window.dockWidgetArea(fullscreen_pass) !=
            Qt::NoDockWidgetArea);
    REQUIRE(current_window.dockWidgetArea(assets) != Qt::NoDockWidgetArea);
    REQUIRE(current_window.dockWidgetArea(materials) != Qt::NoDockWidgetArea);
    REQUIRE(current_window.dockWidgetArea(problems) != Qt::NoDockWidgetArea);

    const auto docks = current_window.findChildren<QDockWidget *>();
    // HW-2s / MT-1s / PB-1s's three, SG-2's Layers, UI-2's Graph,
    // Features and Rendering settings, and OSC-3c's Live Inputs (fifteen).
    REQUIRE(docks.size() == 15);
    QSet<QString> object_names;
    for (const QDockWidget *current : docks) {
        REQUIRE_FALSE(current->objectName().isEmpty());
        object_names.insert(current->objectName());
    }
    REQUIRE(object_names.size() == 15);

    auto *panels = current_window.findChild<QMenu *>(
        QStringLiteral("pelican.panelsMenu"));
    REQUIRE(panels != nullptr);
    REQUIRE(panels->actions().size() == 15);
}

TEST_CASE("Devstudio restores the last panel layout at startup with an unsaved default control",
          "[devstudio][layout][workspace]") {
    QApplication &app = application();
    QTemporaryDir unsaved_config;
    QTemporaryDir saved_config;
    REQUIRE(unsaved_config.isValid());
    REQUIRE(saved_config.isValid());

    Qt::DockWidgetArea unsaved_area = Qt::NoDockWidgetArea;
    {
        PelicanStudio::MainWindow unsaved_window{unsaved_config.path()};
        auto *project = unsaved_window.findChild<QDockWidget *>(
            QStringLiteral("pelican.projectDock"));
        REQUIRE(project != nullptr);
        unsaved_area = unsaved_window.dockWidgetArea(project);
        REQUIRE(unsaved_area == Qt::LeftDockWidgetArea);
    }

    {
        PelicanStudio::MainWindow changed_window{saved_config.path()};
        changed_window.show();
        app.processEvents();
        auto *project = changed_window.findChild<QDockWidget *>(
            QStringLiteral("pelican.projectDock"));
        REQUIRE(project != nullptr);
        changed_window.addDockWidget(Qt::RightDockWidgetArea, project);
        app.processEvents();
        REQUIRE(changed_window.dockWidgetArea(project) ==
                Qt::RightDockWidgetArea);
        REQUIRE(changed_window.close());
    }

    {
        PelicanStudio::MainWindow restored_window{saved_config.path()};
        auto *project = restored_window.findChild<QDockWidget *>(
            QStringLiteral("pelican.projectDock"));
        REQUIRE(project != nullptr);
        const Qt::DockWidgetArea restored_area =
            restored_window.dockWidgetArea(project);
        REQUIRE(restored_area == Qt::RightDockWidgetArea);
        REQUIRE(restored_area != unsaved_area);
        REQUIRE(restored_window.statusBar()->currentMessage().contains(
            QStringLiteral("Last panel session")));
    }

    const QString layout_directory =
        QDir(saved_config.path()).filePath(QStringLiteral("layouts"));
    const QStringList layout_files = QDir(layout_directory).entryList(
        {QStringLiteral("*.layout.json")}, QDir::Files);
    REQUIRE(layout_files.size() == 1);
    const QString layout_path =
        QDir(layout_directory).filePath(layout_files.front());
    QJsonObject corrupt = readPreset(layout_path);
    corrupt.insert(QStringLiteral("window_state"),
                   QStringLiteral("not base64"));
    writePreset(layout_path, corrupt);

    PelicanStudio::MainWindow fallback_window{saved_config.path()};
    auto *project = fallback_window.findChild<QDockWidget *>(
        QStringLiteral("pelican.projectDock"));
    REQUIRE(project != nullptr);
    REQUIRE(fallback_window.dockWidgetArea(project) ==
            Qt::LeftDockWidgetArea);
    const auto docks = fallback_window.findChildren<QDockWidget *>();
    REQUIRE(docks.size() == 15);
    for (const QDockWidget *dock : docks) {
        REQUIRE(fallback_window.dockWidgetArea(
                    const_cast<QDockWidget *>(dock)) !=
                Qt::NoDockWidgetArea);
    }
    const QString fallback_notice =
        fallback_window.statusBar()->currentMessage();
    REQUIRE(fallback_notice.contains(QStringLiteral("Last panel session")));
    REQUIRE(fallback_notice.contains(QStringLiteral("default"),
                                     Qt::CaseInsensitive));
}

TEST_CASE("F-7 opening and closing the Pass Editor leaves the viewport its "
          "height",
          "[devstudio][layout][workspace][f7]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    auto *viewport = window.findChild<QWidget *>(
        QStringLiteral("pelican.viewportHost"));
    auto *pass_editor = window.findChild<QDockWidget *>(
        QStringLiteral("pelican.fullscreenPassDock"));
    auto *frame_plan = window.findChild<QDockWidget *>(
        QStringLiteral("pelican.framePlanDock"));
    auto *panel_entry = window.findChild<QAction *>(
        QStringLiteral("pelican.view.panels.fullscreenPassDock"));
    REQUIRE(viewport != nullptr);
    REQUIRE(pass_editor != nullptr);
    REQUIRE(frame_plan != nullptr);
    REQUIRE(panel_entry != nullptr);
    // The window is as tall as a desktop lets it be; the offscreen platform
    // would otherwise grow it to whatever the docks ask for.
    window.setMaximumSize(1280, 800);
    window.resize(1280, 800);
    // A session that starts with the Pass Editor closed and the bottom docks
    // at a working height.
    window.show();
    app.processEvents();
    REQUIRE(panel_entry->isChecked());
    panel_entry->trigger();
    app.processEvents();
    window.resizeDocks({frame_plan}, {260}, Qt::Vertical);
    app.processEvents();
    REQUIRE_FALSE(pass_editor->isVisible());
    const int before = viewport->height();
    INFO("viewport height before: " << before);
    REQUIRE(before > 200);

    // What the Frame Plan's "Edit pass" and "Add fullscreen pass..." do.
    pass_editor->show();
    pass_editor->raise();
    app.processEvents();
    REQUIRE(pass_editor->isVisible());
    const int open = viewport->height();
    INFO("viewport height with the Pass Editor in front: " << open);
    // Closed from View > Panels, the way a person (and a scenario) does.
    REQUIRE(panel_entry->isChecked());
    panel_entry->trigger();
    app.processEvents();
    REQUIRE_FALSE(pass_editor->isVisible());
    const int after = viewport->height();
    INFO("viewport height after it closed: " << after);
    CHECK(std::abs(after - before) <= 2);
    // Opening it is not a reason to shrink the viewport to a strip either.
    CHECK(open > before / 2);
}

TEST_CASE("UI-2 the Graph, the Frame Plan, Features and Rendering settings are docks of their own",
          "[devstudio][layout][ui2]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    app.processEvents();
    const auto dock = [&window](const char *name) {
        auto *result = window.findChild<QDockWidget *>(QString::fromLatin1(name));
        REQUIRE(result != nullptr);
        return result;
    };
    QDockWidget *graph = dock("pelican.graphDock");
    QDockWidget *frame_plan = dock("pelican.framePlanDock");
    QDockWidget *features = dock("pelican.renderFeaturesDock");
    QDockWidget *rendering_settings = dock("pelican.renderingSettingsDock");
    QDockWidget *output = dock("pelican.outputDock");

    // Each panel sits in its own dock; no tab widget holds them together.
    const auto panel = [&window](const char *name) {
        auto *result = window.findChild<QWidget *>(QString::fromLatin1(name));
        REQUIRE(result != nullptr);
        return result;
    };
    CHECK(dockHolding(panel("pelican.graphCanvas")) == graph);
    CHECK(dockHolding(panel("pelican.framePlan")) == frame_plan);
    CHECK(dockHolding(panel("pelican.renderFeatures")) == features);
    CHECK(dockHolding(panel("pelican.renderingSettings")) == rendering_settings);
    CHECK(window.findChild<QTabWidget *>(QStringLiteral("pelican.renderTools")) == nullptr);
    // Every dock carries the banner that says render edits stay after Stop.
    for (const char *name : {"pelican.play.renderBanner.graph", "pelican.play.renderBanner.framePlan",
                             "pelican.play.renderBanner.renderFeatures",
                             "pelican.play.renderBanner.renderingSettings"}) {
        INFO(name);
        CHECK(window.findChild<QLabel *>(QString::fromLatin1(name)) != nullptr);
    }

    // The default layout: the Graph alone on the bottom's left, on screen;
    // the others in one stack with Output on its right.
    CHECK(window.dockWidgetArea(graph) == Qt::BottomDockWidgetArea);
    CHECK(graph->isVisible());
    CHECK(window.tabifiedDockWidgets(graph).isEmpty());
    CHECK_FALSE(window.tabifiedDockWidgets(output).contains(graph));
    const auto stack = window.tabifiedDockWidgets(output);
    QDockWidget *live_inputs = dock("pelican.liveInputsDock");
    for (QDockWidget *member : {dock("pelican.engineLogDock"), dock("pelican.problemsDock"), frame_plan,
                                dock("pelican.fullscreenPassDock"), features, rendering_settings,
                                live_inputs}) {
        INFO(member->objectName().toStdString());
        CHECK(stack.contains(member));
    }
    // The new tab is last and is not selected: the Frame Plan still is.
    CHECK_FALSE(frame_plan->visibleRegion().isEmpty());
    CHECK(live_inputs->visibleRegion().isEmpty());
    // Side by side: the Graph left of the stack's front tab (a tab behind it
    // is moved out of the window, so its geometry says nothing).
    CHECK(graph->geometry().right() < frame_plan->geometry().left());

    // View > Render Tools and --tab bring the dock forward, from hidden or
    // from behind another tab of its stack.
    const std::pair<const char *, QDockWidget *> tools[] = {
        {"graph", graph},
        {"frame_plan", frame_plan},
        {"render_features", features},
        {"rendering_settings", rendering_settings},
    };
    for (const auto &[id, target] : tools) {
        INFO(id);
        auto *entry = window.findChild<QAction *>(QStringLiteral("pelican.view.renderTools.") +
                                                  QString::fromLatin1(id));
        REQUIRE(entry != nullptr);
        for (const auto &tool : tools) tool.second->hide();
        output->raise();
        app.processEvents();
        REQUIRE_FALSE(target->isVisible());
        entry->trigger();
        app.processEvents();
        CHECK(target->isVisible());
        CHECK_FALSE(target->visibleRegion().isEmpty());
        CHECK(window.rect().contains(target->geometry()));

        for (const auto &tool : tools) tool.second->hide();
        output->raise();
        app.processEvents();
        REQUIRE(window.showRenderTool(QString::fromLatin1(id)));
        app.processEvents();
        CHECK(target->isVisible());
        CHECK_FALSE(target->visibleRegion().isEmpty());
        CHECK(window.rect().contains(target->geometry()));
    }
    CHECK_FALSE(window.showRenderTool(QStringLiteral("render_tools")));
}

TEST_CASE("UI-2 a panel layout saved before the split still shows the Graph dock",
          "[devstudio][layout][compatibility][ui2]") {
    QApplication &app = application();
    // The window of the version before UI-2: eleven docks, the Graph a tab of
    // the Frame Plan dock, the bottom docks in one stack.
    QMainWindow legacy;
    const auto add = [&legacy](const char *name, Qt::DockWidgetArea area) {
        return addDock(legacy, QString::fromLatin1(name), area);
    };
    QDockWidget *project = add("pelican.projectDock", Qt::LeftDockWidgetArea);
    QDockWidget *outliner = add("pelican.outlinerDock", Qt::LeftDockWidgetArea);
    QDockWidget *assets = add("pelican.assetsDock", Qt::LeftDockWidgetArea);
    QDockWidget *materials = add("pelican.materialsDock", Qt::LeftDockWidgetArea);
    legacy.tabifyDockWidget(project, outliner);
    legacy.tabifyDockWidget(project, assets);
    legacy.tabifyDockWidget(assets, materials);
    QDockWidget *inspector = add("pelican.inspectorDock", Qt::RightDockWidgetArea);
    QDockWidget *layers = new QDockWidget(&legacy);
    layers->setObjectName(QStringLiteral("pelican.layersDock"));
    layers->setWidget(new QWidget(layers));
    legacy.splitDockWidget(inspector, layers, Qt::Vertical);
    QDockWidget *output = add("pelican.outputDock", Qt::BottomDockWidgetArea);
    QDockWidget *engine_log = add("pelican.engineLogDock", Qt::BottomDockWidgetArea);
    QDockWidget *problems = add("pelican.problemsDock", Qt::BottomDockWidgetArea);
    QDockWidget *frame_plan = add("pelican.framePlanDock", Qt::BottomDockWidgetArea);
    QDockWidget *pass_editor = add("pelican.fullscreenPassDock", Qt::BottomDockWidgetArea);
    legacy.tabifyDockWidget(output, engine_log);
    legacy.tabifyDockWidget(engine_log, problems);
    legacy.tabifyDockWidget(problems, frame_plan);
    legacy.tabifyDockWidget(frame_plan, pass_editor);
    legacy.resize(1280, 800);
    legacy.show();
    app.processEvents();
    frame_plan->raise();
    app.processEvents();
    const QByteArray state = legacy.saveState(LayoutPresetManager::WindowStateVersion);
    REQUIRE_FALSE(state.isEmpty());
    const QByteArray geometry = legacy.saveGeometry();
    legacy.hide();

    // Saved as the last session, which the next window restores at start.
    QTemporaryDir config;
    REQUIRE(config.isValid());
    LayoutPresetManager manager(QDir(config.path()).filePath(QStringLiteral("layouts")));
    REQUIRE(manager.savePreset(QStringLiteral("Last panel session"), {geometry, state}));

    PelicanStudio::MainWindow window{config.path()};
    REQUIRE(window.statusBar()->currentMessage().contains(QStringLiteral("restored")));
    REQUIRE_FALSE(window.statusBar()->currentMessage().contains(QStringLiteral("default")));
    window.show();
    app.processEvents();
    const auto dock = [&window](const char *name) {
        auto *result = window.findChild<QDockWidget *>(QString::fromLatin1(name));
        REQUIRE(result != nullptr);
        return result;
    };
    // What the state knew came back: the Frame Plan in the bottom stack.
    CHECK(window.tabifiedDockWidgets(dock("pelican.outputDock")).contains(dock("pelican.framePlanDock")));
    // The Graph, which it did not know, is on screen in the window.
    QDockWidget *graph = dock("pelican.graphDock");
    QDockWidget *live_inputs = dock("pelican.liveInputsDock");
    // The saved state's selected tab, the Frame Plan, is still selected after
    // Studio adds the Live Inputs tab last.
    CHECK_FALSE(dock("pelican.framePlanDock")->visibleRegion().isEmpty());
    CHECK(live_inputs->visibleRegion().isEmpty());
    CHECK(window.dockWidgetArea(graph) != Qt::NoDockWidgetArea);
    CHECK_FALSE(graph->isFloating());
    CHECK(graph->isVisible());
    CHECK_FALSE(graph->visibleRegion().isEmpty());
    CHECK(window.rect().contains(graph->geometry()));
    // And the other two new docks have a place to be shown in.
    for (const char *name : {"pelican.renderFeaturesDock", "pelican.renderingSettingsDock"}) {
        INFO(name);
        QDockWidget *added = dock(name);
        CHECK(window.dockWidgetArea(added) != Qt::NoDockWidgetArea);
        CHECK_FALSE(added->isFloating());
        CHECK_FALSE(added->isHidden());
    }
}

TEST_CASE("Frame Plan tool layouts restore movable tabs and splitter state independently",
          "[devstudio][layout][workspace]") {
    QApplication &app = application();
    QTemporaryDir application_config;
    QTemporaryDir project_directory;
    REQUIRE(application_config.isValid());
    REQUIRE(project_directory.isValid());
    const QStringList project_contents_before =
        QDir(project_directory.path()).entryList(QDir::AllEntries |
                                                 QDir::NoDotAndDotDot);

    PelicanStudio::MainWindow source_window{application_config.path()};
    source_window.show();
    app.processEvents();
    auto *source_frame_plan_widget = source_window.findChild<QWidget *>(
        QStringLiteral("pelican.framePlan"));
    auto *source_tabs = source_window.findChild<QTabWidget *>(
        QStringLiteral("pelican.framePlanTabs"));
    auto *source_splitter = source_window.findChild<QSplitter *>(
        QStringLiteral("pelican.framePlanLogicalSplitter"));
    REQUIRE(source_frame_plan_widget != nullptr);
    auto *source_frame_plan =
        static_cast<PelicanStudio::FramePlanWidget *>(
            source_frame_plan_widget);
    REQUIRE(source_tabs != nullptr);
    REQUIRE(source_splitter != nullptr);
    REQUIRE(source_tabs->isMovable());

    const PelicanStudio::ToolLayoutSnapshot default_snapshot =
        source_frame_plan->toolLayoutSnapshot();
    source_tabs->tabBar()->moveTab(0, source_tabs->count() - 1);
    source_splitter->setSizes({840, 120});
    app.processEvents();
    const PelicanStudio::ToolLayoutSnapshot changed_snapshot =
        source_frame_plan->toolLayoutSnapshot();
    REQUIRE(changed_snapshot.tab_order != default_snapshot.tab_order);
    REQUIRE(changed_snapshot.logical_splitter_state !=
            default_snapshot.logical_splitter_state);

    ToolLayoutPresetManager tool_layouts{
        application_config.path(), project_directory.path()};
    LayoutPresetManager panel_layouts{
        QDir(application_config.path()).filePath(QStringLiteral("layouts"))};
    const QString shared_name = QStringLiteral("Editing");
    REQUIRE(tool_layouts.savePreset(shared_name, changed_snapshot));
    REQUIRE(panel_layouts.savePreset(
        shared_name,
        {source_window.saveGeometry(),
         source_window.saveState(LayoutPresetManager::WindowStateVersion)}));
    REQUIRE(tool_layouts.presetNames() == QStringList{shared_name});
    REQUIRE(panel_layouts.presetNames() == QStringList{shared_name});
    REQUIRE(QDir(project_directory.path()).entryList(
                QDir::AllEntries | QDir::NoDotAndDotDot) ==
            project_contents_before);
    REQUIRE(QDir::cleanPath(tool_layouts.storageDirectory()) !=
            QDir::cleanPath(project_directory.path()));

    QTemporaryDir fresh_window_config;
    REQUIRE(fresh_window_config.isValid());
    PelicanStudio::MainWindow fresh_window{fresh_window_config.path()};
    fresh_window.show();
    app.processEvents();
    auto *fresh_frame_plan_widget = fresh_window.findChild<QWidget *>(
        QStringLiteral("pelican.framePlan"));
    REQUIRE(fresh_frame_plan_widget != nullptr);
    auto *fresh_frame_plan =
        static_cast<PelicanStudio::FramePlanWidget *>(
            fresh_frame_plan_widget);
    const PelicanStudio::ToolLayoutSnapshot unsaved_snapshot =
        fresh_frame_plan->toolLayoutSnapshot();
    REQUIRE(unsaved_snapshot.tab_order == default_snapshot.tab_order);
    REQUIRE(unsaved_snapshot.tab_order != changed_snapshot.tab_order);

    bool default_applied = false;
    QString error;
    const ToolLayoutRestoreResult result = tool_layouts.restorePreset(
        shared_name,
        [fresh_frame_plan](const PelicanStudio::ToolLayoutSnapshot &snapshot) {
            return fresh_frame_plan->restoreToolLayout(snapshot);
        },
        [fresh_frame_plan, &default_applied]() {
            default_applied = true;
            fresh_frame_plan->applyDefaultToolLayout();
        },
        &error);
    REQUIRE(result == ToolLayoutRestoreResult::Restored);
    REQUIRE_FALSE(default_applied);
    REQUIRE(error.isEmpty());
    REQUIRE(fresh_frame_plan->toolLayoutSnapshot() == changed_snapshot);
}

TEST_CASE("Devstudio layout version mismatch selects the default without applying saved state",
          "[devstudio][layout]") {
    const QStringList version_fields{
        QStringLiteral("version"),
        QStringLiteral("main_window_state_version"),
    };
    for (const QString &version_field : version_fields) {
        QTemporaryDir directory;
        REQUIRE(directory.isValid());
        LayoutPresetManager manager(directory.path());
        REQUIRE(manager.savePreset(
            QStringLiteral("Future"),
            {QByteArrayLiteral("window geometry"), QByteArrayLiteral("dock state")}));

        const QString path = onlyPresetPath(directory);
        QJsonObject root = readPreset(path);
        root.insert(version_field, root.value(version_field).toInt() + 1);
        writePreset(path, root);

        bool saved_state_applied = false;
        bool default_applied = false;
        QString error;
        const LayoutRestoreResult result = manager.restorePreset(
            QStringLiteral("Future"),
            [&saved_state_applied](const LayoutSnapshot &) {
                saved_state_applied = true;
                return true;
            },
            [&default_applied]() { default_applied = true; }, &error);

        REQUIRE(result == LayoutRestoreResult::DefaultUnsupportedVersion);
        REQUIRE_FALSE(saved_state_applied);
        REQUIRE(default_applied);
        REQUIRE_FALSE(error.isEmpty());
        REQUIRE(manager.presetNames().isEmpty());
    }
}

TEST_CASE("Devstudio layout restore rejection and invalid data fall back safely", "[devstudio][layout]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    LayoutPresetManager manager(directory.path());
    REQUIRE(manager.savePreset(
        QStringLiteral("Rejected"),
        {QByteArrayLiteral("window geometry"), QByteArrayLiteral("dock state")}));

    bool default_applied = false;
    QString error;
    LayoutRestoreResult result = manager.restorePreset(
        QStringLiteral("Rejected"), [](const LayoutSnapshot &) { return false; },
        [&default_applied]() { default_applied = true; }, &error);
    REQUIRE(result == LayoutRestoreResult::DefaultRejectedState);
    REQUIRE(default_applied);
    REQUIRE_FALSE(error.isEmpty());

    const QString path = onlyPresetPath(directory);
    QJsonObject root = readPreset(path);
    root.insert(QStringLiteral("window_state"), QStringLiteral("not base64"));
    writePreset(path, root);

    bool invalid_state_applied = false;
    default_applied = false;
    result = manager.restorePreset(
        QStringLiteral("Rejected"),
        [&invalid_state_applied](const LayoutSnapshot &) {
            invalid_state_applied = true;
            return true;
        },
        [&default_applied]() { default_applied = true; }, &error);
    REQUIRE(result == LayoutRestoreResult::DefaultInvalidPreset);
    REQUIRE_FALSE(invalid_state_applied);
    REQUIRE(default_applied);
}

TEST_CASE("Devstudio layout names cannot escape their storage directory", "[devstudio][layout]") {
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    LayoutPresetManager manager(directory.path());
    const LayoutSnapshot snapshot{QByteArrayLiteral("window geometry"), QByteArrayLiteral("dock state")};

    REQUIRE(manager.savePreset(QStringLiteral("../Review 日本語"), snapshot));
    REQUIRE(manager.presetNames() == QStringList{QStringLiteral("../Review 日本語")});
    REQUIRE(QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());

    QString error;
    REQUIRE_FALSE(manager.savePreset(QStringLiteral("\n"), snapshot, &error));
    REQUIRE_FALSE(error.isEmpty());
}

// N-5. A theme is a person's taste, filed the way a panel layout is: the
// window writes it the moment it is chosen and reads it back before it draws
// anything. The control is the write: drop `storeStudioThemeName` from
// MainWindow::chooseTheme and the second window comes up in the default.
TEST_CASE("The View menu switches the theme and the next window keeps it",
          "[devstudio][studio][theme]") {
    (void)application();

    // The preference must not reach the developer's real settings, so this
    // test gives QSettings a file of its own.
    QTemporaryDir settings_directory;
    REQUIRE(settings_directory.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_directory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("PelicanThemeTest"));
    QCoreApplication::setApplicationName(QStringLiteral("Studio"));

    PelicanStudio::resetStudioThemeTable();
    REQUIRE(PelicanStudio::setCurrentStudioTheme(
        PelicanStudio::defaultStudioThemeName()));
    REQUIRE(PelicanStudio::defaultStudioThemeName() == QStringLiteral("dark"));

    const auto themeAction = [](PelicanStudio::MainWindow &window,
                                const char *name) {
        QAction *action =
            window.findChild<QAction *>(QString::fromLatin1(name));
        REQUIRE(action != nullptr);
        return action;
    };

    QTemporaryDir application_config;
    REQUIRE(application_config.isValid());
    {
        PelicanStudio::MainWindow window{application_config.path()};
        REQUIRE(PelicanStudio::currentStudioThemeName() ==
                QStringLiteral("dark"));
        REQUIRE(themeAction(window, "pelican.view.theme.dark")->isChecked());

        themeAction(window, "pelican.view.theme.light")->trigger();
        REQUIRE(PelicanStudio::currentStudioThemeName() ==
                QStringLiteral("light"));
        REQUIRE(themeAction(window, "pelican.view.theme.light")->isChecked());
        REQUIRE_FALSE(themeAction(window, "pelican.view.theme.dark")->isChecked());

        QSettings settings;
        REQUIRE(settings.value(QStringLiteral("studio/theme")).toString() ==
                QStringLiteral("light"));
    }

    // A fresh process starts at the table's default; the window is what puts
    // the stored preference back.
    REQUIRE(PelicanStudio::setCurrentStudioTheme(
        PelicanStudio::defaultStudioThemeName()));
    REQUIRE(PelicanStudio::currentStudioThemeName() == QStringLiteral("dark"));
    {
        PelicanStudio::MainWindow window{application_config.path()};
        REQUIRE(PelicanStudio::currentStudioThemeName() ==
                QStringLiteral("light"));
        REQUIRE(themeAction(window, "pelican.view.theme.light")->isChecked());
    }

    // Leave the process in the theme every other case expects.
    QSettings settings;
    settings.remove(QStringLiteral("studio/theme"));
    settings.sync();
    REQUIRE(PelicanStudio::setCurrentStudioTheme(QStringLiteral("dark")));
}

TEST_CASE("Studio protects unsaved scenes on close and project switching",
          "[devstudio][mainwindow][save][unsaved]") {
    (void)application();
    QTemporaryDir application_config;
    REQUIRE(application_config.isValid());
    PelicanStudio::MainWindow window{application_config.path()};
    auto *inspector = dynamic_cast<PelicanStudio::InspectorWidget *>(
        window.findChild<QWidget *>(QStringLiteral("pelican.inspector")));
    REQUIRE(inspector != nullptr);
    inspector->observeSceneRevision(1);
    inspector->observeSceneRevision(2);
    window.show();
    QCoreApplication::processEvents();
    REQUIRE(window.isWindowModified());

    const auto checkNavigation = [&](QMessageBox::StandardButton answer,
                                     bool switching_project = false) {
        if (switching_project) {
            // If cancellation is ignored this path would attempt to open an
            // invalid project and display another dialog.
            window.openProject(application_config.filePath(QStringLiteral("not-a-project")));
        } else {
            REQUIRE_FALSE(window.close()); // Deferred while the decision is pending.
        }
        QCoreApplication::processEvents();
        auto *prompt = window.findChild<QMessageBox *>(
            QStringLiteral("pelican.unsavedScenePrompt"));
        REQUIRE(prompt != nullptr);
        prompt->button(answer)->click();
        QCoreApplication::processEvents();
        CHECK(window.isVisible() == (answer != QMessageBox::Discard));
        if (answer != QMessageBox::Discard) {
            CHECK(inspector->hasUnsavedSceneChanges());
            CHECK(window.isWindowModified());
        }
        if (answer == QMessageBox::Save) {
            CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("remains open")));
        }
    };

    SECTION("Cancel leaves the dirty window open") {
        checkNavigation(QMessageBox::Cancel);
    }
    SECTION("Discard explicitly permits closing") {
        checkNavigation(QMessageBox::Discard);
    }
    SECTION("a save that cannot start never closes the window") {
        checkNavigation(QMessageBox::Save);
    }
    SECTION("Cancel prevents replacing the project") {
        checkNavigation(QMessageBox::Cancel, true);
    }
}

TEST_CASE("Studio owns an asynchronous unsaved prompt through window destruction",
          "[devstudio][mainwindow][save][unsaved]") {
    (void)application();
    QTemporaryDir application_config;
    REQUIRE(application_config.isValid());
    QPointer<QMessageBox> prompt;
    {
        PelicanStudio::MainWindow window{application_config.path()};
        auto *inspector = dynamic_cast<PelicanStudio::InspectorWidget *>(
            window.findChild<QWidget *>(QStringLiteral("pelican.inspector")));
        REQUIRE(inspector != nullptr);
        inspector->observeSceneRevision(1);
        inspector->observeSceneRevision(2);
        window.show();
        REQUIRE_FALSE(window.close());
        QCoreApplication::processEvents();
        prompt = window.findChild<QMessageBox *>(QStringLiteral("pelican.unsavedScenePrompt"));
        REQUIRE(prompt != nullptr);
    }
    CHECK(prompt.isNull());
    QCoreApplication::processEvents();
}

TEST_CASE("G-5 an engine restart asks about unsaved scene edits first, and "
          "Cancel keeps the engine running",
          "[devstudio][mainwindow][save][unsaved][g5]") {
    (void)application();
    QTemporaryDir application_config;
    REQUIRE(application_config.isValid());
    PelicanStudio::MainWindow window{application_config.path()};
    auto *inspector = dynamic_cast<PelicanStudio::InspectorWidget *>(
        window.findChild<QWidget *>(QStringLiteral("pelican.inspector")));
    REQUIRE(inspector != nullptr);
    auto *frame_plan_status =
        window.findChild<QLabel *>(QStringLiteral("pelican.framePlanStatus"));
    REQUIRE(frame_plan_status != nullptr);
    window.show();
    QCoreApplication::processEvents();
    const QString reason =
        QStringLiteral("Restarting the engine to apply the material route setting...");
    const auto prompt = [&window] {
        return window.findChild<QMessageBox *>(
            QStringLiteral("pelican.unsavedScenePrompt"));
    };
    // Q-9. While the question is open the engine running now is the one about
    // to be stopped: the viewport holds "ready" for whoever waits on it.
    PelicanStudio::EmbeddedViewport *viewport = window.engineViewport();
    REQUIRE(viewport != nullptr);
    REQUIRE_FALSE(viewport->restartPending());

    SECTION("with nothing unsaved the engine restarts at once") {
        window.requestEngineRestart(reason);
        QCoreApplication::processEvents();
        CHECK(prompt() == nullptr);
        CHECK(window.statusBar()->currentMessage() == reason);
        CHECK_FALSE(viewport->restartPending());
    }

    SECTION("with unsaved edits the prompt comes first") {
        inspector->observeSceneRevision(1);
        inspector->observeSceneRevision(2);
        QCoreApplication::processEvents();
        REQUIRE(inspector->hasUnsavedSceneChanges());
        window.requestEngineRestart(reason);
        QCoreApplication::processEvents();
        auto *asked = prompt();
        REQUIRE(asked != nullptr);
        CHECK(asked->text().contains(QStringLiteral("engine restarts")));
        // Nothing restarted while the question is open, and the engine that
        // is answering it does not count as ready (Q-9).
        CHECK(window.statusBar()->currentMessage() != reason);
        CHECK(viewport->restartPending());

        SECTION("Cancel: no restart, and the setting is said to wait for the next start") {
            auto *cancel = asked->findChild<QAbstractButton *>(
                QStringLiteral("pelican.unsavedScenePrompt.cancel"));
            REQUIRE(cancel != nullptr);
            cancel->click();
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            CHECK(prompt() == nullptr);
            CHECK(window.statusBar()->currentMessage() != reason);
            CHECK(window.statusBar()->currentMessage().contains(
                QStringLiteral("applies the next time the engine starts")));
            CHECK(frame_plan_status->text().contains(
                QStringLiteral("applies the next time the engine starts")));
            CHECK(inspector->hasUnsavedSceneChanges());
            // Declined: the engine that stays is the current one again.
            CHECK_FALSE(viewport->restartPending());
        }
        SECTION("Discard: the restart goes ahead") {
            auto *discard = asked->findChild<QAbstractButton *>(
                QStringLiteral("pelican.unsavedScenePrompt.discard"));
            REQUIRE(discard != nullptr);
            discard->click();
            QCoreApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            CHECK(prompt() == nullptr);
            CHECK(window.statusBar()->currentMessage() == reason);
            CHECK_FALSE(viewport->restartPending());
        }
        SECTION("Save that cannot start: no restart") {
            asked->button(QMessageBox::Save)->click();
            QCoreApplication::processEvents();
            CHECK(window.statusBar()->currentMessage() != reason);
            CHECK_FALSE(viewport->restartPending());
            CHECK(frame_plan_status->text().contains(
                QStringLiteral("applies the next time the engine starts")));
        }
    }
}

// UI-1a: the named controls of each control row, in the row's order. The
// hidden-at-start widgets are in the lists (they have a row), but are not
// required to be visible.
namespace {

QStringList graphRowOneNames() {
    return {
        QStringLiteral("pelican.graphCanvasScope"),
        QStringLiteral("pelican.graphCanvasBreadcrumb"),
        QStringLiteral("pelican.graphCanvasLoopBranch"),
        QStringLiteral("pelican.graphCanvasShared"),
        QStringLiteral("pelican.graphCanvasValues"),
        QStringLiteral("pelican.graphCanvasLive"),
        QStringLiteral("pelican.graphCanvas.undoButton"),
        QStringLiteral("pelican.graphCanvas.redoButton"),
        QStringLiteral("pelican.graphCanvasRefresh"),
    };
}

QStringList graphRowTwoNames() {
    QStringList names;
    for (const char *domain : {"render", "pose", "value", "audio", "ui", "canvas"})
        names.append(QStringLiteral("pelican.graphCanvasShow.") + QString::fromLatin1(domain));
    names.append(QStringLiteral("pelican.graphCanvas.autoLayoutButton"));
    names.append(QStringLiteral("pelican.graphCanvas.fitViewButton"));
    names.append(QStringLiteral("pelican.graphCanvas.recomputeOnceButton"));
    return names;
}

QStringList framePlanRowOneNames() {
    return {
        QStringLiteral("pelican.framePlanRefresh"),
        QStringLiteral("pelican.framePlan.undoButton"),
        QStringLiteral("pelican.framePlan.redoButton"),
        QStringLiteral("pelican.framePlanFilter"),
        QStringLiteral("pelican.framePlanTarget"),
        QStringLiteral("pelican.framePlanDepth"),
        QStringLiteral("pelican.framePlanZoomStatus"),
        QStringLiteral("pelican.framePlan.help"),
    };
}

QStringList framePlanRowTwoNames() {
    return {
        QStringLiteral("pelican.framePlanAddMaterialPass"),
        QStringLiteral("pelican.framePlanOrderNodes"),
        QStringLiteral("pelican.framePlanEditTask"),
        QStringLiteral("pelican.framePlanEditPresetPass"),
        QStringLiteral("pelican.framePlanNewRenderTarget"),
    };
}

QWidget *namedControl(QWidget &root, const QString &name) {
    auto *widget = root.findChild<QWidget *>(name);
    REQUIRE(widget != nullptr);
    return widget;
}

template <typename T>
T *namedPanel(QWidget &root, const QString &name) {
    auto *widget = root.findChild<QWidget *>(name);
    REQUIRE(widget != nullptr);
    auto *panel = dynamic_cast<T *>(widget);
    REQUIRE(panel != nullptr);
    return panel;
}

QLayout *namedRow(QWidget &root, const QString &name) {
    QObject *object = root.findChild<QObject *>(name);
    REQUIRE(object != nullptr);
    auto *layout = dynamic_cast<QLayout *>(object);
    REQUIRE(layout != nullptr);
    return layout;
}

void settle(QApplication &app) {
    app.processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    app.processEvents();
}

QStringList chainNames(const QStringList &expected, QWidget &window) {
    QSet<QString> wanted{expected.cbegin(), expected.cend()};
    QStringList seen;
    QWidget *start = window.nextInFocusChain();
    QWidget *at = start;
    for (int step = 0; step < 10000 && at != nullptr; ++step) {
        if (wanted.contains(at->objectName())) seen.append(at->objectName());
        at = at->nextInFocusChain();
        if (at == start) break;
    }
    return seen;
}

} // namespace

// The real budget is 1536 logical pixels (a 1920-wide screen at 125%); the real
// window measures 1323 on Windows through the size_hints command. This pins the
// offscreen platform as a regression check, and its fonts on Windows are wider.
#ifdef _WIN32
constexpr int kUi1aOffscreenFloor = 1900;
constexpr int kUi1aOffscreenStressFloor = 2250;
#else
constexpr int kUi1aOffscreenFloor = 1500;
constexpr int kUi1aOffscreenStressFloor = 1500;
#endif

TEST_CASE("UI-1a the split rows keep the whole window's floor under 1500 pixels",
          "[devstudio][layout][ui1a]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(app);

    const auto dock = [&window](const char *name) {
        auto *result = window.findChild<QDockWidget *>(QString::fromLatin1(name));
        REQUIRE(result != nullptr);
        return result;
    };
    QDockWidget *graph = dock("pelican.graphDock");
    QDockWidget *frame_plan = dock("pelican.framePlanDock");
    QDockWidget *output = dock("pelican.outputDock");
    QDockWidget *features = dock("pelican.renderFeaturesDock");
    QDockWidget *rendering_settings = dock("pelican.renderingSettingsDock");

    // The default layout: Graph and the Frame Plan bundle side by side.
    bool side_by_side = graph->geometry().right() < frame_plan->geometry().left();
    if (!side_by_side) {
        auto *reset = window.findChild<QAction *>(
            QStringLiteral("pelican.layout.resetDefault"));
        REQUIRE(reset != nullptr);
        reset->trigger();
        settle(app);
        side_by_side = graph->geometry().right() < frame_plan->geometry().left();
    }
    REQUIRE(side_by_side);
    const auto stack = window.tabifiedDockWidgets(output);
    for (QDockWidget *member : {dock("pelican.engineLogDock"), dock("pelican.problemsDock"),
                                frame_plan, dock("pelican.fullscreenPassDock"), features,
                                rendering_settings}) {
        INFO(member->objectName().toStdString());
        CHECK(stack.contains(member));
    }

    // Every dock's own floor, so a failure names the wall. (The bundle tabs
    // share the space of the one visible tab.)
    for (QDockWidget *member : window.findChildren<QDockWidget *>()) {
        if (!member->isVisible()) continue;
        INFO("dock " << member->objectName().toStdString()
             << " minimumSizeHint=" << member->minimumSizeHint().width()
             << " minimumSize=" << member->minimumSize().width());
        CHECK(member->minimumSizeHint().width() <= kUi1aOffscreenFloor);
    }

    // The width must not depend on which bundle tab is in front.
    QList<QDockWidget *> bundle{output};
    for (QDockWidget *member : stack)
        if (!bundle.contains(member)) bundle.append(member);
    for (QDockWidget *member : bundle) {
        member->raise();
        settle(app);
        INFO("bundle tab " << member->objectName().toStdString());
        INFO("window minimumSizeHint width: " << window.minimumSizeHint().width());
        CHECK(window.minimumSizeHint().width() <= kUi1aOffscreenFloor);
    }

    // Stress the text and the hidden rows. A 300-character status, a long
    // breadcrumb, a long target name, and the controls that a refusal shows
    // must not raise the floor above the budget.
    auto *graph_panel = namedPanel<PelicanStudio::GraphCanvasWidget>(
        window, QStringLiteral("pelican.graphCanvas"));
    auto *frame_plan_panel = namedPanel<PelicanStudio::FramePlanWidget>(
        window, QStringLiteral("pelican.framePlan"));
    REQUIRE(graph_panel != nullptr);
    REQUIRE(frame_plan_panel != nullptr);
    auto *breadcrumb = qobject_cast<QLabel *>(
        namedControl(*graph_panel, QStringLiteral("pelican.graphCanvasBreadcrumb")));
    REQUIRE(breadcrumb != nullptr);
    auto *target = qobject_cast<QComboBox *>(
        namedControl(*frame_plan_panel, QStringLiteral("pelican.framePlanTarget")));
    REQUIRE(target != nullptr);

    const auto floor = [&window](const char *stage) {
        INFO("stage " << stage << " window minimumSizeHint width: "
             << window.minimumSizeHint().width());
        return window.minimumSizeHint().width();
    };
    const int default_floor = floor("default");
    frame_plan->raise();
    settle(app);

    QString trail;
    for (int depth = 0; depth < 5; ++depth) {
        if (!trail.isEmpty()) trail += QStringLiteral(" / ");
        trail += QStringLiteral("nested_scope_%1").arg(depth);
    }
    breadcrumb->setText(trail);
    settle(app);
    const int breadcrumb_floor = floor("long breadcrumb");

    const QString long_target = QStringLiteral("render_target_with_a_long_name_") + QString(120, QLatin1Char('x'));
    target->addItem(long_target);
    target->setCurrentIndex(target->count() - 1);
    settle(app);
    const int target_floor = floor("long target name");

    const QString long_status(300, QLatin1Char('s'));
    frame_plan_panel->showRenderHistoryLine(long_status);
    settle(app);
    const int status_floor = floor("300-character status");

    const QString long_perf(300, QLatin1Char('p'));
    graph_panel->setPerfLines(long_perf, long_perf);
    frame_plan_panel->setPerfLines(long_perf, long_perf);
    settle(app);
    const int perf_floor = floor("300-character perf");
    for (QLabel *label : {qobject_cast<QLabel *>(namedControl(
                              *graph_panel, QStringLiteral("pelican.graphCanvasPerf"))),
                          qobject_cast<QLabel *>(namedControl(
                              *frame_plan_panel, QStringLiteral("pelican.framePlanPerf")))}) {
        REQUIRE(label != nullptr);
        CHECK(label->toolTip() == long_perf);
        CHECK(label->accessibleDescription() == long_perf);
    }

    for (const char *name : {"pelican.graphCanvasLoopBranch", "pelican.graphCanvasShared",
                             "pelican.framePlan.reloadRefused.disconnect",
                             "pelican.framePlan.reloadRefused.openShader"}) {
        auto *widget = namedControl(window, QString::fromLatin1(name));
        widget->show();
    }
    settle(app);
    const int refusal_floor = floor("hidden controls shown");

    INFO("floors: default=" << default_floor << " breadcrumb=" << breadcrumb_floor
         << " target=" << target_floor << " status=" << status_floor
         << " perf=" << perf_floor << " refusal=" << refusal_floor);
    // The refusal buttons are transient and may raise the floor above the default.
    CHECK(window.minimumSizeHint().width() <= kUi1aOffscreenStressFloor);

    // At the computed floor every visible control of both design rows has a
    // rectangle, and no two rectangles overlap.
    window.resize(window.minimumSizeHint());
    settle(app);
    QStringList graph_names = graphRowOneNames();
    graph_names.append(graphRowTwoNames());
    QStringList plan_names = framePlanRowOneNames();
    plan_names.append(framePlanRowTwoNames());
    QList<QWidget *> controls;
    for (const QString &name : graph_names) {
        auto *widget = namedControl(*graph_panel, name);
        if (widget->isVisible()) controls.append(widget);
    }
    for (const QString &name : plan_names) {
        auto *widget = namedControl(*frame_plan_panel, name);
        if (widget->isVisible()) controls.append(widget);
    }
    REQUIRE(controls.size() >= 15);
    for (QWidget *widget : controls) {
        INFO("control " << widget->objectName().toStdString()
             << " size=" << widget->width() << "x" << widget->height());
        CHECK_FALSE(widget->rect().isEmpty());
    }
    for (int left = 0; left < controls.size(); ++left) {
        const QRect a{controls[left]->mapTo(&window, QPoint(0, 0)), controls[left]->size()};
        for (int right = left + 1; right < controls.size(); ++right) {
            const QRect b{controls[right]->mapTo(&window, QPoint(0, 0)), controls[right]->size()};
            INFO("overlap " << controls[left]->objectName().toStdString()
                 << " and " << controls[right]->objectName().toStdString());
            CHECK_FALSE(a.intersects(b));
        }
    }
}

TEST_CASE("UI-1a each control sits on its named row and fits the window minimum",
          "[devstudio][layout][ui1a]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(app);

    auto *graph_panel = namedPanel<PelicanStudio::GraphCanvasWidget>(
        window, QStringLiteral("pelican.graphCanvas"));
    auto *frame_plan_panel = namedPanel<PelicanStudio::FramePlanWidget>(
        window, QStringLiteral("pelican.framePlan"));
    REQUIRE(graph_panel != nullptr);
    REQUIRE(frame_plan_panel != nullptr);
    auto *frame_plan_dock = window.findChild<QDockWidget *>(
        QStringLiteral("pelican.framePlanDock"));
    REQUIRE(frame_plan_dock != nullptr);
    frame_plan_dock->raise();
    settle(app);

    QLayout *graph_one = namedRow(*graph_panel, QStringLiteral("controls_top"));
    QLayout *graph_two = namedRow(*graph_panel, QStringLiteral("controls_tools"));
    QLayout *plan_one = namedRow(*frame_plan_panel, QStringLiteral("toolbar_row"));
    QLayout *plan_two = namedRow(*frame_plan_panel, QStringLiteral("frame_plan_tools_row"));
    REQUIRE(plan_one->objectName() == QStringLiteral("toolbar_row"));

    const QSet<QString> start_hidden{
        QStringLiteral("pelican.graphCanvasLoopBranch"),
        QStringLiteral("pelican.graphCanvasShared"),
        QStringLiteral("pelican.graphCanvas.recomputeOnceButton"),
    };
    const std::pair<QLayout *, QStringList> rows[] = {
        {graph_one, graphRowOneNames()},
        {graph_two, graphRowTwoNames()},
        {plan_one, framePlanRowOneNames()},
        {plan_two, framePlanRowTwoNames()},
    };
    for (const auto &[layout, names] : rows) {
        for (const QString &name : names) {
            auto *widget = namedControl(window, name);
            INFO("row membership " << name.toStdString()
                 << " row=" << layout->objectName().toStdString());
            CHECK(layout->indexOf(widget) >= 0);
            if (!start_hidden.contains(name)) CHECK(widget->isVisible());
        }
    }

    // The window's own floor is the honest narrow width; a maximum-size
    // clamp would be a different test.
    window.resize(window.minimumSizeHint());
    settle(app);
    for (const auto &[layout, names] : rows) {
        Q_UNUSED(layout);
        for (const QString &name : names) {
            auto *widget = namedControl(window, name);
            if (!widget->isVisible()) continue;
            INFO("narrow control " << name.toStdString()
                 << " width=" << widget->width()
                 << " minimumSizeHint=" << widget->minimumSizeHint().width());
            // The shared "?" help button is deliberately fixed at 22 pixels;
            // Ignored labels (the breadcrumb, status and perf summaries) are
            // allowed to shrink to their explicit minimum.
            if (widget->minimumWidth() == widget->maximumWidth())
                CHECK(widget->width() >= widget->minimumSize().width());
            else if (widget->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored)
                CHECK(widget->width() >= widget->minimumWidth());
            else
                CHECK(widget->width() >= widget->minimumSizeHint().width());
        }
    }
}

TEST_CASE("UI-1a a long frame plan status clips without widening its row",
          "[devstudio][layout][ui1a]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(app);
    auto *frame_plan = namedPanel<PelicanStudio::FramePlanWidget>(
        window, QStringLiteral("pelican.framePlan"));
    REQUIRE(frame_plan != nullptr);
    auto *frame_plan_dock = window.findChild<QDockWidget *>(
        QStringLiteral("pelican.framePlanDock"));
    REQUIRE(frame_plan_dock != nullptr);
    frame_plan_dock->raise();
    settle(app);

    auto *status = namedControl(*frame_plan, QStringLiteral("pelican.framePlanStatus"));
    auto *row = namedRow(*frame_plan, QStringLiteral("frame_plan_status_row"));
    const QString short_text = QStringLiteral("short status");
    frame_plan->showRenderHistoryLine(short_text);
    settle(app);
    REQUIRE(status->isVisible());
    const int short_floor = row->minimumSize().width();

    // 300 characters: longer than the panel, and no reason for the row to
    // grow. The whole text stays in the tooltip.
    const QString long_text(300, QLatin1Char('s'));
    frame_plan->showRenderHistoryLine(long_text);
    settle(app);
    INFO("status row minimum with short text: " << short_floor);
    INFO("status row minimum with 300 characters: " << row->minimumSize().width());
    CHECK(row->minimumSize().width() == short_floor);
    CHECK(status->toolTip() == long_text);
    CHECK(status->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored);
}

TEST_CASE("UI-1a tab order walks each control row and then the next",
          "[devstudio][layout][ui1a]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(app);

    auto *graph_panel = namedPanel<PelicanStudio::GraphCanvasWidget>(
        window, QStringLiteral("pelican.graphCanvas"));
    auto *frame_plan_panel = namedPanel<PelicanStudio::FramePlanWidget>(
        window, QStringLiteral("pelican.framePlan"));
    REQUIRE(graph_panel != nullptr);
    REQUIRE(frame_plan_panel != nullptr);

    const auto focusable = [](QWidget &panel, const QStringList &names) {
        QStringList focusable_names;
        for (const QString &name : names) {
            auto *widget = namedControl(panel, name);
            if (widget->focusPolicy() != Qt::NoFocus)
                focusable_names.append(name);
        }
        return focusable_names;
    };
    QStringList graph_expected = focusable(*graph_panel, graphRowOneNames());
    graph_expected.append(focusable(*graph_panel, graphRowTwoNames()));
    QStringList plan_expected = focusable(*frame_plan_panel, framePlanRowOneNames());
    plan_expected.append(focusable(*frame_plan_panel, framePlanRowTwoNames()));

    const QStringList graph_seen = chainNames(graph_expected, *graph_panel);
    INFO("graph focus chain: " << graph_seen.join(QStringLiteral(", ")).toStdString());
    CHECK(graph_seen == graph_expected);
    const QStringList plan_seen = chainNames(plan_expected, *frame_plan_panel);
    INFO("frame plan focus chain: " << plan_seen.join(QStringLiteral(", ")).toStdString());
    CHECK(plan_seen == plan_expected);
}

TEST_CASE("UI-1a refresh, undo, fit view and filter still act from the split rows",
          "[devstudio][layout][ui1a]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(app);

    auto *graph_panel = namedPanel<PelicanStudio::GraphCanvasWidget>(
        window, QStringLiteral("pelican.graphCanvas"));
    auto *frame_plan_panel = namedPanel<PelicanStudio::FramePlanWidget>(
        window, QStringLiteral("pelican.framePlan"));
    REQUIRE(graph_panel != nullptr);
    REQUIRE(frame_plan_panel != nullptr);
    auto *frame_plan_dock = window.findChild<QDockWidget *>(
        QStringLiteral("pelican.framePlanDock"));
    REQUIRE(frame_plan_dock != nullptr);
    frame_plan_dock->raise();
    settle(app);

    auto *graph_refresh = qobject_cast<QPushButton *>(
        namedControl(*graph_panel, QStringLiteral("pelican.graphCanvasRefresh")));
    auto *graph_undo = qobject_cast<QPushButton *>(
        namedControl(*graph_panel, QStringLiteral("pelican.graphCanvas.undoButton")));
    auto *graph_fit = qobject_cast<QPushButton *>(
        namedControl(*graph_panel, QStringLiteral("pelican.graphCanvas.fitViewButton")));
    REQUIRE(graph_refresh != nullptr);
    REQUIRE(graph_undo != nullptr);
    REQUIRE(graph_fit != nullptr);

    auto *refresh = qobject_cast<QPushButton *>(
        namedControl(*frame_plan_panel, QStringLiteral("pelican.framePlanRefresh")));
    auto *filter = qobject_cast<QLineEdit *>(
        namedControl(*frame_plan_panel, QStringLiteral("pelican.framePlanFilter")));
    REQUIRE(refresh != nullptr);
    REQUIRE(filter != nullptr);

    // Refresh: the button calls the panel's refresh. A plan request is the
    // observable side of that call.
    int plan_requests = 0;
    frame_plan_panel->setPlanRequest([&plan_requests](QString *) {
        ++plan_requests;
        return qint64{1};
    });
    // No engine is attached offscreen, so the panel's readiness gate leaves
    // the button disabled; the click's wiring is what this checks.
    refresh->setEnabled(true);
    refresh->click();
    settle(app);
    CHECK(plan_requests == 1);

    // Filter: typing still reaches the panel's filter (and rebuilds without
    // touching the text).
    filter->setText(QStringLiteral("material"));
    settle(app);
    CHECK(filter->text() == QStringLiteral("material"));
    CHECK(filter->isVisible());

    // Undo and Fit view fire their actions after the row split.
    auto *undo_action = graph_panel->findChild<QAction *>(
        QStringLiteral("pelican.graphCanvas.undo"));
    auto *fit_action = graph_panel->findChild<QAction *>(
        QStringLiteral("pelican.graphCanvas.fitView"));
    REQUIRE(undo_action != nullptr);
    REQUIRE(fit_action != nullptr);
    undo_action->setEnabled(true);
    int undo_triggered = 0;
    int fit_triggered = 0;
    QObject::connect(undo_action, &QAction::triggered, [&undo_triggered] { ++undo_triggered; });
    QObject::connect(fit_action, &QAction::triggered, [&fit_triggered] { ++fit_triggered; });
    graph_undo->click();
    graph_fit->click();
    settle(app);
    CHECK(undo_triggered == 1);
    CHECK(fit_triggered == 1);

    // Refresh on the Graph is a click with no crash and no disabled state.
    graph_refresh->click();
    settle(app);
    CHECK(graph_refresh->isVisible());
}

namespace {

QDockWidget *namedDock(QWidget &root, const char *name) {
    auto *dock = root.findChild<QDockWidget *>(QString::fromLatin1(name));
    REQUIRE(dock != nullptr);
    return dock;
}

PelicanStudio::DockTitleBar *titleBarOf(QDockWidget *dock) {
    auto *bar = qobject_cast<PelicanStudio::DockTitleBar *>(dock->titleBarWidget());
    REQUIRE(bar != nullptr);
    return bar;
}

QToolButton *dockButton(QDockWidget *dock, const char *suffix) {
    auto *button = dock->findChild<QToolButton *>(
        dock->objectName() + QLatin1Char('.') + QString::fromLatin1(suffix));
    REQUIRE(button != nullptr);
    return button;
}

void floatDock(QApplication &app, QDockWidget *dock) {
    dock->setFloating(true);
    if (!dock->isVisible()) dock->show();
    QTest::qWait(50);
    settle(app);
}

// The offscreen platform can crash while destroying a main window that still
// owns a floating top-level dock. No DK-1 assertion needs the window to go
// away in that state, so put every dock back before the window's destructor.
class DockCleanup final {
public:
    DockCleanup(QMainWindow &window, QApplication &app) : window_(window), app_(app) {}
    ~DockCleanup() {
        for (QDockWidget *dock : window_.findChildren<QDockWidget *>()) {
            if (dock->isFloating() || dock->window() != &window_) dock->setFloating(false);
        }
        settle(app_);
    }
    DockCleanup(const DockCleanup &) = delete;
    DockCleanup &operator=(const DockCleanup &) = delete;

private:
    QMainWindow &window_;
    QApplication &app_;
};

} // namespace

TEST_CASE("DK-1 every dock has a title bar whose buttons and title follow the dock",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(application());

    const QList<QDockWidget *> docks = window.findChildren<QDockWidget *>();
    REQUIRE(docks.size() >= 14);
    for (QDockWidget *dock : docks) {
        INFO("dock " << dock->objectName().toStdString());
        REQUIRE_FALSE(dock->objectName().isEmpty());
        auto *bar = titleBarOf(dock);
        CHECK(bar->isVisible() == dock->isVisible());
        const int title_height =
            window.style()->pixelMetric(QStyle::PM_TitleBarHeight, nullptr, bar);
        CHECK(bar->minimumHeight() == title_height);
        CHECK(bar->maximumHeight() == title_height);
        if (bar->isVisible()) CHECK(bar->height() == title_height);
        CHECK(bar->titleLabel()->text() == dock->windowTitle());
        for (const char *suffix : {"maximize", "float", "close"}) {
            auto *button = dockButton(dock, suffix);
            CHECK(button->parentWidget() == bar);
            CHECK_FALSE(button->icon().isNull());
        }
    }

    auto *project = namedDock(window, "pelican.projectDock");
    auto *bar = titleBarOf(project);
    project->setWindowTitle(QStringLiteral("Renamed project"));
    settle(application());
    CHECK(bar->titleLabel()->text() == QStringLiteral("Renamed project"));
    CHECK(project->windowTitle() == QStringLiteral("Renamed project"));
}

TEST_CASE("PRJ-1a the Project tree does not raise the window's floor",
          "[prj1a][ui1a]") {
    QApplication &app = application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(app);

    auto *dock = window.findChild<QDockWidget *>(QStringLiteral("pelican.projectDock"));
    REQUIRE(dock != nullptr);
    CHECK(window.findChild<QAction *>(QStringLiteral("pelican.view.panels.projectDock")) != nullptr);
    CHECK(window.findChild<QWidget *>(QStringLiteral("pelican.projectPanel")) != nullptr);
    auto *list = dock->findChild<QListWidget *>();
    REQUIRE(list != nullptr);
    REQUIRE(list->count() == 1);
    CHECK(list->item(0)->text() == QStringLiteral("No project open"));
    CHECK(list->maximumHeight() ==
          2 * list->sizeHintForRow(0) + 2 * list->frameWidth());
    CHECK(list->horizontalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    CHECK(list->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff);
    CHECK(list->textElideMode() == Qt::ElideMiddle);
    auto *search = dock->findChild<QLineEdit *>(QStringLiteral("pelican.project.search"));
    REQUIRE(search != nullptr);
    CHECK(search->placeholderText() == QStringLiteral("Filter by path"));
    CHECK(dock->findChild<QComboBox *>(QStringLiteral("pelican.project.kind")) != nullptr);
    CHECK(dock->findChild<QPushButton *>(QStringLiteral("pelican.project.refresh")) != nullptr);
    CHECK(dock->findChild<QWidget *>(QStringLiteral("pelican.projectTree")) != nullptr);
    for (const char *name : {"pelican.project.truncated", "pelican.project.notice"}) {
        auto *label = dock->findChild<QLabel *>(QString::fromLatin1(name));
        REQUIRE(label != nullptr);
        CHECK(label->wordWrap());
    }
    CHECK(window.isVisible());
    CHECK(window.geometry().isValid());
    CHECK(window.width() > 0);

    const int with_tree = window.minimumSizeHint().width();
    INFO("with_tree=" << with_tree);
    auto *old_page = dock->widget();
    REQUIRE(old_page != nullptr);
    old_page->setParent(&window);
    old_page->hide();
    dock->setWidget(new QListWidget);
    settle(app);
    const int with_list = window.minimumSizeHint().width();
    INFO("with_list=" << with_list);
    CHECK(with_tree <= with_list);
}

TEST_CASE("DK-1 maximize is hidden when docked and disabled for a floating group",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(application());
    DockCleanup cleanup{window, application()};

    auto *inspector = namedDock(window, "pelican.inspectorDock");
    auto *maximize = dockButton(inspector, "maximize");
    CHECK(inspector->titleBarWidget() == titleBarOf(inspector));
    CHECK(inspector->isFloating() == false);
    CHECK(maximize->isHidden());
    CHECK_FALSE(inspector->titleBarWidget()->isHidden());

    floatDock(application(), inspector);
    REQUIRE(inspector->isFloating());
    CHECK_FALSE(maximize->isHidden());
    CHECK(maximize->isVisible());
    CHECK(maximize->isEnabled());

    // A dock that is not top level and whose window is not a QMainWindow is
    // how a floating tab group presents itself to the title bar. Driving a
    // real drag into QDockWidgetGroupWindow here leaves the offscreen plugin
    // with a backing store that crashes when MainWindow is destroyed, so use
    // the same parent/window shape directly. The button logic below is the
    // one a real group gets; the drag itself stays QDockWidget's.
    QWidget group_host;
    group_host.resize(320, 220);
    auto *group_dock = new QDockWidget(QStringLiteral("Grouped"), &group_host);
    group_dock->setObjectName(QStringLiteral("test.groupDock"));
    group_dock->setWidget(new QLabel(QStringLiteral("grouped")));
    group_dock->setTitleBarWidget(new PelicanStudio::DockTitleBar(group_dock));
    group_host.show();
    settle(application());

    CHECK_FALSE(group_dock->isWindow());
    CHECK(group_dock->window() == &group_host);
    auto *group_bar = titleBarOf(group_dock);
    REQUIRE(group_bar != nullptr);
    auto *group_maximize = group_bar->maximizeButton();
    CHECK_FALSE(group_maximize->isHidden());
    CHECK_FALSE(group_maximize->isEnabled());
    CHECK(group_maximize->toolTip().contains(QStringLiteral("single floating")));
}

TEST_CASE("DK-1 maximize and restore a floating dock with its geometry",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(application());
    DockCleanup cleanup{window, application()};

    auto *inspector = namedDock(window, "pelican.inspectorDock");
    floatDock(application(), inspector);
    REQUIRE(inspector->isFloating());
    auto *maximize = dockButton(inspector, "maximize");
    REQUIRE(maximize->isVisible());
    REQUIRE(maximize->isEnabled());
    const QRect normal_geometry = inspector->geometry();
    const QIcon normal_icon = maximize->icon();

    maximize->click();
    QTest::qWait(50);
    settle(application());
    CHECK(inspector->isMaximized());
    CHECK(maximize->toolTip() == QStringLiteral("Restore"));
    CHECK(maximize->icon().cacheKey() != normal_icon.cacheKey());

    maximize->click();
    QTest::qWait(50);
    settle(application());
    CHECK_FALSE(inspector->isMaximized());
    CHECK(maximize->toolTip() == QStringLiteral("Maximize"));
    CHECK(inspector->geometry() == normal_geometry);
}

TEST_CASE("DK-1 a maximized dock returns normal before it docks again",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(application());
    DockCleanup cleanup{window, application()};

    auto *inspector = namedDock(window, "pelican.inspectorDock");
    floatDock(application(), inspector);
    REQUIRE(inspector->isFloating());
    auto *maximize = dockButton(inspector, "maximize");
    auto *float_button = dockButton(inspector, "float");
    const QSize normal_size = inspector->size();

    maximize->click();
    QTest::qWait(50);
    settle(application());
    REQUIRE(inspector->isMaximized());
    const QSize maximized_size = inspector->size();
    REQUIRE(maximized_size != normal_size);

    float_button->click();
    QTest::qWait(50);
    settle(application());
    CHECK_FALSE(inspector->isFloating());
    CHECK_FALSE(inspector->isMaximized());
    CHECK(qobject_cast<QMainWindow *>(inspector->window()) == &window);
    CHECK(inspector->isVisible());
    CHECK(window.rect().contains(inspector->geometry()));

    // The maximized size must not be the size the next float comes back at.
    float_button->click();
    QTest::qWait(50);
    settle(application());
    CHECK(inspector->isFloating());
    CHECK(inspector->size() != maximized_size);
    CHECK(inspector->size() == normal_size);
}

TEST_CASE("DK-1 save and restore keep a floating dock and Reset docks it",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(application());
    DockCleanup cleanup{window, application()};

    auto *inspector = namedDock(window, "pelican.inspectorDock");
    floatDock(application(), inspector);
    REQUIRE(inspector->isFloating());

    const QByteArray state =
        window.saveState(PelicanStudio::LayoutPresetManager::WindowStateVersion);
    REQUIRE_FALSE(state.isEmpty());
    inspector->setFloating(false);
    window.addDockWidget(Qt::RightDockWidgetArea, inspector);
    settle(application());
    REQUIRE_FALSE(inspector->isFloating());
    REQUIRE(window.restoreState(
        state, PelicanStudio::LayoutPresetManager::WindowStateVersion));
    QTest::qWait(50);
    settle(application());

    CHECK(inspector->isFloating());
    CHECK(inspector->isVisible());
    auto *bar = titleBarOf(inspector);
    CHECK(bar->isVisible());
    CHECK_FALSE(bar->maximizeButton()->isHidden());
    CHECK(bar->maximizeButton()->isEnabled());

    auto *reset = window.findChild<QAction *>(QStringLiteral("pelican.layout.resetDefault"));
    REQUIRE(reset != nullptr);
    reset->trigger();
    QTest::qWait(50);
    settle(application());
    CHECK_FALSE(inspector->isFloating());
    CHECK(window.dockWidgetArea(inspector) == Qt::RightDockWidgetArea);
    CHECK(inspector->isVisible());
    CHECK(titleBarOf(inspector) == bar);
    CHECK(bar->maximizeButton()->isHidden());
}

TEST_CASE("DK-1 a main window shortcut still arrives while a floating dock is active",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    window.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&window));
    settle(application());
    DockCleanup cleanup{window, application()};

    auto *inspector = namedDock(window, "pelican.inspectorDock");
    floatDock(application(), inspector);
    REQUIRE(inspector->isFloating());

    auto *play = window.findChild<QAction *>(QStringLiteral("pelican.play.play"));
    REQUIRE(play != nullptr);
    play->setEnabled(false); // The shortcut is the subject; do not start Play.
    auto *shortcut = window.findChild<QShortcut *>(QStringLiteral("pelican.play.playKey"));
    REQUIRE(shortcut != nullptr);
    CHECK(shortcut->context() == Qt::WindowShortcut);
    QSignalSpy activated{shortcut, &QShortcut::activated};

    inspector->raise();
    inspector->activateWindow();
    if (!QTest::qWaitForWindowActive(inspector)) {
        WARN("DK-1: this platform cannot activate a floating tool window; "
             "the shortcut route is covered on Windows");
        return;
    }
    QTest::keyClick(inspector, Qt::Key_P, Qt::ControlModifier);
    QTest::qWait(50);
    settle(application());
    CHECK(activated.count() == 1);
}

TEST_CASE("DK-1 a press on the title bar is ignored and reaches the dock",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(application());

    auto *inspector = namedDock(window, "pelican.inspectorDock");
    auto *bar = titleBarOf(inspector);
    struct PressWatcher final : QObject {
        int presses = 0;
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::MouseButtonPress) ++presses;
            return false;
        }
    } watcher;
    inspector->installEventFilter(&watcher);

    const QPoint local{4, bar->height() / 2};
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, local);
    QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, local);
    settle(application());
    inspector->removeEventFilter(&watcher);
    CHECK(watcher.presses >= 1);

    // A bar on its own has no dock parent to take an ignored event: accepted
    // false here is exactly the flag QDockWidget's drag path depends on.
    PelicanStudio::DockTitleBar standalone(inspector);
    standalone.setParent(nullptr);
    standalone.resize(200, bar->height());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(4, 12), QPointF(4, 12),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&standalone, &press);
    CHECK_FALSE(press.isAccepted());
}

TEST_CASE("DK-1 a drag from maximized restores the normal window before moving",
          "[devstudio][layout][dk1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    PelicanStudio::MainWindow window{config.path()};
    window.show();
    settle(application());
    DockCleanup cleanup{window, application()};

    auto *inspector = namedDock(window, "pelican.inspectorDock");
    floatDock(application(), inspector);
    REQUIRE(inspector->isFloating());
    auto *bar = titleBarOf(inspector);
    const QSize normal_size = inspector->size();
    auto *maximize = dockButton(inspector, "maximize");
    maximize->click();
    QTest::qWait(50);
    settle(application());
    REQUIRE(inspector->isMaximized());
    REQUIRE(inspector->size() != normal_size);

    const QPoint local{bar->width() / 2, bar->height() / 2};
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, local);
    QTest::qWait(30);
    CHECK(inspector->isMaximized());

    QTest::mouseMove(bar, local + QPoint(30, 15));
    QTest::qWait(50);
    settle(application());
    CHECK_FALSE(inspector->isMaximized());
    CHECK(inspector->size() == normal_size);
    QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, local + QPoint(30, 15));
    QTest::qWait(50);
    settle(application());

    auto *float_button = dockButton(inspector, "float");
    float_button->click();
    QTest::qWait(50);
    settle(application());
    CHECK_FALSE(inspector->isFloating());
    float_button->click();
    QTest::qWait(50);
    settle(application());
    CHECK(inspector->isFloating());
    CHECK(inspector->size() == normal_size);
}

TEST_CASE("DK-1 the maximize glyph stands out from the bar it sits on",
          "[devstudio][layout][dk1]") {
    (void)application();
    QMainWindow window;
    QPalette dark;
    dark.setColor(QPalette::Window, QColor(43, 49, 56));
    dark.setColor(QPalette::WindowText, QColor(230, 230, 230));
    // A floating dock is a window, and a window takes the application's palette.
    const QPalette saved = QApplication::palette();
    QApplication::setPalette(dark);
    auto *dock = new QDockWidget(QStringLiteral("Panel"), &window);
    dock->setObjectName(QStringLiteral("dk1.icon"));
    auto *bar = new PelicanStudio::DockTitleBar(dock);
    dock->setTitleBarWidget(bar);
    window.addDockWidget(Qt::LeftDockWidgetArea, dock);
    window.show();
    dock->setFloating(true);
    application().processEvents();
    auto *maximize = dock->findChild<QToolButton *>(QStringLiteral("dk1.icon.maximize"));
    REQUIRE(maximize != nullptr);
    REQUIRE(maximize->isEnabled());
    const QImage image = maximize->icon().pixmap(QSize(16, 16)).toImage().convertToFormat(QImage::Format_ARGB32);
    int visible = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.alpha() > 100 && pixel.lightness() > 150) ++visible;
        }
    }
    QApplication::setPalette(saved);
    CHECK(visible >= 20);
}
