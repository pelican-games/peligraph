#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace PelicanStudio {

struct LayoutSnapshot {
    QByteArray geometry;
    QByteArray window_state;

    bool operator==(const LayoutSnapshot &) const = default;
};

enum class LayoutRestoreResult {
    Restored,
    DefaultMissing,
    DefaultUnsupportedVersion,
    DefaultInvalidPreset,
    DefaultRejectedState,
};

class LayoutPresetManager {
  public:
    static constexpr int FileVersion = 1;
    static constexpr int WindowStateVersion = 1;

    using ApplyLayout = std::function<bool(const LayoutSnapshot &)>;
    using ApplyDefault = std::function<void()>;

    explicit LayoutPresetManager(QString storage_directory);

    bool savePreset(const QString &name, const LayoutSnapshot &snapshot, QString *error = nullptr) const;
    LayoutRestoreResult restorePreset(
        const QString &name,
        const ApplyLayout &apply_layout,
        const ApplyDefault &apply_default,
        QString *error = nullptr) const;
    QStringList presetNames() const;
    bool deletePreset(const QString &name, QString *error = nullptr) const;

    const QString &storageDirectory() const;

  private:
    QString storage_directory_;

    QString presetPath(const QString &normalized_name) const;
};

} // namespace PelicanStudio
