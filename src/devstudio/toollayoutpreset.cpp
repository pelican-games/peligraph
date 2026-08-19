#include "toollayoutpreset.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <QSet>

#include <optional>
#include <stdexcept>
#include <utility>

namespace PelicanStudio {
namespace {

constexpr auto ToolLayoutFormat = "pelican.devstudio.tool_layout";
constexpr qint64 MaximumToolLayoutFileSize = 1024 * 1024;
constexpr qsizetype MaximumTabCount = 128;

enum class ReadStatus {
    Loaded,
    Missing,
    UnsupportedVersion,
    Invalid,
};

struct ReadResult {
    ReadStatus status = ReadStatus::Invalid;
    ToolLayoutSnapshot snapshot;
    QString name;
    QString error;
};

void setError(QString *destination, const QString &message) {
    if (destination != nullptr) {
        *destination = message;
    }
}

std::optional<QString> normalizedName(const QString &name) {
    const QString normalized = name.trimmed();
    if (normalized.isEmpty() || normalized.size() > 80) {
        return std::nullopt;
    }
    for (const QChar character : normalized) {
        if (character.isNull() || character.category() == QChar::Other_Control) {
            return std::nullopt;
        }
    }
    return normalized;
}

QString normalizedProjectKey(QString project_key) {
    project_key = QDir::cleanPath(
        QDir::fromNativeSeparators(project_key.trimmed()));
    if (project_key.isEmpty() || project_key == QStringLiteral(".")) {
        throw std::invalid_argument("tool layout project key must not be empty");
    }
    return project_key;
}

bool validSnapshot(const ToolLayoutSnapshot &snapshot) {
    if (snapshot.tab_order.isEmpty() ||
        snapshot.tab_order.size() > MaximumTabCount ||
        snapshot.logical_splitter_state.isEmpty()) {
        return false;
    }

    QSet<QString> tab_ids;
    for (const QString &tab_id : snapshot.tab_order) {
        const auto normalized = normalizedName(tab_id);
        if (!normalized || *normalized != tab_id || tab_ids.contains(tab_id)) {
            return false;
        }
        tab_ids.insert(tab_id);
    }
    return true;
}

bool decodeBase64(const QJsonValue &value, QByteArray &decoded) {
    if (!value.isString()) {
        return false;
    }
    const QByteArray encoded = value.toString().toLatin1();
    decoded = QByteArray::fromBase64(encoded);
    return !decoded.isEmpty() && decoded.toBase64() == encoded;
}

ReadResult readPresetFile(const QString &path, const QString &project_key,
                          const std::optional<QString> &expected_name) {
    QFile file(path);
    if (!file.exists()) {
        return {ReadStatus::Missing, {}, {},
                QStringLiteral("Tool layout preset does not exist.")};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Could not open tool layout preset: %1")
                    .arg(file.errorString())};
    }
    if (file.size() <= 0 || file.size() > MaximumToolLayoutFileSize) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Tool layout preset has an invalid size.")};
    }

    QJsonParseError parse_error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError ||
        !document.isObject()) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Tool layout preset is not valid JSON: %1")
                    .arg(parse_error.errorString())};
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() !=
        QString::fromLatin1(ToolLayoutFormat)) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral(
                    "Tool layout preset format is missing or invalid.")};
    }

    const QJsonValue version_value = root.value(QStringLiteral("version"));
    if (!version_value.isDouble()) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Tool layout preset version is required.")};
    }
    const int version = version_value.toInt(-1);
    if (version_value.toDouble() != version) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Tool layout preset version must be an integer.")};
    }
    if (version != ToolLayoutPresetManager::FileVersion) {
        return {ReadStatus::UnsupportedVersion, {}, {},
                QStringLiteral(
                    "Tool layout preset version is unsupported (%1; expected %2).")
                    .arg(version)
                    .arg(ToolLayoutPresetManager::FileVersion)};
    }

    const auto stored_name =
        normalizedName(root.value(QStringLiteral("name")).toString());
    if (!stored_name || (expected_name && *stored_name != *expected_name)) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral(
                    "Tool layout preset name is missing or invalid.")};
    }
    if (root.value(QStringLiteral("project_key")).toString() != project_key) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral(
                    "Tool layout preset belongs to a different project.")};
    }

    const QJsonValue tab_order_value =
        root.value(QStringLiteral("tab_order"));
    if (!tab_order_value.isArray()) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Tool layout tab order is missing or invalid.")};
    }

    ToolLayoutSnapshot snapshot;
    const QJsonArray tab_order = tab_order_value.toArray();
    snapshot.tab_order.reserve(tab_order.size());
    for (const QJsonValue &tab_id : tab_order) {
        if (!tab_id.isString()) {
            return {ReadStatus::Invalid, {}, {},
                    QStringLiteral(
                        "Tool layout tab order is missing or invalid.")};
        }
        snapshot.tab_order.push_back(tab_id.toString());
    }
    if (!decodeBase64(root.value(QStringLiteral("logical_splitter_state")),
                      snapshot.logical_splitter_state) ||
        !validSnapshot(snapshot)) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Tool layout state is missing or invalid.")};
    }

    return {ReadStatus::Loaded, std::move(snapshot), *stored_name, {}};
}

} // namespace

ToolLayoutPresetManager::ToolLayoutPresetManager(
    QString application_config_directory, QString project_key)
    : project_key_(normalizedProjectKey(std::move(project_key))) {
    application_config_directory =
        QDir::cleanPath(std::move(application_config_directory));
    if (application_config_directory.isEmpty() ||
        application_config_directory == QStringLiteral(".")) {
        throw std::invalid_argument(
            "tool layout application config directory must not be empty");
    }
    const QByteArray project_digest = QCryptographicHash::hash(
        project_key_.toUtf8(), QCryptographicHash::Sha256).toHex();
    storage_directory_ =
        QDir(application_config_directory)
            .filePath(QStringLiteral("tool-layouts/%1")
                          .arg(QString::fromLatin1(project_digest)));
}

bool ToolLayoutPresetManager::savePreset(
    const QString &name, const ToolLayoutSnapshot &snapshot,
    QString *error) const {
    const auto normalized_name = normalizedName(name);
    if (!normalized_name) {
        setError(
            error,
            QStringLiteral(
                "Tool layout name must contain 1 to 80 non-control characters."));
        return false;
    }
    if (!validSnapshot(snapshot)) {
        setError(error,
                 QStringLiteral(
                     "The tool did not provide a restorable layout state."));
        return false;
    }
    if (!QDir().mkpath(storage_directory_)) {
        setError(error,
                 QStringLiteral("Could not create tool layout directory: %1")
                     .arg(storage_directory_));
        return false;
    }

    QJsonArray tab_order;
    for (const QString &tab_id : snapshot.tab_order) {
        tab_order.push_back(tab_id);
    }
    const QJsonObject root{
        {QStringLiteral("format"), QString::fromLatin1(ToolLayoutFormat)},
        {QStringLiteral("version"), FileVersion},
        {QStringLiteral("name"), *normalized_name},
        {QStringLiteral("project_key"), project_key_},
        {QStringLiteral("tab_order"), tab_order},
        {QStringLiteral("logical_splitter_state"),
         QString::fromLatin1(
             snapshot.logical_splitter_state.toBase64())},
    };

    QSaveFile file(presetPath(*normalized_name));
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error,
                 QStringLiteral("Could not open tool layout preset for writing: %1")
                     .arg(file.errorString()));
        return false;
    }
    const QByteArray contents =
        QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size()) {
        setError(error,
                 QStringLiteral("Could not write tool layout preset: %1")
                     .arg(file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error,
                 QStringLiteral("Could not commit tool layout preset: %1")
                     .arg(file.errorString()));
        return false;
    }

    setError(error, {});
    return true;
}

ToolLayoutRestoreResult ToolLayoutPresetManager::restorePreset(
    const QString &name, const ApplyLayout &apply_layout,
    const ApplyDefault &apply_default, QString *error) const {
    const auto normalized_name = normalizedName(name);
    ReadResult read_result;
    if (!normalized_name) {
        read_result = {ReadStatus::Invalid, {}, {},
                       QStringLiteral("Tool layout name is invalid.")};
    } else {
        read_result = readPresetFile(presetPath(*normalized_name),
                                     project_key_, normalized_name);
    }

    if (read_result.status == ReadStatus::Loaded) {
        if (apply_layout && apply_layout(read_result.snapshot)) {
            setError(error, {});
            return ToolLayoutRestoreResult::Restored;
        }
        if (apply_default) {
            apply_default();
        }
        setError(
            error,
            QStringLiteral(
                "The tool rejected the saved layout state; the default tool layout was restored."));
        return ToolLayoutRestoreResult::DefaultRejectedState;
    }

    if (apply_default) {
        apply_default();
    }
    setError(error, read_result.error);
    switch (read_result.status) {
    case ReadStatus::Missing:
        return ToolLayoutRestoreResult::DefaultMissing;
    case ReadStatus::UnsupportedVersion:
        return ToolLayoutRestoreResult::DefaultUnsupportedVersion;
    case ReadStatus::Invalid:
        return ToolLayoutRestoreResult::DefaultInvalidPreset;
    case ReadStatus::Loaded:
        break;
    }
    return ToolLayoutRestoreResult::DefaultInvalidPreset;
}

QStringList ToolLayoutPresetManager::presetNames() const {
    QDir directory(storage_directory_);
    if (!directory.exists()) {
        return {};
    }

    QStringList names;
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.tool-layout.json")},
        QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &file : files) {
        const ReadResult result = readPresetFile(
            file.absoluteFilePath(), project_key_, std::nullopt);
        if (result.status == ReadStatus::Loaded &&
            presetPath(result.name) == file.absoluteFilePath()) {
            names.push_back(result.name);
        }
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool ToolLayoutPresetManager::deletePreset(const QString &name,
                                           QString *error) const {
    const auto normalized_name = normalizedName(name);
    if (!normalized_name) {
        setError(error, QStringLiteral("Tool layout name is invalid."));
        return false;
    }

    QFile file(presetPath(*normalized_name));
    if (!file.exists()) {
        setError(error,
                 QStringLiteral("Tool layout preset does not exist."));
        return false;
    }
    if (!file.remove()) {
        setError(error,
                 QStringLiteral("Could not delete tool layout preset: %1")
                     .arg(file.errorString()));
        return false;
    }

    setError(error, {});
    return true;
}

const QString &ToolLayoutPresetManager::projectKey() const {
    return project_key_;
}

const QString &ToolLayoutPresetManager::storageDirectory() const {
    return storage_directory_;
}

QString ToolLayoutPresetManager::presetPath(
    const QString &normalized_name) const {
    const QByteArray digest = QCryptographicHash::hash(
        normalized_name.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(storage_directory_)
        .absoluteFilePath(QString::fromLatin1(digest) +
                          QStringLiteral(".tool-layout.json"));
}

} // namespace PelicanStudio
