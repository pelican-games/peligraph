#include "layoutpreset.hpp"
#include "mainwindow.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QSet>
#include <QTemporaryDir>
#include <QWidget>

namespace {

using PelicanStudio::LayoutPresetManager;
using PelicanStudio::LayoutRestoreResult;
using PelicanStudio::LayoutSnapshot;

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
    int argument_count = 1;
    char application_name[] = "pelican_layout_compatibility_test";
    char *arguments[] = {application_name};
    QApplication application{argument_count, arguments};

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
    QDockWidget *legacy_output = addDock(
        six_dock_window, QStringLiteral("pelican.outputDock"),
        Qt::BottomDockWidgetArea);
    QDockWidget *legacy_engine_log = addDock(
        six_dock_window, QStringLiteral("pelican.engineLogDock"),
        Qt::BottomDockWidgetArea);
    QDockWidget *legacy_frame_plan = addDock(
        six_dock_window, QStringLiteral("pelican.framePlanDock"),
        Qt::BottomDockWidgetArea);
    six_dock_window.tabifyDockWidget(legacy_project, legacy_outliner);
    six_dock_window.tabifyDockWidget(legacy_output, legacy_engine_log);
    six_dock_window.tabifyDockWidget(legacy_engine_log,
                                    legacy_frame_plan);
    legacy_inspector->hide();

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

    PelicanStudio::MainWindow current_window;
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
    REQUIRE(current_window.dockWidgetArea(fullscreen_pass) !=
            Qt::NoDockWidgetArea);

    const auto docks = current_window.findChildren<QDockWidget *>();
    REQUIRE(docks.size() == 7);
    QSet<QString> object_names;
    for (const QDockWidget *current : docks) {
        REQUIRE_FALSE(current->objectName().isEmpty());
        object_names.insert(current->objectName());
    }
    REQUIRE(object_names.size() == 7);

    auto *panels = current_window.findChild<QMenu *>(
        QStringLiteral("pelican.panelsMenu"));
    REQUIRE(panels != nullptr);
    REQUIRE(panels->actions().size() == 7);
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
