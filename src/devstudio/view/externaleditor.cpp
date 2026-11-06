#include "externaleditor.hpp"

#include <QCollator>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <climits>
#include <map>
#include <mutex>
#include <thread>

namespace PelicanStudio {
namespace {

QString clean(QString path) {
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString probePath(const EditorProbe &probe, const QString &name) {
    return probe.find_on_path ? clean(probe.find_on_path(name)) : QString{};
}

bool probeExists(const EditorProbe &probe, const QString &path) {
    return !path.isEmpty() && probe.file_exists && probe.file_exists(path);
}

QString environment(const EditorProbe &probe, const QString &name) {
    return probe.environment ? probe.environment(name) : QString{};
}

bool absoluteFor(const QString &path, EditorPlatform platform) {
    if (platform == EditorPlatform::windows) {
        static const QRegularExpression drive{QStringLiteral(R"(^[A-Za-z]:[/\\])")};
        return drive.match(path).hasMatch() || path.startsWith(QStringLiteral(R"(\\)"));
    }
    return path.startsWith(QLatin1Char('/'));
}

bool shellInterpreter(const QString &program) {
    const QString name = QFileInfo{program}.fileName().toLower();
    static const QStringList names{
        QStringLiteral("cmd.exe"), QStringLiteral("powershell.exe"),
        QStringLiteral("pwsh.exe"), QStringLiteral("wscript.exe"),
        QStringLiteral("cscript.exe"), QStringLiteral("bash.exe"),
        QStringLiteral("sh.exe"), QStringLiteral("cm" "d"),
        QStringLiteral("powershell"), QStringLiteral("pwsh"),
        QStringLiteral("bash"), QStringLiteral("s" "h")};
    return names.contains(name);
}

QString shellRefusal(const QString &program) {
    return QObject::tr("%1 needs a shell to start, which Studio doesn't use. "
                       "Choose the program itself in Edit > External Editor...")
        .arg(program);
}

QString missingRefusal(const QString &file, const QString &label) {
    return QObject::tr("Could not open %1: %2 was not found on this computer. "
                       "Change it in Edit > External Editor...")
        .arg(QFileInfo{file}.fileName(), label);
}

QStringList expandedLocations(const ExternalEditorPreset &preset, const EditorProbe &probe) {
    QStringList result;
    for (QString location : preset.windows_locations) {
        static const QRegularExpression variable{QStringLiteral("%([^%]+)%")};
        auto match = variable.match(location);
        while (match.hasMatch()) {
            location.replace(match.capturedStart(), match.capturedLength(),
                             environment(probe, match.captured(1)));
            match = variable.match(location);
        }
        if (!location.isEmpty()) result.push_back(clean(location));
    }
    return result;
}

QString lexicalJoin(const QString &directory, const QString &name) {
    if (directory.isEmpty() || directory.endsWith(QLatin1Char('/'))) return clean(directory + name);
    return clean(directory + QLatin1Char('/') + name);
}

QString lexicalParent(const QString &directory) {
    const int slash = directory.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) return {};
    if (slash == 0) return QStringLiteral("/");
    if (slash == 2 && directory.size() >= 3 && directory.at(1) == QLatin1Char(':'))
        return directory.left(3);
    return directory.left(slash);
}

QString launcherProgram(const ExternalEditorPreset &preset, const QString &launcher,
                        const EditorProbe &probe) {
    QString directory = launcher.left(launcher.lastIndexOf(QLatin1Char('/')));
    for (int parent = 0; parent <= 3 && !directory.isEmpty(); ++parent) {
        const QString candidate = lexicalJoin(directory, preset.windows_program);
        if (probeExists(probe, candidate)) return candidate;
        const QString parent_directory = lexicalParent(directory);
        if (parent_directory == directory) break;
        directory = parent_directory;
    }
    return {};
}

QString fromLocations(const ExternalEditorPreset &preset, const EditorProbe &probe) {
    QCollator collator;
    collator.setNumericMode(true);
    for (const QString &location : expandedLocations(preset, probe)) {
        const QStringList parts = location.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        QStringList directories{location.startsWith(QLatin1Char('/')) ? QStringLiteral("/")
                                                                     : QString{}};
        for (const QString &part : parts) {
            QStringList next;
            for (const QString &directory : std::as_const(directories)) {
                if (part.contains(QLatin1Char('*'))) {
                    QStringList matches = probe.directories
                                              ? probe.directories(directory, part)
                                              : QStringList{};
                    std::sort(matches.begin(), matches.end(),
                              [&collator](const QString &left, const QString &right) {
                                  return collator.compare(QFileInfo{left}.fileName(),
                                                          QFileInfo{right}.fileName()) > 0;
                              });
                    next.append(matches);
                } else {
                    next.push_back(lexicalJoin(directory, part));
                }
            }
            directories = std::move(next);
            if (directories.isEmpty()) break;
        }
        for (const QString &directory : std::as_const(directories)) {
            const QString candidate = lexicalJoin(directory, preset.windows_program);
            if (probeExists(probe, candidate)) return candidate;
        }
    }
    return {};
}

QString runVswhere(const EditorProbe &probe, bool prerelease) {
    if (!probe.run_vswhere) return {};
    QStringList arguments{QStringLiteral("-nologo"), QStringLiteral("-utf8"),
                          QStringLiteral("-latest")};
    if (prerelease) arguments.push_back(QStringLiteral("-prerelease"));
    arguments.append({QStringLiteral("-property"), QStringLiteral("productPath")});
    const QString output = probe.run_vswhere(arguments);
    const QStringList lines = output.split(QRegularExpression{QStringLiteral(R"(\r?\n)")},
                                           Qt::SkipEmptyParts);
    if (lines.isEmpty()) return {};
    const QString candidate = clean(lines.front().trimmed());
    if (QFileInfo{candidate}.fileName().compare(QStringLiteral("devenv.exe"),
                                                Qt::CaseInsensitive) != 0 ||
        !probeExists(probe, candidate))
        return {};
    return candidate;
}

} // namespace

EditorPlatform currentEditorPlatform() {
#ifdef Q_OS_WIN
    return EditorPlatform::windows;
#else
    return EditorPlatform::other;
#endif
}

ExternalEditorStore::ExternalEditorStore(QSettings &settings) : settings_(settings) {}

ExternalEditorSettings ExternalEditorStore::load() const {
    ExternalEditorSettings result;
    result.choice = settings_.value(QStringLiteral("external_editor/choice"), result.choice).toString();
    result.custom_program = settings_.value(QStringLiteral("external_editor/custom_program")).toString();
    result.custom_arguments = settings_.value(QStringLiteral("external_editor/custom_arguments")).toString();
    return result;
}

bool ExternalEditorStore::save(const ExternalEditorSettings &settings, QString *error) {
    const ExternalEditorSettings previous = load();
    const auto restore = [this, &previous] {
        settings_.setValue(QStringLiteral("external_editor/choice"), previous.choice);
        settings_.setValue(QStringLiteral("external_editor/custom_program"), previous.custom_program);
        settings_.setValue(QStringLiteral("external_editor/custom_arguments"), previous.custom_arguments);
    };
    settings_.setValue(QStringLiteral("external_editor/choice"), settings.choice);
    settings_.setValue(QStringLiteral("external_editor/custom_program"), settings.custom_program);
    settings_.setValue(QStringLiteral("external_editor/custom_arguments"), settings.custom_arguments);
    settings_.sync();
    if (settings_.status() != QSettings::NoError) {
        restore();
        if (error) *error = QObject::tr("The external editor settings could not be saved to %1.")
                                .arg(settings_.fileName());
        return false;
    }
    return true;
}

EditorProbe systemEditorProbe() {
    EditorProbe probe;
    probe.find_on_path = [](const QString &name) { return QStandardPaths::findExecutable(name); };
    probe.file_exists = [](const QString &path) { return QFileInfo::exists(path) && QFileInfo{path}.isFile(); };
    probe.environment = [](const QString &name) { return qEnvironmentVariable(name.toUtf8().constData()); };
    probe.directories = [](const QString &directory, const QString &pattern) {
        return QDir{directory}.entryList({pattern}, QDir::Dirs | QDir::NoDotAndDotDot,
                                         QDir::Name | QDir::Reversed);
    };
    probe.run_vswhere = [](const QStringList &arguments) {
        const QString installer = QDir::fromNativeSeparators(qEnvironmentVariable("ProgramFiles(x86)")) +
                                  QStringLiteral("/Microsoft Visual Studio/Installer/vswhere.exe");
        QProcess process;
        process.start(installer, arguments, QIODevice::ReadOnly);
        if (!process.waitForStarted(3000)) return QString{};
        if (!process.waitForFinished(3000)) {
            process.kill();
            process.waitForFinished();
            return QString{};
        }
        return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0
                   ? QString::fromUtf8(process.readAllStandardOutput())
                   : QString{};
    };
    return probe;
}

const std::vector<ExternalEditorPreset> &externalEditorPresets() {
    static const std::vector<ExternalEditorPreset> presets{
        {QStringLiteral("vscode"), QStringLiteral("Visual Studio Code"), QStringLiteral("Code.exe"),
         QStringLiteral("code.cmd"), QStringLiteral("code"),
         {QStringLiteral("%LOCALAPPDATA%/Programs/Microsoft VS Code"),
          QStringLiteral("%ProgramFiles%/Microsoft VS Code")}},
        {QStringLiteral("visual_studio"), QStringLiteral("Visual Studio"), QStringLiteral("devenv.exe"), {}, {}, {}, false},
        {QStringLiteral("cursor"), QStringLiteral("Cursor"), QStringLiteral("Cursor.exe"),
         QStringLiteral("cursor.cmd"), QStringLiteral("cursor"),
         {QStringLiteral("%LOCALAPPDATA%/Programs/cursor"), QStringLiteral("%ProgramFiles%/cursor")}},
        {QStringLiteral("sublime"), QStringLiteral("Sublime Text"), QStringLiteral("subl.exe"), {},
         QStringLiteral("subl"),
         {QStringLiteral("%ProgramFiles%/Sublime Text"), QStringLiteral("%ProgramFiles(x86)%/Sublime Text")}},
        {QStringLiteral("notepadpp"), QStringLiteral("Notepad++"), QStringLiteral("notepad++.exe"), {}, {},
         {QStringLiteral("%ProgramFiles%/Notepad++"), QStringLiteral("%ProgramFiles(x86)%/Notepad++")}},
        {QStringLiteral("clion"), QStringLiteral("CLion"), QStringLiteral("clion64.exe"), {},
         QStringLiteral("clion"),
         {QStringLiteral("%LOCALAPPDATA%/Programs/CLion*/bin"),
          QStringLiteral("%LOCALAPPDATA%/JetBrains/Toolbox/apps/CLion/ch-*/*/bin"),
          QStringLiteral("%ProgramFiles%/JetBrains/CLion*/bin")}},
        {QStringLiteral("rider"), QStringLiteral("Rider"), QStringLiteral("rider64.exe"), {},
         QStringLiteral("rider"),
         {QStringLiteral("%ProgramFiles%/JetBrains/JetBrains Rider*/bin"),
          QStringLiteral("%LOCALAPPDATA%/Programs/Rider*/bin"),
          QStringLiteral("%LOCALAPPDATA%/JetBrains/Toolbox/apps/Rider/ch-*/*/bin")}},
    };
    return presets;
}

const ExternalEditorPreset *externalEditorPreset(const QString &id) {
    const auto &presets = externalEditorPresets();
    const auto found = std::ranges::find_if(presets, [&id](const auto &preset) { return preset.id == id; });
    return found == presets.end() ? nullptr : &*found;
}

std::vector<const ExternalEditorPreset *> presetsFor(EditorPlatform platform) {
    std::vector<const ExternalEditorPreset *> result;
    for (const auto &preset : externalEditorPresets()) {
        const bool available = platform == EditorPlatform::windows
                                   ? !preset.windows_program.isEmpty()
                                   : !preset.other_program.isEmpty();
        if (available) result.push_back(&preset);
    }
    return result;
}

QString resolvePresetProgram(const ExternalEditorPreset &preset, EditorPlatform platform,
                             const EditorProbe &probe) {
    if (platform == EditorPlatform::other)
        return preset.other_program.isEmpty() ? QString{} : probePath(probe, preset.other_program);
    if (preset.id == QStringLiteral("visual_studio")) {
        QString found = runVswhere(probe, false);
        if (found.isEmpty()) found = runVswhere(probe, true);
        if (!found.isEmpty()) return found;
    }
    const QString on_path = probePath(probe, preset.windows_program);
    if (!on_path.isEmpty() && probeExists(probe, on_path)) return on_path;
    if (!preset.windows_launcher.isEmpty()) {
        const QString launcher = probePath(probe, preset.windows_launcher);
        const QString found = launcherProgram(preset, launcher, probe);
        if (!found.isEmpty()) return found;
    }
    return fromLocations(preset, probe);
}

struct ExternalEditorDiscovery::Impl {
    explicit Impl(EditorProbe value) : probe{std::move(value)} {}
    mutable std::mutex mutex;
    EditorProbe probe;
    std::map<QString, QString> cache;
};

ExternalEditorDiscovery::ExternalEditorDiscovery(EditorProbe probe)
    : impl_{std::make_shared<Impl>(std::move(probe))} {}
ExternalEditorDiscovery::~ExternalEditorDiscovery() = default;

QString ExternalEditorDiscovery::cached(const QString &id, EditorPlatform platform) const {
    std::lock_guard lock{impl_->mutex};
    return impl_->cache.at(QStringLiteral("%1:%2").arg(platform == EditorPlatform::windows ? 1 : 0).arg(id));
}

bool ExternalEditorDiscovery::hasCached(const QString &id, EditorPlatform platform) const {
    std::lock_guard lock{impl_->mutex};
    return impl_->cache.contains(QStringLiteral("%1:%2").arg(platform == EditorPlatform::windows ? 1 : 0).arg(id));
}

void ExternalEditorDiscovery::resolve(const ExternalEditorPreset &preset, EditorPlatform platform,
                                      QWidget *context,
                                      std::function<void(const QString &)> completed) {
    const QString key = QStringLiteral("%1:%2").arg(platform == EditorPlatform::windows ? 1 : 0).arg(preset.id);
    {
        std::lock_guard lock{impl_->mutex};
        if (const auto found = impl_->cache.find(key); found != impl_->cache.end()) {
            const QString program = found->second;
            QTimer::singleShot(0, context, [completed = std::move(completed), program] { completed(program); });
            return;
        }
    }
    const auto state = impl_;
    QPointer<QWidget> guard{context};
    std::thread([state, preset, platform, key, guard, completed = std::move(completed)]() mutable {
        const QString program = resolvePresetProgram(preset, platform, state->probe);
        {
            std::lock_guard lock{state->mutex};
            state->cache[key] = program;
        }
        if (guard)
            QTimer::singleShot(0, guard, [guard, completed = std::move(completed), program] {
                if (guard) completed(program);
            });
    }).detach();
}

ExpandedArguments expandEditorArguments(const QStringList &templates,
                                        const EditorPlaceholders &values) {
    ExpandedArguments result;
    static const QRegularExpression placeholder{QStringLiteral(R"(\{([A-Za-z_][A-Za-z0-9_]*)\})")};
    for (const QString &text : templates) {
        QString expanded;
        qsizetype copied = 0;
        auto matches = placeholder.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            expanded += text.mid(copied, match.capturedStart() - copied);
            const QString name = match.captured(1);
            if (name == QStringLiteral("file")) expanded += values.file;
            else if (name == QStringLiteral("line")) expanded += QString::number(values.line.value_or(1));
            else if (name == QStringLiteral("column")) expanded += QString::number(values.column.value_or(1));
            else if (name == QStringLiteral("project")) {
                if (values.project.isEmpty()) {
                    result.project_missing = true;
                    result.arguments.clear();
                    return result;
                }
                expanded += values.project;
            } else {
                result.unknown = name;
                result.arguments.clear();
                return result;
            }
            copied = match.capturedEnd();
        }
        expanded += text.mid(copied);
        result.arguments.push_back(expanded);
    }
    return result;
}

QString customEditorProblem(const QString &program, const QString &arguments,
                            EditorPlatform platform) {
    if (program.trimmed().isEmpty())
        return QObject::tr("Set the Custom editor's program.");
    const QString trimmed = program.trimmed();
    if ((platform == EditorPlatform::windows && !trimmed.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) ||
        shellInterpreter(trimmed))
        return shellRefusal(program);
    const QStringList split = QProcess::splitCommand(arguments);
    if (!split.contains(QStringLiteral("{file}")) &&
        !std::ranges::any_of(split, [](const QString &argument) { return argument.contains(QStringLiteral("{file}")); }))
        return QObject::tr("The Custom editor's arguments must contain {file}.");
    const ExpandedArguments expanded = expandEditorArguments(split, {QStringLiteral("x"), 1, 1, QStringLiteral("p")});
    if (!expanded.unknown.isEmpty())
        return QObject::tr("The Custom editor's arguments use the unknown placeholder {%1}. "
                           "Change them in Edit > External Editor...")
            .arg(expanded.unknown);
    return {};
}

QString nativeAbsolutePath(const std::filesystem::path &path) {
    return QDir::toNativeSeparators(
        QFileInfo(QString::fromStdU16String(path.u16string())).absoluteFilePath());
}

EditorCommand presetEditorCommand(const ExternalEditorPreset &preset, const QString &program,
                                  const SourceLocation &where, const QString &file) {
    QStringList arguments;
    if (preset.id == QStringLiteral("visual_studio")) {
        arguments = {QStringLiteral("/Edit"), file};
    } else if (!where.line) {
        arguments = {file};
    } else if (preset.id == QStringLiteral("vscode") || preset.id == QStringLiteral("cursor")) {
        arguments = {QStringLiteral("--goto"),
                     QStringLiteral("%1:%2:%3").arg(file).arg(*where.line).arg(where.column.value_or(1))};
    } else if (preset.id == QStringLiteral("sublime")) {
        arguments = {QStringLiteral("%1:%2:%3").arg(file).arg(*where.line).arg(where.column.value_or(1))};
    } else if (preset.id == QStringLiteral("notepadpp")) {
        arguments = {QStringLiteral("-n%1").arg(*where.line),
                     QStringLiteral("-c%1").arg(where.column.value_or(1)), file};
    } else {
        arguments = {QStringLiteral("--line"), QString::number(*where.line),
                     QStringLiteral("--column"), QString::number(where.column.value_or(1)), file};
    }
    return {program, arguments, preset.honours_line};
}

EditorCommandResult buildEditorCommand(const ExternalEditorSettings &settings,
                                       const SourceLocation &where, const QString &project,
                                       EditorPlatform platform, const EditorProbe &probe) {
    const QString file = nativeAbsolutePath(where.file);
    if (settings.choice == QStringLiteral("custom")) {
        const QString problem = customEditorProblem(settings.custom_program,
                                                    settings.custom_arguments, platform);
        if (settings.custom_program.trimmed().isEmpty())
            return {{}, QObject::tr("No program is set for the Custom editor. Set it in Edit > External Editor...")};
        if (!problem.isEmpty()) return {{}, problem};
        QString program = settings.custom_program.trimmed();
        if (!absoluteFor(program, platform)) program = probePath(probe, program);
        if (program.isEmpty() || !probeExists(probe, program))
            return {{}, missingRefusal(file, QObject::tr("Custom"))};
        const ExpandedArguments expanded = expandEditorArguments(
            QProcess::splitCommand(settings.custom_arguments),
            {file, where.line, where.column, project});
        if (!expanded.unknown.isEmpty())
            return {{}, QObject::tr("The Custom editor's arguments use the unknown placeholder {%1}. "
                                    "Change them in Edit > External Editor...").arg(expanded.unknown)};
        if (expanded.project_missing)
            return {{}, QObject::tr("The Custom editor's arguments use {project}, and no project is open.")};
        return {EditorCommand{program, expanded.arguments}, {}};
    }

    const auto *preset = externalEditorPreset(settings.choice);
    if (!preset)
        return {{}, QObject::tr("The saved editor choice %1 isn't known. Change it in Edit > External Editor...")
                        .arg(settings.choice)};
    const QString program = resolvePresetProgram(*preset, platform, probe);
    if (program.isEmpty()) return {{}, missingRefusal(file, preset->label)};

    return {presetEditorCommand(*preset, program, where, file), {}};
}

SourceOpenBackends defaultSourceOpenBackends() {
    SourceOpenBackends backends;
    backends.platform = currentEditorPlatform();
    backends.probe = systemEditorProbe();
    backends.system_default = [](const SourceLocation &where) {
        return QDesktopServices::openUrl(
            QUrl::fromLocalFile(QString::fromStdU16String(where.file.u16string())));
    };
    backends.start_process = [](const QString &program, const QStringList &arguments) {
        QProcess process;
        process.setProgram(program);
        process.setArguments(arguments);
        process.setWorkingDirectory(QFileInfo{program}.absolutePath());
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("ELECTRON_RUN_AS_NODE"));
        process.setProcessEnvironment(environment);
        return process.startDetached(nullptr);
    };
    return backends;
}

SourceOpenResult openSourceFileWith(const ExternalEditorSettings &settings,
                                    const SourceLocation &where, const QString &project,
                                    const SourceOpenBackends &backends) {
    SourceLocation normalized = where;
    normalized.file = std::filesystem::path{nativeAbsolutePath(where.file).toStdU16String()};
    const QString file = QString::fromStdU16String(normalized.file.u16string());
    const QString name = QFileInfo{file}.fileName();
    if (settings.choice == QStringLiteral("system")) {
        if (!backends.system_default || !backends.system_default(normalized))
            return {false, QObject::tr("Could not open %1: this computer has no program for this kind of file. "
                                       "Choose an editor in Edit > External Editor...").arg(name)};
        if (normalized.line)
            return {true, QObject::tr("Open requested for %1 with the system's default program; "
                                      "it can't be told the line: go to line %2.")
                              .arg(name).arg(*normalized.line)};
        return {true, {}};
    }

    EditorCommandResult built;
    if (!backends.resolved_program.isEmpty()) {
        const auto *preset = externalEditorPreset(settings.choice);
        if (!preset)
            return {false, QObject::tr("The saved editor choice %1 isn't known. Change it in Edit > External Editor...")
                               .arg(settings.choice)};
        built = {presetEditorCommand(*preset, backends.resolved_program, normalized, file), {}};
    } else {
        built = buildEditorCommand(settings, normalized, project, backends.platform, backends.probe);
    }
    if (!built.command) return {false, built.refusal};
    if (!backends.start_process || !backends.start_process(built.command->program, built.command->arguments))
        return {false, QObject::tr("Could not start %2 to open %1. Change it in Edit > External Editor...")
                           .arg(name, built.command->program)};
    if (normalized.line && !built.command->honours_line)
        return {true, QObject::tr("Open requested for %1 in Visual Studio; this command-line integration "
                                  "cannot reliably go to a line in a running instance: go to line %2.")
                          .arg(name).arg(*normalized.line)};
    return {true, {}};
}

std::optional<SourceLocation> projectSourceLocation(const QString &root,
                                                    const QString &document,
                                                    std::int64_t line,
                                                    std::int64_t column) {
    if (root.isEmpty() || document.isEmpty() || document.startsWith(QStringLiteral("engine://"))) return std::nullopt;
    static const QRegularExpression windowsAbsolute{QStringLiteral(R"(^([A-Za-z]:[/\\]|[/\\]{2}))")};
    if (QDir::isAbsolutePath(document) || windowsAbsolute.match(document).hasMatch()) return std::nullopt;

    std::error_code error;
    const auto root_path = std::filesystem::weakly_canonical(
        std::filesystem::path{root.toStdU16String()}, error);
    if (error || !std::filesystem::is_directory(root_path, error) || error) return std::nullopt;
    const auto candidate = std::filesystem::weakly_canonical(
        root_path / std::filesystem::path{document.toStdU16String()}, error);
    if (error || !std::filesystem::is_regular_file(candidate, error) || error) return std::nullopt;
    const auto relative = candidate.lexically_relative(root_path);
    if (relative.empty()) return std::nullopt;
    for (const auto &part : relative)
        if (part == std::filesystem::path{".."}) return std::nullopt;

    static const std::vector<std::u16string> extensions{
        u".c", u".cc", u".cpp", u".cxx", u".h", u".hh", u".hpp", u".hxx",
        u".inc", u".inl", u".frag", u".vert", u".geom", u".comp", u".tesc",
        u".tese", u".glsl", u".hlsl", u".slang", u".wgsl", u".rgen", u".rmiss",
        u".rchit", u".rahit", u".rint", u".rcall", u".shader", u".json", u".yaml",
        u".yml", u".xml", u".txt", u".md", u".cmake"};
    QString extension = QString::fromStdU16String(candidate.extension().u16string()).toLower();
    const bool supported = std::ranges::any_of(extensions, [&extension](const auto &value) {
        return extension == QString::fromStdU16String(value);
    });
    if (!supported) return std::nullopt;

    SourceLocation result{candidate};
    if (line > 0 && line <= INT_MAX) result.line = static_cast<int>(line);
    if (column > 0 && column <= INT_MAX) result.column = static_cast<int>(column);
    return result;
}

} // namespace PelicanStudio
