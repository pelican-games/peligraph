#include "layoutpreset.hpp"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

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
