#include "layoutpreset.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

#include <optional>
#include <utility>

namespace PelicanStudio {
namespace {

constexpr auto LayoutFormat = "pelican.devstudio.layout";
constexpr qint64 MaximumLayoutFileSize = 16 * 1024 * 1024;

enum class ReadStatus {
    Loaded,
    Missing,
    UnsupportedVersion,
    Invalid,
};

struct ReadResult {
    ReadStatus status = ReadStatus::Invalid;
    LayoutSnapshot snapshot;
    QString name;
    QString error;
};

void setError(QString *destination, const QString &message) {
    if (destination != nullptr) {
        *destination = message;
    }
}

std::optional<QString> normalizedPresetName(const QString &name) {
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

bool decodeBase64(const QJsonValue &value, QByteArray &decoded) {
    if (!value.isString()) {
        return false;
    }

    const QByteArray encoded = value.toString().toLatin1();
    decoded = QByteArray::fromBase64(encoded);
    return !decoded.isEmpty() && decoded.toBase64() == encoded;
}

ReadResult readPresetFile(const QString &path, const std::optional<QString> &expected_name) {
    QFile file(path);
    if (!file.exists()) {
        return {ReadStatus::Missing, {}, {}, QStringLiteral("Layout preset does not exist.")};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Could not open layout preset: %1").arg(file.errorString())};
    }
    if (file.size() <= 0 || file.size() > MaximumLayoutFileSize) {
        return {ReadStatus::Invalid, {}, {}, QStringLiteral("Layout preset has an invalid size.")};
    }

    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return {ReadStatus::Invalid, {}, {},
                QStringLiteral("Layout preset is not valid JSON: %1").arg(parse_error.errorString())};
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("format")).toString() != QString::fromLatin1(LayoutFormat)) {
        return {ReadStatus::Invalid, {}, {}, QStringLiteral("Layout preset format is missing or invalid.")};
    }

    const QJsonValue file_version_value = root.value(QStringLiteral("version"));
    const QJsonValue state_version_value = root.value(QStringLiteral("main_window_state_version"));
    if (!file_version_value.isDouble() || !state_version_value.isDouble()) {
        return {ReadStatus::Invalid, {}, {}, QStringLiteral("Layout preset version fields are required.")};
    }

    const int file_version = file_version_value.toInt(-1);
    const int state_version = state_version_value.toInt(-1);
    if (file_version_value.toDouble() != file_version || state_version_value.toDouble() != state_version) {
        return {ReadStatus::Invalid, {}, {}, QStringLiteral("Layout preset versions must be integers.")};
    }
    if (file_version != LayoutPresetManager::FileVersion ||
        state_version != LayoutPresetManager::WindowStateVersion) {
        return {ReadStatus::UnsupportedVersion, {}, {},
                QStringLiteral("Layout preset version is unsupported (file %1, state %2; expected %3 and %4).")
                    .arg(file_version)
                    .arg(state_version)
                    .arg(LayoutPresetManager::FileVersion)
                    .arg(LayoutPresetManager::WindowStateVersion)};
    }

    const auto stored_name = normalizedPresetName(root.value(QStringLiteral("name")).toString());
    if (!stored_name || (expected_name && *stored_name != *expected_name)) {
        return {ReadStatus::Invalid, {}, {}, QStringLiteral("Layout preset name is missing or invalid.")};
    }

    LayoutSnapshot snapshot;
    if (!decodeBase64(root.value(QStringLiteral("geometry")), snapshot.geometry) ||
        !decodeBase64(root.value(QStringLiteral("window_state")), snapshot.window_state)) {
        return {ReadStatus::Invalid, {}, {}, QStringLiteral("Layout preset state is missing or invalid.")};
    }

    return {ReadStatus::Loaded, std::move(snapshot), *stored_name, {}};
}

} // namespace

LayoutPresetManager::LayoutPresetManager(QString storage_directory)
    : storage_directory_(QDir::cleanPath(std::move(storage_directory))) {}

bool LayoutPresetManager::savePreset(
    const QString &name, const LayoutSnapshot &snapshot, QString *error) const {
    const auto normalized_name = normalizedPresetName(name);
    if (!normalized_name) {
        setError(error, QStringLiteral("Layout name must contain 1 to 80 non-control characters."));
        return false;
    }
    if (snapshot.geometry.isEmpty() || snapshot.window_state.isEmpty()) {
        setError(error, QStringLiteral("The current window did not provide a restorable layout state."));
        return false;
    }
    if (!QDir().mkpath(storage_directory_)) {
        setError(error, QStringLiteral("Could not create layout directory: %1").arg(storage_directory_));
        return false;
    }

    const QJsonObject root{
        {QStringLiteral("format"), QString::fromLatin1(LayoutFormat)},
        {QStringLiteral("version"), FileVersion},
        {QStringLiteral("main_window_state_version"), WindowStateVersion},
        {QStringLiteral("name"), *normalized_name},
        {QStringLiteral("geometry"), QString::fromLatin1(snapshot.geometry.toBase64())},
        {QStringLiteral("window_state"), QString::fromLatin1(snapshot.window_state.toBase64())},
    };

    QSaveFile file(presetPath(*normalized_name));
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QStringLiteral("Could not open layout preset for writing: %1").arg(file.errorString()));
        return false;
    }
    const QByteArray contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(contents) != contents.size()) {
        setError(error, QStringLiteral("Could not write layout preset: %1").arg(file.errorString()));
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        setError(error, QStringLiteral("Could not commit layout preset: %1").arg(file.errorString()));
        return false;
    }

    setError(error, {});
    return true;
}

LayoutRestoreResult LayoutPresetManager::restorePreset(
    const QString &name,
    const ApplyLayout &apply_layout,
    const ApplyDefault &apply_default,
    QString *error) const {
    const auto normalized_name = normalizedPresetName(name);
    ReadResult read_result;
    if (!normalized_name) {
        read_result = {ReadStatus::Invalid, {}, {}, QStringLiteral("Layout name is invalid.")};
    } else {
        read_result = readPresetFile(presetPath(*normalized_name), normalized_name);
    }

    if (read_result.status == ReadStatus::Loaded) {
        if (apply_layout && apply_layout(read_result.snapshot)) {
            setError(error, {});
            return LayoutRestoreResult::Restored;
        }
        if (apply_default) {
            apply_default();
        }
        setError(error, QStringLiteral("Qt rejected the saved layout state; the default layout was restored."));
        return LayoutRestoreResult::DefaultRejectedState;
    }

    if (apply_default) {
        apply_default();
    }
    setError(error, read_result.error);
    switch (read_result.status) {
    case ReadStatus::Missing:
        return LayoutRestoreResult::DefaultMissing;
    case ReadStatus::UnsupportedVersion:
        return LayoutRestoreResult::DefaultUnsupportedVersion;
    case ReadStatus::Invalid:
        return LayoutRestoreResult::DefaultInvalidPreset;
    case ReadStatus::Loaded:
        break;
    }
    return LayoutRestoreResult::DefaultInvalidPreset;
}

QStringList LayoutPresetManager::presetNames() const {
    QDir directory(storage_directory_);
    if (!directory.exists()) {
        return {};
    }

    QStringList names;
    const QFileInfoList files = directory.entryInfoList(
        {QStringLiteral("*.layout.json")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &file : files) {
        const ReadResult result = readPresetFile(file.absoluteFilePath(), std::nullopt);
        if (result.status == ReadStatus::Loaded && presetPath(result.name) == file.absoluteFilePath()) {
            names.push_back(result.name);
        }
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

bool LayoutPresetManager::deletePreset(const QString &name, QString *error) const {
    const auto normalized_name = normalizedPresetName(name);
    if (!normalized_name) {
        setError(error, QStringLiteral("Layout name is invalid."));
        return false;
    }

    QFile file(presetPath(*normalized_name));
    if (!file.exists()) {
        setError(error, QStringLiteral("Layout preset does not exist."));
        return false;
    }
    if (!file.remove()) {
        setError(error, QStringLiteral("Could not delete layout preset: %1").arg(file.errorString()));
        return false;
    }

    setError(error, {});
    return true;
}

const QString &LayoutPresetManager::storageDirectory() const {
    return storage_directory_;
}

QString LayoutPresetManager::presetPath(const QString &normalized_name) const {
    const QByteArray digest = QCryptographicHash::hash(normalized_name.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(storage_directory_)
        .absoluteFilePath(QString::fromLatin1(digest) + QStringLiteral(".layout.json"));
}

} // namespace PelicanStudio
