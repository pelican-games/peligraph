// F-2: File > New Project... in Studio. The dialog writes the template a person
// picked through Pelican::initializeProjectTemplate -- the function
// `pelican_cli project init` calls -- and the window opens what it wrote through
// its ordinary openProject, unsaved-changes guard included.
#include "inspectorwidget.hpp"
#include "mainwindow.hpp"
#include "newprojectdialog.hpp"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace {

using PelicanStudio::MainWindow;
using PelicanStudio::NewProjectDialog;

QApplication &application() {
    static int argument_count = 1;
    static char application_name[] = "pelican_newproject_test";
    static char *arguments[] = {application_name};
    static QApplication instance{argument_count, arguments};
    return instance;
}

std::filesystem::path fsPath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

nlohmann::json readJson(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    REQUIRE(file.is_open());
    return nlohmann::json::parse(file);
}

template <class T>
T &required(const QWidget &root, const char *name) {
    auto *found = root.findChild<T *>(QString::fromLatin1(name));
    REQUIRE(found != nullptr);
    return *found;
}

void fill(NewProjectDialog &dialog, const QString &location,
          const QString &name, const char *template_name) {
    required<QLineEdit>(dialog, "pelican.newProject.location").setText(location);
    required<QLineEdit>(dialog, "pelican.newProject.name").setText(name);
    auto &templates = required<QComboBox>(dialog, "pelican.newProject.template");
    const int index = templates.findData(QString::fromLatin1(template_name));
    REQUIRE(index >= 0);
    templates.setCurrentIndex(index);
}

// The player the viewport would launch does not exist, so opening a project
// never starts an engine: what is under test is the window's route to it.
class MissingPlayer {
  public:
    explicit MissingPlayer(const QTemporaryDir &directory)
        : previous_(qEnvironmentVariable("PELICAN_STUDIO_PLAYER")),
          had_previous_(qEnvironmentVariableIsSet("PELICAN_STUDIO_PLAYER")) {
        qputenv("PELICAN_STUDIO_PLAYER",
                directory.filePath(QStringLiteral("missing-player.exe")).toUtf8());
    }
    ~MissingPlayer() {
        if (had_previous_)
            qputenv("PELICAN_STUDIO_PLAYER", previous_.toUtf8());
        else
            qunsetenv("PELICAN_STUDIO_PLAYER");
    }
    MissingPlayer(const MissingPlayer &) = delete;
    MissingPlayer &operator=(const MissingPlayer &) = delete;

  private:
    QString previous_;
    bool had_previous_;
};

} // namespace

TEST_CASE("F-2 New Project writes the template that was picked and hands on its root",
          "[devstudio][newproject][f2]") {
    (void)application();
    QTemporaryDir parent;
    REQUIRE(parent.isValid());

    SECTION("Blank pipeline") {
        std::optional<QString> opened;
        NewProjectDialog dialog{nullptr, [&](const QString &root) { opened = root; }};
        fill(dialog, parent.path(), QStringLiteral("blank_one"), "blank");
        dialog.show();
        required<QPushButton>(dialog, "pelican.newProject.create").click();

        REQUIRE(opened.has_value());
        REQUIRE(dialog.result() == QDialog::Accepted);
        REQUIRE_FALSE(dialog.isVisible());
        const auto root = fsPath(*opened);
        std::error_code ec;
        REQUIRE(std::filesystem::equivalent(
            root, fsPath(parent.path()) / "blank_one", ec));
        REQUIRE(std::filesystem::is_regular_file(root / "shaders" / "present.frag"));
        const auto config = readJson(root / "passes" / "main_rendering_config.json");
        const auto &rendering = config.at("rendering");
        REQUIRE_FALSE(rendering.contains("pipeline"));
        REQUIRE(rendering.at("rendering_passes").at(0).at("passes").size() == 1);
        REQUIRE(rendering.at("rendering_passes").at(0).at("passes").at(0).at("name") ==
                "present");
    }

    SECTION("Standard 3D scene") {
        std::optional<QString> opened;
        NewProjectDialog dialog{nullptr, [&](const QString &root) { opened = root; }};
        fill(dialog, parent.path(), QStringLiteral("standard_one"), "standard");
        required<QPushButton>(dialog, "pelican.newProject.create").click();

        REQUIRE(opened.has_value());
        const auto root = fsPath(*opened);
        REQUIRE_FALSE(std::filesystem::exists(root / "shaders"));
        const auto config = readJson(root / "passes" / "main_rendering_config.json");
        REQUIRE(config.at("rendering").at("pipeline").at("preset") ==
                "engine://render_pipelines/hybrid_v1.json");
    }
}

TEST_CASE("F-2 New Project refuses a folder that already holds files, in initializeProjectTemplate's words",
          "[devstudio][newproject][f2]") {
    (void)application();
    QTemporaryDir parent;
    REQUIRE(parent.isValid());
    REQUIRE(QDir(parent.path()).mkdir(QStringLiteral("taken")));
    const QString keep = QDir(parent.path()).filePath(QStringLiteral("taken/keep.txt"));
    {
        QFile file{keep};
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("keep\n");
    }

    bool called = false;
    NewProjectDialog dialog{nullptr, [&](const QString &) { called = true; }};
    fill(dialog, parent.path(), QStringLiteral("taken"), "blank");
    dialog.show();
    required<QPushButton>(dialog, "pelican.newProject.create").click();

    REQUIRE_FALSE(called);
    REQUIRE(dialog.isVisible());
    REQUIRE(dialog.result() != QDialog::Accepted);
    const QString notice = required<QLabel>(dialog, "pelican.newProject.notice").text();
    REQUIRE(notice.contains(QStringLiteral("directory is not empty")));
    REQUIRE(notice.contains(QStringLiteral("taken")));
    REQUIRE_FALSE(QFile::exists(
        QDir(parent.path()).filePath(QStringLiteral("taken/project.json"))));

    // Another name in the same folder goes through, and the notice goes away
    // the moment the name changes.
    required<QLineEdit>(dialog, "pelican.newProject.name").setText(QStringLiteral("free"));
    REQUIRE(required<QLabel>(dialog, "pelican.newProject.notice").text().isEmpty());
    required<QPushButton>(dialog, "pelican.newProject.create").click();
    REQUIRE(called);
}

TEST_CASE("T-1 New Project starts on the last folder used and a free project_NNN",
          "[devstudio][newproject][t1]") {
    (void)application();
    QTemporaryDir parent;
    QTemporaryDir settings;
    REQUIRE(parent.isValid());
    REQUIRE(settings.isValid());

    CHECK(PelicanStudio::nextFreeProjectName(parent.path()) == QStringLiteral("project_001"));
    REQUIRE(QDir(parent.path()).mkdir(QStringLiteral("project_001")));
    // Any entry counts, a plain file too; the lowest free number is taken.
    {
        QFile plain_file{QDir(parent.path()).filePath(QStringLiteral("project_002"))};
        REQUIRE(plain_file.open(QIODevice::WriteOnly));
    }
    REQUIRE(QDir(parent.path()).mkdir(QStringLiteral("project_004")));
    CHECK(PelicanStudio::nextFreeProjectName(parent.path()) == QStringLiteral("project_003"));
    CHECK(PelicanStudio::nextFreeProjectName(QDir(parent.path()).filePath(QStringLiteral("absent"))) ==
          QStringLiteral("project_001"));

    const QString elsewhere = QDir(parent.path()).filePath(QStringLiteral("elsewhere"));
    REQUIRE(QDir{}.mkpath(elsewhere));
    std::optional<QString> opened;
    {
        NewProjectDialog dialog{nullptr, [&](const QString &root) { opened = root; },
                                settings.path()};
        auto &location = required<QLineEdit>(dialog, "pelican.newProject.location");
        auto &name = required<QLineEdit>(dialog, "pelican.newProject.name");
        // Never used: Documents, and a name nothing there has.
        CHECK(QDir::cleanPath(location.text()) ==
              QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)));
        CHECK(name.text().startsWith(QStringLiteral("project_")));
        // The proposed name follows the folder...
        location.setText(parent.path());
        CHECK(name.text() == QStringLiteral("project_003"));
        location.setText(elsewhere);
        CHECK(name.text() == QStringLiteral("project_001"));
        // ...until the person types one of their own.
        name.setText(QStringLiteral("mine"));
        location.setText(parent.path());
        CHECK(name.text() == QStringLiteral("mine"));
        name.clear();
        location.setText(elsewhere);
        CHECK(name.text() == QStringLiteral("project_001"));
        required<QPushButton>(dialog, "pelican.newProject.create").click();
    }
    REQUIRE(opened.has_value());
    std::error_code error;
    CHECK(std::filesystem::equivalent(fsPath(*opened), fsPath(elsewhere) / "project_001", error));

    // The next New Project starts where that one was made, on the next name.
    NewProjectDialog again{nullptr, [](const QString &) {}, settings.path()};
    CHECK(QDir::cleanPath(required<QLineEdit>(again, "pelican.newProject.location").text()) ==
          QDir::cleanPath(elsewhere));
    CHECK(required<QLineEdit>(again, "pelican.newProject.name").text() ==
          QStringLiteral("project_002"));
    // Without a settings directory nothing is remembered.
    NewProjectDialog plain{nullptr, [](const QString &) {}};
    CHECK(QDir::cleanPath(required<QLineEdit>(plain, "pelican.newProject.location").text()) ==
          QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)));
}

TEST_CASE("F-2 File > New Project... sits above Open Project... and opens what it wrote",
          "[devstudio][newproject][f2][mainwindow]") {
    (void)application();
    QTemporaryDir application_config;
    QTemporaryDir parent;
    REQUIRE(application_config.isValid());
    REQUIRE(parent.isValid());
    MissingPlayer missing_player{parent};
    MainWindow window{application_config.path()};

    auto *file_menu = window.menuBar()->findChild<QMenu *>(QStringLiteral("pelican.fileMenu"));
    REQUIRE(file_menu != nullptr);
    int new_at = -1;
    int open_at = -1;
    QAction *new_project = nullptr;
    const auto actions = file_menu->actions();
    for (int index = 0; index < actions.size(); ++index) {
        if (actions[index]->objectName() == QLatin1String("pelican.file.newProject")) {
            new_at = index;
            new_project = actions[index];
        }
        if (actions[index]->objectName() == QLatin1String("pelican.file.openProject"))
            open_at = index;
    }
    REQUIRE(new_project != nullptr);
    REQUIRE(open_at >= 0);
    REQUIRE(new_at + 1 == open_at);

    const auto create = [&](const QString &name) {
        new_project->trigger();
        QApplication::processEvents();
        // NewProjectDialog has no Q_OBJECT of its own, so it is found as the
        // QDialog it is and then asked what it is.
        auto *dialog = dynamic_cast<NewProjectDialog *>(window.findChild<QDialog *>(
            QStringLiteral("pelican.newProjectDialog")));
        REQUIRE(dialog != nullptr);
        REQUIRE(dialog->isVisible());
        fill(*dialog, parent.path(), name, "blank");
        required<QPushButton>(*dialog, "pelican.newProject.create").click();
        QApplication::processEvents();
    };

    SECTION("a clean window opens the new project") {
        window.show();
        create(QStringLiteral("opened_blank"));
        const QString message = window.statusBar()->currentMessage();
        INFO(message.toStdString());
        REQUIRE(message.startsWith(QStringLiteral("Opened ")));
        REQUIRE(message.contains(QStringLiteral("opened_blank")));
    }

    SECTION("a window with unsaved scene changes asks first, as Open Project does") {
        auto *inspector = dynamic_cast<PelicanStudio::InspectorWidget *>(
            window.findChild<QWidget *>(QStringLiteral("pelican.inspector")));
        REQUIRE(inspector != nullptr);
        inspector->observeSceneRevision(1);
        inspector->observeSceneRevision(2);
        window.show();
        QApplication::processEvents();
        REQUIRE(inspector->hasUnsavedSceneChanges());

        create(QStringLiteral("guarded_blank"));
        // Written, but not opened behind the person's back.
        REQUIRE(QFile::exists(QDir(parent.path()).filePath(
            QStringLiteral("guarded_blank/shaders/present.frag"))));
        auto *prompt = window.findChild<QMessageBox *>(
            QStringLiteral("pelican.unsavedScenePrompt"));
        REQUIRE(prompt != nullptr);
        prompt->button(QMessageBox::Cancel)->click();
        QApplication::processEvents();
        REQUIRE_FALSE(window.statusBar()->currentMessage().startsWith(
            QStringLiteral("Opened ")));
    }
}
