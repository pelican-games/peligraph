#include "layoutpreset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
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

    QMainWindow wp249_window;
    QDockWidget *legacy_project = addDock(
        wp249_window, QStringLiteral("pelican.projectDock"), Qt::LeftDockWidgetArea);
    QDockWidget *legacy_outliner = addDock(
        wp249_window, QStringLiteral("pelican.outlinerDock"), Qt::LeftDockWidgetArea);
    wp249_window.tabifyDockWidget(legacy_project, legacy_outliner);
    addDock(wp249_window, QStringLiteral("pelican.inspectorDock"), Qt::RightDockWidgetArea);
    addDock(wp249_window, QStringLiteral("pelican.outputDock"), Qt::BottomDockWidgetArea);
    const QByteArray wp249_state =
        wp249_window.saveState(LayoutPresetManager::WindowStateVersion);
    REQUIRE_FALSE(wp249_state.isEmpty());

    QMainWindow wp263_window;
    QDockWidget *wp263_project = addDock(
        wp263_window, QStringLiteral("pelican.projectDock"), Qt::LeftDockWidgetArea);
    QDockWidget *wp263_outliner = addDock(
        wp263_window, QStringLiteral("pelican.outlinerDock"), Qt::LeftDockWidgetArea);
    wp263_window.tabifyDockWidget(wp263_project, wp263_outliner);
    addDock(wp263_window, QStringLiteral("pelican.inspectorDock"),
            Qt::RightDockWidgetArea);
    QDockWidget *wp263_output = addDock(
        wp263_window, QStringLiteral("pelican.outputDock"), Qt::BottomDockWidgetArea);
    QDockWidget *wp263_engine_log = addDock(
        wp263_window, QStringLiteral("pelican.engineLogDock"), Qt::BottomDockWidgetArea);
    wp263_window.tabifyDockWidget(wp263_output, wp263_engine_log);
    const QByteArray wp263_state =
        wp263_window.saveState(LayoutPresetManager::WindowStateVersion);
    REQUIRE_FALSE(wp263_state.isEmpty());

    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    LayoutPresetManager manager(directory.path());
    REQUIRE(manager.savePreset(
        QStringLiteral("WP249"), {QByteArrayLiteral("geometry"), wp249_state}));
    REQUIRE(manager.savePreset(
        QStringLiteral("WP263"), {QByteArrayLiteral("geometry"), wp263_state}));

    QMainWindow current_window;
    QDockWidget *project = addDock(
        current_window, QStringLiteral("pelican.projectDock"), Qt::LeftDockWidgetArea);
    QDockWidget *outliner = addDock(
        current_window, QStringLiteral("pelican.outlinerDock"), Qt::LeftDockWidgetArea);
    current_window.tabifyDockWidget(project, outliner);
    addDock(current_window, QStringLiteral("pelican.inspectorDock"), Qt::RightDockWidgetArea);
    QDockWidget *output = addDock(
        current_window, QStringLiteral("pelican.outputDock"), Qt::BottomDockWidgetArea);
    QDockWidget *engine_log = addDock(
        current_window, QStringLiteral("pelican.engineLogDock"), Qt::BottomDockWidgetArea);
    QDockWidget *frame_plan = addDock(
        current_window, QStringLiteral("pelican.framePlanDock"), Qt::BottomDockWidgetArea);
    current_window.tabifyDockWidget(output, engine_log);
    current_window.tabifyDockWidget(engine_log, frame_plan);

    bool default_applied = false;
    QString error;
    LayoutRestoreResult result = manager.restorePreset(
        QStringLiteral("WP249"),
        [&current_window](const LayoutSnapshot &snapshot) {
            return current_window.restoreState(
                snapshot.window_state, LayoutPresetManager::WindowStateVersion);
        },
        [&default_applied]() { default_applied = true; }, &error);

    REQUIRE(result == LayoutRestoreResult::Restored);
    REQUIRE_FALSE(default_applied);
    REQUIRE(error.isEmpty());
    REQUIRE(current_window.dockWidgetArea(engine_log) != Qt::NoDockWidgetArea);
    REQUIRE_FALSE(engine_log->isHidden());
    REQUIRE(current_window.dockWidgetArea(frame_plan) != Qt::NoDockWidgetArea);
    REQUIRE_FALSE(frame_plan->isHidden());

    default_applied = false;
    error.clear();
    result = manager.restorePreset(
        QStringLiteral("WP263"),
        [&current_window](const LayoutSnapshot &snapshot) {
            return current_window.restoreState(
                snapshot.window_state, LayoutPresetManager::WindowStateVersion);
        },
        [&default_applied]() { default_applied = true; }, &error);

    REQUIRE(result == LayoutRestoreResult::Restored);
    REQUIRE_FALSE(default_applied);
    REQUIRE(error.isEmpty());
    REQUIRE(current_window.dockWidgetArea(frame_plan) != Qt::NoDockWidgetArea);
    REQUIRE_FALSE(frame_plan->isHidden());
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
