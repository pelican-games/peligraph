#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

namespace PelicanStudio {

struct ToolLayoutSnapshot {
    QStringList tab_order;
    QByteArray logical_splitter_state;

    bool operator==(const ToolLayoutSnapshot &) const = default;
};

enum class ToolLayoutRestoreResult {
    Restored,
    DefaultMissing,
    DefaultUnsupportedVersion,
    DefaultInvalidPreset,
    DefaultRejectedState,
};

class ToolLayoutPresetManager {
  public:
    static constexpr int FileVersion = 1;

    using ApplyLayout = std::function<bool(const ToolLayoutSnapshot &)>;
    using ApplyDefault = std::function<void()>;

    ToolLayoutPresetManager(QString application_config_directory,
                            QString project_key);

    bool savePreset(const QString &name, const ToolLayoutSnapshot &snapshot,
                    QString *error = nullptr) const;
    ToolLayoutRestoreResult restorePreset(
        const QString &name, const ApplyLayout &apply_layout,
        const ApplyDefault &apply_default, QString *error = nullptr) const;
    QStringList presetNames() const;
    bool deletePreset(const QString &name, QString *error = nullptr) const;

    const QString &projectKey() const;
    const QString &storageDirectory() const;

  private:
    QString project_key_;
    QString storage_directory_;

    QString presetPath(const QString &normalized_name) const;
};

} // namespace PelicanStudio
