#include "view/externaleditor.hpp"
#include "view/externaleditordialog.hpp"
#include "view/frameplanwidget.hpp"
#include "view/fullscreenpasswidget.hpp"
#include "view/mainwindow.hpp"
#include "view/shaderdiagnostics.hpp"

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <map>
#include <set>

using namespace PelicanStudio;

namespace {
QApplication &application() {
    static int argc = 1;
    static char name[] = "devstudio_external_editor_test";
    static char *argv[] = {name, nullptr};
    static QApplication app{argc, argv};
    return app;
}

struct FakeProbe {
    EditorProbe probe;
    std::map<QString, QString> path;
    std::set<QString> files;
    std::map<QString, QString> env;
    QStringList directories;
    QStringList vswhere_output;
    std::vector<QStringList> vswhere_calls;

    FakeProbe() {
        probe.find_on_path = [this](const QString &name) {
            const auto found = path.find(name);
            return found == path.end() ? QString{} : found->second;
        };
        probe.file_exists = [this](const QString &value) { return files.contains(value); };
        probe.environment = [this](const QString &name) {
            const auto found = env.find(name);
            return found == env.end() ? QString{} : found->second;
        };
        probe.directories = [this](const QString &, const QString &) { return directories; };
        probe.run_vswhere = [this](const QStringList &arguments) {
            vswhere_calls.push_back(arguments);
            const int index = static_cast<int>(vswhere_calls.size()) - 1;
            if (vswhere_output.isEmpty()) return QString{};
            return vswhere_output[std::min<int>(index, vswhere_output.size() - 1)];
        };
    }
};

std::filesystem::path fs(const QString &value) {
    return std::filesystem::path{value.toStdU16String()};
}

SourceOpenBackends recordingBackends(std::vector<EditorCommand> &started,
                                     std::vector<SourceLocation> &system) {
    SourceOpenBackends backends;
    backends.platform = EditorPlatform::other;
    backends.system_default = [&system](const SourceLocation &where) {
        system.push_back(where);
        return true;
    };
    backends.start_process = [&started](const QString &program, const QStringList &arguments) {
        started.push_back({program, arguments});
        return true;
    };
    return backends;
}

template <typename T> T *childOfType(QObject &root) {
    for (QObject *child : root.findChildren<QObject *>())
        if (auto *found = dynamic_cast<T *>(child)) return found;
    return nullptr;
}
}

TEST_CASE("ED-1 expansion is literal and single pass", "[devstudio][ed1]") {
    struct Row {
        QStringList templates;
        QString file;
        std::optional<int> line;
        std::optional<int> column;
        QString project;
        QStringList expected;
        QString unknown;
        bool project_missing = false;
    };
    const QString japanese = QString::fromUtf16(u"シェーダ.frag");
    const QString metacharacters = QStringLiteral("a&b|c;d$(e)%PATH%^f.frag");
    const std::vector<Row> rows{
        {{QStringLiteral("{file}")}, QStringLiteral("C:/a b/x.frag"), {}, {}, {}, {QStringLiteral("C:/a b/x.frag")}},
        {{QStringLiteral("{file}:{line}:{column}")}, QStringLiteral("C:/a b/x.frag"), 7, 3, {}, {QStringLiteral("C:/a b/x.frag:7:3")}},
        {{QStringLiteral("{file}:{line}:{column}")}, QStringLiteral("x"), {}, {}, {}, {QStringLiteral("x:1:1")}},
        {{QStringLiteral("{file}:{line}:{column}")}, QStringLiteral("x"), 7, {}, {}, {QStringLiteral("x:7:1")}},
        {{QStringLiteral("+{line}")}, QStringLiteral("x"), 7, {}, {}, {QStringLiteral("+7")}},
        {{QStringLiteral("-n{line}"), QStringLiteral("-c{column}"), QStringLiteral("{file}")}, QStringLiteral("x"), 7, 3, {}, {QStringLiteral("-n7"), QStringLiteral("-c3"), QStringLiteral("x")}},
        {{QStringLiteral("{file}")}, QStringLiteral("a\"b.frag"), {}, {}, {}, {QStringLiteral("a\"b.frag")}},
        {{QStringLiteral("{file}")}, japanese, {}, {}, {}, {japanese}},
        {{QStringLiteral("{file}")}, metacharacters, {}, {}, {}, {metacharacters}},
        {{QStringLiteral("{file}")}, QStringLiteral("x --evil y.frag"), {}, {}, {}, {QStringLiteral("x --evil y.frag")}},
        {{QStringLiteral("{file}")}, QStringLiteral("{line}.frag"), 7, {}, {}, {QStringLiteral("{line}.frag")}},
        {{QStringLiteral("{foo}")}, QStringLiteral("x"), {}, {}, {}, {}, QStringLiteral("foo")},
        {{QStringLiteral("{File}")}, QStringLiteral("x"), {}, {}, {}, {}, QStringLiteral("File")},
        {{QStringLiteral("{"), QStringLiteral("}"), QStringLiteral("{1}")}, QStringLiteral("x"), {}, {}, {}, {QStringLiteral("{"), QStringLiteral("}"), QStringLiteral("{1}")}},
        {{QStringLiteral("{project}")}, QStringLiteral("x"), {}, {}, QStringLiteral("C:/p q"), {QStringLiteral("C:/p q")}},
        {{QStringLiteral("{project}")}, QStringLiteral("x"), {}, {}, {}, {}, {}, true},
    };
    for (const Row &row : rows) {
        INFO(row.templates.join(QStringLiteral(" ")).toStdString());
        const auto actual = expandEditorArguments(
            row.templates, {row.file, row.line, row.column, row.project});
        CHECK(actual.arguments == row.expected);
        CHECK(actual.unknown == row.unknown);
        CHECK(actual.project_missing == row.project_missing);
    }
}

TEST_CASE("ED-1 Custom splits before expansion and validates fields", "[devstudio][ed1]") {
    const auto split = QProcess::splitCommand(QStringLiteral("--x \"{file}\"  --y"));
    CHECK(expandEditorArguments(split, {QStringLiteral("a b.frag")}).arguments ==
          QStringList{QStringLiteral("--x"), QStringLiteral("a b.frag"), QStringLiteral("--y")});
    CHECK(expandEditorArguments(QProcess::splitCommand(QStringLiteral("{file}")),
                                {QStringLiteral("a b.frag")}).arguments ==
          QStringList{QStringLiteral("a b.frag")});
    CHECK(expandEditorArguments(QProcess::splitCommand(QStringLiteral("--x {file} --y")),
                                {QStringLiteral("a b.frag")}).arguments ==
          QStringList{QStringLiteral("--x"), QStringLiteral("a b.frag"), QStringLiteral("--y")});
    FakeProbe fake;
    fake.files.insert(QStringLiteral("/opt/editor"));
    ExternalEditorSettings settings;
    settings.choice = QStringLiteral("custom");
    settings.custom_program = QStringLiteral("/opt/editor");
    settings.custom_arguments = QStringLiteral("--x {file} --y");
    const auto built = buildEditorCommand(settings, SourceLocation{fs(QStringLiteral("a b.frag"))},
                                          {}, EditorPlatform::other, fake.probe);
    REQUIRE(built.command.has_value());
    CHECK(built.command->arguments ==
          QStringList{QStringLiteral("--x"), nativeAbsolutePath(fs(QStringLiteral("a b.frag"))),
                      QStringLiteral("--y")});
    CHECK(!customEditorProblem({}, QStringLiteral("{file}"), EditorPlatform::other).isEmpty());
    CHECK(customEditorProblem(QStringLiteral("x.cmd"), QStringLiteral("{file}"),
                              EditorPlatform::windows).contains(QStringLiteral("needs a shell")));
    CHECK(customEditorProblem(QStringLiteral("x"), QStringLiteral("--goto {line}"),
                              EditorPlatform::other).contains(QStringLiteral("{file}")));
    CHECK(customEditorProblem(QStringLiteral("x"), QStringLiteral("{file} {unknown}"),
                              EditorPlatform::other).contains(QStringLiteral("{unknown}")));
    CHECK(customEditorProblem(QStringLiteral("cmd.exe"), QStringLiteral("{file}"),
                              EditorPlatform::other).contains(QStringLiteral("needs a shell")));
    CHECK(customEditorProblem(QStringLiteral("gvim"), QStringLiteral("+{line} {file}"),
                              EditorPlatform::other).isEmpty());
}

TEST_CASE("ED-1 every preset uses its documented arguments", "[devstudio][ed1]") {
    const SourceLocation where{fs(QStringLiteral("a b/x.frag")), 7, 3};
    const QString file = nativeAbsolutePath(where.file);
    for (const EditorPlatform platform : {EditorPlatform::windows, EditorPlatform::other}) {
        for (const auto *preset : presetsFor(platform)) {
            INFO(preset->id.toStdString());
            FakeProbe fake;
            QString program;
            if (preset->id == QStringLiteral("visual_studio")) {
                program = QStringLiteral("C:/VS/Common7/IDE/devenv.exe");
                fake.files.insert(program);
                fake.vswhere_output.push_back(program + QStringLiteral("\r\n"));
            } else if (platform == EditorPlatform::windows) {
                program = QStringLiteral("C:/Editors/") + preset->windows_program;
                fake.path[preset->windows_program] = program;
                fake.files.insert(program);
            } else {
                program = QStringLiteral("/opt/editors/") + preset->other_program;
                fake.path[preset->other_program] = program;
                fake.files.insert(program);
            }
            ExternalEditorSettings settings;
            settings.choice = preset->id;
            const auto with_line = buildEditorCommand(settings, where, {}, platform, fake.probe);
            REQUIRE(with_line.command.has_value());
            CHECK(with_line.command->program == program);
            CHECK(with_line.command->honours_line == preset->honours_line);
            if (preset->id == QStringLiteral("visual_studio")) {
                CHECK(with_line.command->arguments == QStringList{QStringLiteral("/Edit"), file});
                for (const QString &argument : with_line.command->arguments) {
                    CHECK(!argument.contains(QStringLiteral("Command")));
                    CHECK(!argument.contains(QStringLiteral("GoTo")));
                }
            } else if (preset->id == QStringLiteral("vscode") || preset->id == QStringLiteral("cursor")) {
                CHECK(with_line.command->arguments ==
                      QStringList{QStringLiteral("--goto"), file + QStringLiteral(":7:3")});
            } else if (preset->id == QStringLiteral("sublime")) {
                CHECK(with_line.command->arguments == QStringList{file + QStringLiteral(":7:3")});
            } else if (preset->id == QStringLiteral("notepadpp")) {
                CHECK(with_line.command->arguments ==
                      QStringList{QStringLiteral("-n7"), QStringLiteral("-c3"), file});
            } else {
                CHECK(with_line.command->arguments ==
                      QStringList{QStringLiteral("--line"), QStringLiteral("7"),
                                  QStringLiteral("--column"), QStringLiteral("3"), file});
            }
            const auto no_line = buildEditorCommand(
                settings, SourceLocation{where.file}, {}, platform, fake.probe);
            REQUIRE(no_line.command.has_value());
            CHECK(no_line.command->arguments ==
                  (preset->honours_line ? QStringList{file}
                                        : QStringList{QStringLiteral("/Edit"), file}));
        }
    }
    const auto other = presetsFor(EditorPlatform::other);
    CHECK(std::ranges::none_of(other, [](const auto *preset) {
        return preset->id == QStringLiteral("visual_studio") ||
               preset->id == QStringLiteral("notepadpp");
    }));
}

TEST_CASE("ED-1 Windows discovery never executes a launcher", "[devstudio][ed1]") {
    const auto *vscode = externalEditorPreset(QStringLiteral("vscode"));
    REQUIRE(vscode != nullptr);
    FakeProbe fake;
    const QString launcher = QStringLiteral("C:/U/Programs/Microsoft VS Code/bin/code.cmd");
    fake.path[QStringLiteral("code.cmd")] = launcher;
    fake.env[QStringLiteral("LOCALAPPDATA")] = QStringLiteral("C:/U");
    fake.files.insert(QStringLiteral("C:/U/Programs/Microsoft VS Code/Code.exe"));
    CHECK(resolvePresetProgram(*vscode, EditorPlatform::windows, fake.probe) ==
          QStringLiteral("C:/U/Programs/Microsoft VS Code/Code.exe"));

    FakeProbe invalid;
    invalid.path[QStringLiteral("code.cmd")] = launcher;
    invalid.env[QStringLiteral("LOCALAPPDATA")] = QStringLiteral("C:/U");
    invalid.files.insert(QStringLiteral("C:/U/Programs/Microsoft VS Code/Code.exe"));
    invalid.path[QStringLiteral("code.cmd")] = QStringLiteral("C:/Cursor/resources/app/bin/code.cmd");
    CHECK(resolvePresetProgram(*vscode, EditorPlatform::windows, invalid.probe) ==
          QStringLiteral("C:/U/Programs/Microsoft VS Code/Code.exe"));
    invalid.files.clear();
    CHECK(resolvePresetProgram(*vscode, EditorPlatform::windows, invalid.probe).isEmpty());

    const auto *cursor = externalEditorPreset(QStringLiteral("cursor"));
    REQUIRE(cursor != nullptr);
    FakeProbe cursor_fake;
    cursor_fake.path[QStringLiteral("cursor.cmd")] =
        QStringLiteral("C:/x/cursor/resources/app/bin/cursor.cmd");
    cursor_fake.files.insert(QStringLiteral("C:/x/cursor/Cursor.exe"));
    CHECK(resolvePresetProgram(*cursor, EditorPlatform::windows, cursor_fake.probe) ==
          QStringLiteral("C:/x/cursor/Cursor.exe"));

    for (const QString id : {QStringLiteral("clion"), QStringLiteral("rider")}) {
        const auto *preset = externalEditorPreset(id);
        REQUIRE(preset != nullptr);
        FakeProbe jetbrains;
        jetbrains.env[QStringLiteral("ProgramFiles")] = QStringLiteral("C:/Program Files");
        const QString folder = id == QStringLiteral("clion")
                                   ? QStringLiteral("C:/Program Files/JetBrains/CLion 2026.1")
                                   : QStringLiteral("C:/Program Files/JetBrains/JetBrains Rider 2026.1");
        jetbrains.directories = {folder};
        jetbrains.files.insert(folder + QStringLiteral("/bin/") + preset->windows_program);
        CHECK(resolvePresetProgram(*preset, EditorPlatform::windows, jetbrains.probe) ==
              folder + QStringLiteral("/bin/") + preset->windows_program);
    }
}

TEST_CASE("ED-1 Visual Studio discovery is bounded and excludes Build Tools", "[devstudio][ed1]") {
    const auto *preset = externalEditorPreset(QStringLiteral("visual_studio"));
    REQUIRE(preset != nullptr);
    FakeProbe fake;
    const QString devenv = QStringLiteral("C:/VS/Common7/IDE/devenv.exe");
    fake.files.insert(devenv);
    fake.vswhere_output = {devenv + QStringLiteral("\r\n")};
    CHECK(resolvePresetProgram(*preset, EditorPlatform::windows, fake.probe) == devenv);
    REQUIRE(fake.vswhere_calls.size() == 1);
    CHECK(fake.vswhere_calls[0] ==
          QStringList{QStringLiteral("-nologo"), QStringLiteral("-utf8"), QStringLiteral("-latest"),
                      QStringLiteral("-property"), QStringLiteral("productPath")});
    CHECK(!fake.vswhere_calls[0].contains(QStringLiteral("-products")));

    FakeProbe preview;
    preview.files.insert(devenv);
    preview.vswhere_output = {QString{}, devenv};
    CHECK(resolvePresetProgram(*preset, EditorPlatform::windows, preview.probe) == devenv);
    REQUIRE(preview.vswhere_calls.size() == 2);
    CHECK(preview.vswhere_calls[1].contains(QStringLiteral("-prerelease")));
    for (const auto &call : preview.vswhere_calls)
        CHECK(!call.contains(QStringLiteral("-products")));

    FakeProbe build_tools;
    build_tools.vswhere_output = {QStringLiteral("C:/VS/LaunchDevCmd.bat"), QString{}};
    build_tools.path[QStringLiteral("devenv.exe")] = devenv;
    build_tools.files.insert(devenv);
    CHECK(resolvePresetProgram(*preset, EditorPlatform::windows, build_tools.probe) == devenv);
    build_tools.path.clear();
    CHECK(resolvePresetProgram(*preset, EditorPlatform::windows, build_tools.probe).isEmpty());
}

TEST_CASE("ED-1 Custom command refuses shells and missing programs", "[devstudio][ed1]") {
    const SourceLocation where{fs(QStringLiteral("source.frag")), 7, 3};
    for (const QString &program : {QStringLiteral("x.cmd"), QStringLiteral("X.BAT"),
                                   QStringLiteral("x.cmd."), QStringLiteral("x.cmd "),
                                   QStringLiteral("run.ps1"), QStringLiteral("cmd.exe")}) {
        ExternalEditorSettings settings;
        settings.choice = QStringLiteral("custom");
        settings.custom_program = program;
        settings.custom_arguments = QStringLiteral("{file}");
        const auto result = buildEditorCommand(settings, where, {}, EditorPlatform::windows, {});
        INFO(program.toStdString());
        CHECK(!result.command);
        CHECK(result.refusal.contains(QStringLiteral("needs a shell")));
    }
    FakeProbe fake;
    fake.files.insert(QStringLiteral("C:/e/x.EXE"));
    ExternalEditorSettings settings;
    settings.choice = QStringLiteral("custom");
    settings.custom_program = QStringLiteral("C:/e/x.EXE");
    settings.custom_arguments = QStringLiteral("{file}:{line}:{column}");
    auto result = buildEditorCommand(settings, where, {}, EditorPlatform::windows, fake.probe);
    REQUIRE(result.command);
    CHECK(result.command->program == QStringLiteral("C:/e/x.EXE"));

    settings.custom_program = QStringLiteral("x.exe");
    result = buildEditorCommand(settings, where, {}, EditorPlatform::windows, fake.probe);
    CHECK(!result.command);
    CHECK(result.refusal.contains(QStringLiteral("Custom was not found")));
    settings.custom_program.clear();
    result = buildEditorCommand(settings, where, {}, EditorPlatform::windows, fake.probe);
    CHECK(result.refusal == QStringLiteral("No program is set for the Custom editor. Set it in Edit > External Editor..."));

    FakeProbe other;
    other.files.insert(QStringLiteral("/opt/x.sh"));
    settings.custom_program = QStringLiteral("/opt/x.sh");
    settings.custom_arguments = QStringLiteral("+{line} {file}");
    result = buildEditorCommand(settings, where, {}, EditorPlatform::other, other.probe);
    REQUIRE(result.command);
    CHECK(result.command->arguments ==
          QStringList{QStringLiteral("+7"), nativeAbsolutePath(where.file)});
}

TEST_CASE("ED-1 opening reports dispatch and never silently falls back", "[devstudio][ed1]") {
    std::vector<EditorCommand> started;
    std::vector<SourceLocation> system;
    auto backends = recordingBackends(started, system);
    const SourceLocation where{fs(QStringLiteral("shader.frag")), 7, 3};
    ExternalEditorSettings settings;
    auto result = openSourceFileWith(settings, where, {}, backends);
    CHECK(result.opened);
    REQUIRE(system.size() == 1);
    CHECK(started.empty());
    CHECK(result.status == QStringLiteral("Open requested for shader.frag with the system's default program; it can't be told the line: go to line 7."));
    backends.system_default = [](const SourceLocation &) { return false; };
    result = openSourceFileWith(settings, where, {}, backends);
    CHECK(!result.opened);
    CHECK(result.status.startsWith(QStringLiteral("Could not open shader.frag:")));

    settings.choice = QStringLiteral("vscode");
    result = openSourceFileWith(settings, where, {}, backends);
    CHECK(!result.opened);
    CHECK(started.empty());
    REQUIRE(system.size() == 1);
    CHECK(result.status.contains(QStringLiteral("Visual Studio Code was not found")));

    settings.choice = QStringLiteral("future_editor");
    result = openSourceFileWith(settings, where, {}, backends);
    CHECK(result.status == QStringLiteral("The saved editor choice future_editor isn't known. Change it in Edit > External Editor..."));

    FakeProbe fake;
    fake.files.insert(QStringLiteral("/opt/editor"));
    fake.path[QStringLiteral("editor")] = QStringLiteral("/opt/editor");
    backends.probe = fake.probe;
    settings.choice = QStringLiteral("custom");
    settings.custom_program = QStringLiteral("editor");
    settings.custom_arguments = QStringLiteral("\"{file}\"");
    started.clear();
    result = openSourceFileWith(settings, where, {}, backends);
    CHECK(result.opened);
    REQUIRE(started.size() == 1);
    CHECK(started.back().program == QStringLiteral("/opt/editor"));
    backends.start_process = [](const QString &, const QStringList &) { return false; };
    result = openSourceFileWith(settings, where, {}, backends);
    CHECK(result.status == QStringLiteral("Could not start /opt/editor to open shader.frag. Change it in Edit > External Editor..."));

    backends.start_process = [&started](const QString &program, const QStringList &arguments) {
        started.push_back({program, arguments});
        return true;
    };
    started.clear();
    settings.custom_arguments = QStringLiteral("{file}");
    CHECK(openSourceFileWith(settings, SourceLocation{fs(QStringLiteral("--evil.frag"))}, {}, backends).opened);
    REQUIRE(started.size() == 1);
    REQUIRE(started[0].arguments.size() == 1);
    CHECK(!started[0].arguments[0].startsWith(QLatin1Char('-')));

    settings.custom_arguments = QStringLiteral("{file} {project}");
    result = openSourceFileWith(settings, where, {}, backends);
    CHECK(!result.opened);
    CHECK(result.status == QStringLiteral("The Custom editor's arguments use {project}, and no project is open."));
}

TEST_CASE("ED-1 project documents are confined to regular source files", "[devstudio][ed1]") {
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const std::filesystem::path root = fs(temporary.path()) / "project";
    std::filesystem::create_directories(root / "shaders");
    std::ofstream{root / "shaders" / "present.frag"} << "shader";
    std::ofstream{root / "run.exe"} << "not a source";
    std::ofstream{root / "run.cmd"} << "not a source";
    std::ofstream{root / "run.lnk"} << "not a source";
    std::ofstream{root / "run.url"} << "not a source";
    std::ofstream{root / "run.py"} << "not a source";
    std::ofstream{root / "run.lua"} << "not a source";
    std::ofstream{root / "outside.frag"} << "outside";
    std::ofstream{fs(temporary.path()) / "external.frag"} << "external";
    const QString root_text = QString::fromStdU16String(root.u16string());
    const auto positive = projectSourceLocation(root_text, QStringLiteral("shaders/present.frag"), 7, 3);
    REQUIRE(positive);
    CHECK(positive->file == std::filesystem::canonical(root / "shaders" / "present.frag"));
    CHECK(positive->line == 7);
    CHECK(positive->column == 3);
    CHECK(!projectSourceLocation(root_text, QStringLiteral("shaders/present.frag"), 0, 0)->line);
    CHECK(!projectSourceLocation(root_text, QStringLiteral("shaders/present.frag"), 7, 0)->column);
    CHECK(!projectSourceLocation({}, QStringLiteral("shaders/present.frag"), 7, 3));
    CHECK(!projectSourceLocation(root_text, {}, 7, 3));
    CHECK(!projectSourceLocation(root_text, QStringLiteral("engine://x"), 7, 3));
    CHECK(!projectSourceLocation(root_text, QStringLiteral("../outside.frag"), 7, 3));
    CHECK(!projectSourceLocation(root_text, QString::fromStdU16String((root / "outside.frag").u16string()), 7, 3));
    CHECK(!projectSourceLocation(root_text, QStringLiteral("missing.frag"), 7, 3));
    CHECK(!projectSourceLocation(root_text, QStringLiteral("shaders"), 7, 3));
    for (const QString &document : {QStringLiteral("run.exe"), QStringLiteral("run.cmd"),
                                    QStringLiteral("run.lnk"), QStringLiteral("run.url"),
                                    QStringLiteral("run.py"), QStringLiteral("run.lua")})
        CHECK(!projectSourceLocation(root_text, document, 7, 3));
    std::error_code error;
    std::filesystem::create_directory_symlink(fs(temporary.path()), root / "shaders" / "escape", error);
    if (!error) CHECK(!projectSourceLocation(root_text, QStringLiteral("shaders/escape/external.frag"), 7, 3));
}

TEST_CASE("ED-1 shaderErrorLine follows only its own file", "[devstudio][ed1]") {
    const std::filesystem::path file = fs(QStringLiteral("shader.frag"));
    CHECK(shaderErrorLine("Shader compile failed: shader.frag\nshader.frag:3: error: bad\n", file) == 3);
    CHECK(!shaderErrorLine("Shader compile failed: shader.frag\ninclude.frag:3: error: bad\n", file));
    CHECK(shaderErrorLine("Shader compile failed: other.frag\nother.frag:4: error: bad\n; "
                          "Shader compile failed: shader.frag\nshader.frag:8: error: bad\n", file) == 8);
    CHECK(!shaderErrorLine("shader.frag:3: error: bad", file));
}

TEST_CASE("ED-1 dialog edits only user settings and preserves unknown choices", "[devstudio][ed1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    QSettings settings{config.filePath(QStringLiteral("studio.ini")), QSettings::IniFormat};
    settings.setValue(QStringLiteral("external_editor/choice"), QStringLiteral("future"));
    settings.setValue(QStringLiteral("external_editor/custom_program"), QStringLiteral("old-program"));
    ExternalEditorDialog dialog{settings, EditorPlatform::other, {}};
    auto *choice = dialog.findChild<QComboBox *>(QStringLiteral("pelican.externalEditor.choice"));
    auto *program = dialog.findChild<QLineEdit *>(QStringLiteral("pelican.externalEditor.program"));
    auto *browse = dialog.findChild<QPushButton *>(QStringLiteral("pelican.externalEditor.browse"));
    auto *arguments = dialog.findChild<QLineEdit *>(QStringLiteral("pelican.externalEditor.arguments"));
    auto *ok = dialog.findChild<QPushButton *>(QStringLiteral("pelican.externalEditor.ok"));
    auto *cancel = dialog.findChild<QPushButton *>(QStringLiteral("pelican.externalEditor.cancel"));
    auto *error = dialog.findChild<QLabel *>(QStringLiteral("pelican.externalEditor.error"));
    REQUIRE(choice);
    REQUIRE(program);
    REQUIRE(browse);
    REQUIRE(arguments);
    REQUIRE(ok);
    REQUIRE(cancel);
    REQUIRE(error);
    CHECK(choice->currentData().toString() == QStringLiteral("future"));
    CHECK(choice->currentText().contains(QStringLiteral("Unknown editor")));
    CHECK(!ok->isEnabled());
    choice->setCurrentIndex(choice->findData(QStringLiteral("system")));
    CHECK(!program->isEnabled());
    CHECK(!browse->isEnabled());
    CHECK(!arguments->isEnabled());
    CHECK(ok->isEnabled());
    choice->setCurrentIndex(choice->findData(QStringLiteral("custom")));
    CHECK(program->isEnabled());
    CHECK(browse->isEnabled());
    CHECK(arguments->isEnabled());
    dialog.setProgramChooser([] { return QStringLiteral("/opt/chosen"); });
    browse->click();
    CHECK(program->text() == QStringLiteral("/opt/chosen"));
    arguments->setText(QStringLiteral("{file} {wrong}"));
    CHECK(!ok->isEnabled());
    CHECK(error->text().contains(QStringLiteral("wrong")));
    arguments->setText(QStringLiteral("--goto {file}:{line}:{column}"));
    REQUIRE(ok->isEnabled());
    cancel->click();
    CHECK(ExternalEditorStore{settings}.load().choice == QStringLiteral("future"));
    ok->click();
    const auto saved = ExternalEditorStore{settings}.load();
    CHECK(saved.choice == QStringLiteral("custom"));
    CHECK(saved.custom_program == QStringLiteral("/opt/chosen"));
    CHECK(saved.custom_arguments == QStringLiteral("--goto {file}:{line}:{column}"));

    QSettings roundtrip{config.filePath(QStringLiteral("studio.ini")), QSettings::IniFormat};
    CHECK(ExternalEditorStore{roundtrip}.load() == saved);
}

TEST_CASE("ED-1 MainWindow uses one source opening function", "[devstudio][ed1]") {
    application();
    QTemporaryDir config;
    REQUIRE(config.isValid());
    MainWindow window{config.path()};
    const int original_width = window.minimumSizeHint().width();
    std::vector<EditorCommand> started;
    std::vector<SourceLocation> system;
    auto backends = recordingBackends(started, system);
    FakeProbe fake;
    fake.files.insert(QStringLiteral("/opt/editor"));
    fake.path[QStringLiteral("editor")] = QStringLiteral("/opt/editor");
    backends.probe = fake.probe;
    window.setSourceOpenBackends(std::move(backends));
    const SourceLocation where{fs(QStringLiteral("shader.frag")), 7, 3};
    CHECK(window.openSourceFile(where));
    CHECK(system.size() == 1);
    CHECK(started.empty());
    CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("Open requested for shader.frag")));

    auto *action = window.findChild<QAction *>(QStringLiteral("pelican.edit.externalEditor"));
    REQUIRE(action);
    action->trigger();
    auto *dialog = window.findChild<ExternalEditorDialog *>(
        QStringLiteral("pelican.externalEditor.dialog"));
    REQUIRE(dialog);
    auto *choice = dialog->findChild<QComboBox *>(QStringLiteral("pelican.externalEditor.choice"));
    auto *program = dialog->findChild<QLineEdit *>(QStringLiteral("pelican.externalEditor.program"));
    auto *arguments = dialog->findChild<QLineEdit *>(QStringLiteral("pelican.externalEditor.arguments"));
    auto *ok = dialog->findChild<QPushButton *>(QStringLiteral("pelican.externalEditor.ok"));
    REQUIRE(choice);
    REQUIRE(program);
    REQUIRE(arguments);
    REQUIRE(ok);
    choice->setCurrentIndex(choice->findData(QStringLiteral("custom")));
    program->setText(QStringLiteral("editor"));
    arguments->setText(QStringLiteral("{file}:{line}:{column}"));
    ok->click();
    CHECK(window.openSourceFile(where));
    REQUIRE(started.size() == 1);
    CHECK(system.size() == 1);
    CHECK(started[0].program == QStringLiteral("/opt/editor"));
    CHECK(started[0].arguments.size() == 1);
    CHECK(started[0].arguments[0].endsWith(QStringLiteral("shader.frag:7:3")));
    dialog->close();
    CHECK(window.minimumSizeHint().width() == original_width);

    QSettings settings{config.filePath(QStringLiteral("studio.ini")), QSettings::IniFormat};
    settings.setValue(QStringLiteral("external_editor/choice"), QStringLiteral("vscode"));
    settings.sync();
    CHECK(!window.openSourceFile(where));
    CHECK(started.size() == 1);
    CHECK(system.size() == 1);
    CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("Visual Studio Code was not found")));
    settings.setValue(QStringLiteral("external_editor/choice"), QStringLiteral("future_editor"));
    settings.sync();
    CHECK(!window.openSourceFile(where));
    CHECK(window.statusBar()->currentMessage().contains(QStringLiteral("future_editor isn't known")));

    settings.setValue(QStringLiteral("external_editor/choice"), QStringLiteral("custom"));
    settings.sync();
    started.clear();
    CHECK(window.openSourceFile(SourceLocation{fs(QStringLiteral("--evil.frag"))}));
    REQUIRE(started.size() == 1);
    CHECK(!started[0].arguments[0].startsWith(QLatin1Char('-')));

    auto failing = recordingBackends(started, system);
    failing.probe = fake.probe;
    failing.start_process = [](const QString &, const QStringList &) { return false; };
    window.setSourceOpenBackends(std::move(failing));
    CHECK(!window.openSourceFile(where));
    CHECK(window.statusBar()->currentMessage() ==
          QStringLiteral("Could not start /opt/editor to open shader.frag. Change it in Edit > External Editor..."));
}

TEST_CASE("ED-1 Studio panels dispatch through MainWindow", "[devstudio][ed1]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const std::filesystem::path root = fs(temporary.path()) / "project";
    std::filesystem::create_directories(root / "shaders");
    const auto file = std::filesystem::canonical(root / "shaders") / "effective.frag";
    std::ofstream{root / "project.json"}
        << R"({"schema":"pelican.project","version":1,"name":"external-editor"})";
    std::ofstream{file} << "void main() {}\n";
    const QString file_text = QString::fromStdU16String(file.u16string());

    MainWindow window{temporary.path()};
    std::vector<EditorCommand> started;
    std::vector<SourceLocation> system;
    auto backends = recordingBackends(started, system);
    FakeProbe fake;
    fake.path[QStringLiteral("editor")] = QStringLiteral("/opt/editor");
    fake.files.insert(QStringLiteral("/opt/editor"));
    backends.probe = fake.probe;
    window.setSourceOpenBackends(std::move(backends));
    ExternalEditorSettings settings;
    settings.choice = QStringLiteral("custom");
    settings.custom_program = QStringLiteral("editor");
    settings.custom_arguments = QStringLiteral("{file}:{line}:{column}");
    QSettings saved{temporary.filePath(QStringLiteral("studio.ini")), QSettings::IniFormat};
    QString error;
    REQUIRE(ExternalEditorStore{saved}.save(settings, &error));

    window.showRenderTool(QStringLiteral("frame_plan"));
    auto *frame_plan = childOfType<FramePlanWidget>(window);
    REQUIRE(frame_plan);
    frame_plan->setProjectRoot(root);
    using Json = nlohmann::json;
    const Json wire{
        {"schema", "pelican.frame_plan"}, {"version", 1}, {"profile", "runtime"},
        {"graph", "external_editor"},
        {"nodes", Json::array({{{"name", "present"}, {"kind", "render"},
          {"declaration_index", 0}, {"order", 0}, {"reads", Json::array()},
          {"writes", Json::array()}, {"shader_resolution", {{"state", "resolved"},
          {"stages", Json::array({{{"stage", "fragment"}, {"declared_ref", "shaders/effective"},
          {"effective_ref", "shaders/effective"}, {"origin", "provider"},
          {"source_open_ref", "project://shaders/effective.frag"}}})}}}}})},
        {"barriers", Json::array()}, {"resources", Json::array()}};
    frame_plan->receiveResult(QByteArray::fromStdString(wire.dump()));
    QApplication::processEvents();
    auto *open = frame_plan->findChild<QPushButton *>(QStringLiteral("pelican.shaderSourceOpen"));
    REQUIRE(open);
    const auto expected = [&](int line) { return file_text + QStringLiteral(":%1:1").arg(line); };
    REQUIRE(window.openSourceFile(SourceLocation{file}));
    REQUIRE(started.size() == 1);
    started.clear();
    open->click();
    INFO(window.statusBar()->currentMessage().toStdString());
    REQUIRE(started.size() == 1);
    CHECK(started.back().arguments == QStringList{expected(1)});

    const auto status = [](const Json &last_error) {
        const Json shaders{{"attempted", 1}, {"applied", 0}, {"failed", 1},
                           {"last_error", last_error}};
        const Json runtime{{"pelican.shaders", shaders}};
        const Json reload{{"runtime", runtime}};
        return QByteArray::fromStdString(Json{{"reload", reload}}.dump());
    };
    const QString log = QStringLiteral("Shader compile failed: %1\n%1:3: error: bad\n").arg(file_text);
    frame_plan->receiveShaderReloadStatus(status(log.toStdString()));
    open->click();
    REQUIRE(started.size() == 2);
    CHECK(started.back().arguments == QStringList{expected(3)});
    frame_plan->receiveShaderReloadStatus(status(nullptr));
    open->click();
    REQUIRE(started.size() == 3);
    CHECK(started.back().arguments == QStringList{expected(1)});

    auto *fullscreen = childOfType<FullscreenPassWidget>(window);
    REQUIRE(fullscreen);
    CHECK(fullscreen->hasShaderSourceOpenAction());
}

TEST_CASE("ED-1 default process backend preserves hostile filenames as arguments", "[devstudio][ed1]") {
    application();
    QTemporaryDir temporary;
    REQUIRE(temporary.isValid());
    const QString installed_helper = QStringLiteral(PELICAN_EDITOR_HELPER_PATH);
    REQUIRE(QFileInfo::exists(installed_helper));
    const QDir launch_directory{temporary.filePath(QStringLiteral("launch"))};
    REQUIRE(QDir{}.mkpath(launch_directory.absolutePath()));
    const QString helper = launch_directory.filePath(QFileInfo{installed_helper}.fileName());
    REQUIRE(QFile::copy(installed_helper, helper));
    const QString canary = launch_directory.filePath(QStringLiteral("canary"));
    auto backends = defaultSourceOpenBackends();
    qputenv("ELECTRON_RUN_AS_NODE", "1");
    const auto restore = qScopeGuard([] { qunsetenv("ELECTRON_RUN_AS_NODE"); });
    QStringList names{
        QStringLiteral("a b.frag"), QStringLiteral("a&calc.frag"),
        QStringLiteral("a;touch canary.frag"), QStringLiteral("$(touch canary).frag"),
        QStringLiteral("%PATH%.frag"), QStringLiteral("^x.frag"),
        QStringLiteral("(x).frag"), QStringLiteral("'q'.frag"),
        QStringLiteral("{line}.frag"), QString::fromUtf16(u"シェーダ.frag")};
#ifndef Q_OS_WIN
    names.append({QStringLiteral("a\"b.frag"), QStringLiteral("a|b.frag"),
                  QStringLiteral("`touch canary`.frag"), QStringLiteral("a<b>.frag")});
#endif
    for (qsizetype index = 0; index < names.size(); ++index) {
        const QString input = temporary.filePath(names[index]);
        const QString output = temporary.filePath(QStringLiteral("out-%1.txt").arg(index));
        {
            QFile file{input};
            REQUIRE(file.open(QIODevice::WriteOnly));
        }
        INFO(names[index].toStdString());
        REQUIRE(backends.start_process(helper, {output, nativeAbsolutePath(fs(input)), QStringLiteral("--line=7")}));
        for (int wait = 0; wait < 100 && !QFileInfo::exists(output); ++wait) QTest::qWait(100);
        REQUIRE(QFileInfo::exists(output));
        QFile result{output};
        REQUIRE(result.open(QIODevice::ReadOnly));
        const QString contents = QString::fromUtf8(result.readAll());
        const QStringList lines = contents.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        CHECK(lines.size() == 3);
        CHECK(!QFileInfo::exists(canary));
        if (lines.size() != 3) continue;
        CHECK(lines[0] == nativeAbsolutePath(fs(input)));
        CHECK(lines[1] == QStringLiteral("--line=7"));
        CHECK(lines[2] == QStringLiteral("env:ELECTRON_RUN_AS_NODE="));
    }
    CHECK(!QFileInfo::exists(canary));
    CHECK(!backends.start_process(temporary.filePath(QStringLiteral("missing-editor")), {}));
}
