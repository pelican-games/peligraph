#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QSettings;
class QWidget;

namespace PelicanStudio {

struct SourceLocation {
    std::filesystem::path file;
    std::optional<int> line;
    std::optional<int> column;
};

enum class EditorPlatform { windows, other };
EditorPlatform currentEditorPlatform();

struct ExternalEditorSettings {
    QString choice = QStringLiteral("system");
    QString custom_program;
    QString custom_arguments;
    friend bool operator==(const ExternalEditorSettings &,
                           const ExternalEditorSettings &) = default;
};

class ExternalEditorStore {
  public:
    explicit ExternalEditorStore(QSettings &settings);
    ExternalEditorSettings load() const;
    bool save(const ExternalEditorSettings &settings, QString *error);

  private:
    QSettings &settings_;
};

struct EditorProbe {
    std::function<QString(const QString &name)> find_on_path;
    std::function<bool(const QString &path)> file_exists;
    std::function<QString(const QString &name)> environment;
    std::function<QStringList(const QString &directory, const QString &pattern)> directories;
    std::function<QString(const QStringList &arguments)> run_vswhere;
};
EditorProbe systemEditorProbe();

struct ExternalEditorPreset {
    QString id;
    QString label;
    QString windows_program;
    QString windows_launcher;
    QString other_program;
    QStringList windows_locations;
    bool honours_line = true;
};
const std::vector<ExternalEditorPreset> &externalEditorPresets();
const ExternalEditorPreset *externalEditorPreset(const QString &id);
std::vector<const ExternalEditorPreset *> presetsFor(EditorPlatform platform);
QString resolvePresetProgram(const ExternalEditorPreset &, EditorPlatform, const EditorProbe &);

class ExternalEditorDiscovery : public std::enable_shared_from_this<ExternalEditorDiscovery> {
  public:
    explicit ExternalEditorDiscovery(EditorProbe probe);
    ~ExternalEditorDiscovery();
    QString cached(const QString &id, EditorPlatform platform) const;
    bool hasCached(const QString &id, EditorPlatform platform) const;
    void resolve(const ExternalEditorPreset &preset, EditorPlatform platform,
                 QWidget *context, std::function<void(const QString &)> completed);

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

struct EditorPlaceholders {
    QString file;
    std::optional<int> line;
    std::optional<int> column;
    QString project;
};
struct ExpandedArguments {
    QStringList arguments;
    QString unknown;
    bool project_missing = false;
};
ExpandedArguments expandEditorArguments(const QStringList &templates,
                                        const EditorPlaceholders &values);

QString customEditorProblem(const QString &program, const QString &arguments,
                            EditorPlatform platform);
QString nativeAbsolutePath(const std::filesystem::path &path);

struct EditorCommand {
    QString program;
    QStringList arguments;
    bool honours_line = true;
};
struct EditorCommandResult {
    std::optional<EditorCommand> command;
    QString refusal;
};
EditorCommandResult buildEditorCommand(const ExternalEditorSettings &,
                                       const SourceLocation &, const QString &project,
                                       EditorPlatform, const EditorProbe &);

struct SourceOpenBackends {
    EditorPlatform platform = EditorPlatform::other;
    EditorProbe probe;
    std::function<bool(const SourceLocation &)> system_default;
    std::function<bool(const QString &program, const QStringList &arguments)> start_process;
    QString resolved_program;
};
SourceOpenBackends defaultSourceOpenBackends();

struct SourceOpenResult {
    bool opened = false;
    QString status;
};
SourceOpenResult openSourceFileWith(const ExternalEditorSettings &,
                                    const SourceLocation &, const QString &project,
                                    const SourceOpenBackends &);

std::optional<SourceLocation> projectSourceLocation(const QString &root,
                                                    const QString &document,
                                                    std::int64_t line,
                                                    std::int64_t column);

} // namespace PelicanStudio
