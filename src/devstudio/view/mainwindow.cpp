#include "mainwindow.hpp"
#include "assetsdock.hpp"
#include "materialsdock.hpp"
#include "frameplaneditcapability.hpp"
#include "frameplanwidget.hpp"
#include "fullscreenpasswidget.hpp"
#include "fullscreenshadertemplate.hpp"
#include "graphcanvaswidget.hpp"
#include "inspectorwidget.hpp"
#include "outlinertree.hpp"
#include "../model/inspectormodel.hpp"
#include "../model/outlineredit.hpp"
#include "newprojectdialog.hpp"
#include "panelhelp.hpp"
#include "playcontrols.hpp"
#include "problemsdock.hpp"
#include "projectmigrationwidget.hpp"
#include "renderconfigdigest.hpp"
#include "planperfhub.hpp"
#include "renderfallbacknotice.hpp"
#include "renderfeatureswidget.hpp"
#include "renderingsettingswidget.hpp"
#include "rendereditcompletion.hpp"
#include "runtimeoutliner.hpp"
#include "studiotheme.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QAbstractItemView>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDebug>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QFont>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPaintEvent>
#include <QRegion>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QElapsedTimer>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <optional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace PelicanStudio {
namespace {

// `scene_tree` carries the engine's own `scene_revision`, so a reply that did
// not move it is an answer Studio already had. The outliner keeps the quick
// cadence while the scene is moving and doubles its way back to the slow one
// while it is not, rather than paying for four full scene trees a second to
// watch nothing happen.
constexpr int OutlinerRefreshMinimumIntervalMs = 250;
constexpr int OutlinerRefreshMaximumIntervalMs = 2000;

const QString &lastPanelLayoutName() {
    static const QString name = QStringLiteral("Last panel session");
    return name;
}

const QString &lastToolLayoutName() {
    static const QString name = QStringLiteral("Last tool session");
    return name;
}

QString applicationConfigDirectory() {
    // A run that must not read or leave a person's layouts -- the
    // automation scenarios -- names a directory of its own.
    const QString overridden =
        qEnvironmentVariable("PELICAN_STUDIO_CONFIG_DIR");
    if (!overridden.isEmpty()) return overridden;
    return QStandardPaths::writableLocation(
        QStandardPaths::AppConfigLocation);
}

QString layoutPresetDirectory(const QString &application_config) {
    return QDir(application_config).filePath(QStringLiteral("layouts"));
}

QStringList userPresetNames(QStringList names, const QString &reserved_name) {
    names.removeAll(reserved_name);
    return names;
}

QDockWidget *makeDock(
    QMainWindow *window, const QString &title, const QString &object_name, QWidget *contents) {
    auto *dock = new QDockWidget(title, window);
    dock->setObjectName(object_name);
    dock->setAllowedAreas(Qt::AllDockWidgetAreas);
    dock->setFeatures(
        QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    dock->setWidget(contents);
    return dock;
}

std::filesystem::path filesystemPath(const QString &path) {
#ifdef _WIN32
    return std::filesystem::path{path.toStdWString()};
#else
    return std::filesystem::path{path.toStdString()};
#endif
}

QString displayPath(const std::filesystem::path &path) {
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

// What keeps a row's open/closed state across a rebuild: the scene, and for
// an object its authoring id (OL-1), which a duplicate or a delete before it
// does not move.
QString rowIdentity(const QTreeWidgetItem *item) {
    const QString scene = item->data(0, OutlinerSceneIdRole).toString();
    const QVariant id = item->data(0, OutlinerAuthoringIdRole);
    return id.isValid() ? scene + QLatin1Char('#') + id.toString() : scene;
}

bool sameObject(const OutlinerObjectKey &left, const OutlinerObjectKey &right) {
    return left.scene_id == right.scene_id &&
           left.authoring_object_id == right.authoring_object_id;
}

// PM-3a / PM-3b (design_play_mode 12.2): the frame around the viewport. While
// the game plays or is paused, the margin between this frame and the viewport
// it holds is filled with the theme's playing or paused colour; in edit, it
// is the window's own.
// The viewport itself is left alone -- the engine's picture is a native
// window no Qt painting can cover.
class PlayTintFrame final : public QFrame {
  public:
    explicit PlayTintFrame(QWidget *parent) : QFrame(parent) {}

    void setHole(QWidget *hole) { hole_ = hole; }
    void setTint(const QColor &tint) {
        if (tint == tint_) return;
        tint_ = tint;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QFrame::paintEvent(event);
        if (!tint_.isValid()) return;
        // Around the viewport, from a little above it to the frame's edges;
        // the title above keeps the window's colour, so it stays readable.
        QRegion ring{rect()};
        if (hole_ != nullptr && hole_->isVisible()) {
            const QRect hole = hole_->geometry();
            ring = QRegion{QRect{QPoint{rect().left(), std::max(rect().top(), hole.top() - 6)},
                                 rect().bottomRight()}} -
                   QRegion{hole};
        }
        QPainter painter{this};
        painter.setClipRegion(ring);
        painter.fillRect(rect(), tint_);
    }

  private:
    QWidget *hole_ = nullptr;
    QColor tint_;
};

} // namespace

MainWindow::MainWindow() : MainWindow(applicationConfigDirectory()) {}

MainWindow::MainWindow(const QString &application_config_directory)
    : application_config_directory_(
          QDir::cleanPath(application_config_directory)),
      layout_presets_(
          layoutPresetDirectory(application_config_directory_)) {
    if (application_config_directory_.isEmpty() ||
        application_config_directory_ == QStringLiteral(".")) {
        throw std::invalid_argument(
            "MainWindow requires an application config directory");
    }
    setWindowTitle(tr("Pelican Studio[*]"));
    setDockOptions(
        QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
        QMainWindow::GroupedDragging);
    setDockNestingEnabled(true);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

    // The theme is the person's taste, like a panel layout: it is stored
    // beside the window and applied before anything is drawn, so a Studio
    // opens in the theme it was closed in rather than in the table's default.
    applyStoredTheme();
    createWorkspace();
    createMenus();
    restoreLastPanelLayout();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!close_authorized_ &&
        (project_model_ || inspector_->hasUnsavedSceneChanges() || inspector_->sceneEditPending())) {
        event->ignore();
        // Do not close recursively from within QWidget's active close event.
        QTimer::singleShot(0, this, [this] { requestNavigation(Navigation::Close); });
        return;
    }
    close_authorized_ = false;
    saveLastToolLayout();
    saveLastPanelLayout();
    QMainWindow::closeEvent(event);
}

void MainWindow::createWorkspace() {
    auto *workspace = new PlayTintFrame(this);
    workspace->setObjectName(QStringLiteral("pelican.viewportFrame"));
    viewport_frame_ = workspace;
    workspace->setFrameShape(QFrame::StyledPanel);
    auto *workspace_layout = new QVBoxLayout(workspace);
    auto *workspace_title = new QLabel(tr("Viewport"), workspace);
    QFont title_font = workspace_title->font();
    title_font.setPointSize(title_font.pointSize() + 4);
    title_font.setBold(true);
    workspace_title->setFont(title_font);
    workspace_title->setAlignment(Qt::AlignCenter);
    auto *workspace_hint = new QLabel(tr("Open a project to start an engine viewport."), workspace);
    workspace_hint->setAlignment(Qt::AlignCenter);
    workspace_hint->setWordWrap(true);
    workspace_layout->addStretch();
    workspace_layout->addWidget(workspace_title);
    workspace_layout->addWidget(workspace_hint);
    workspace_layout->addStretch();
    setCentralWidget(workspace);

    workspace_hint->hide();
    workspace_layout->setStretch(0, 0);
    workspace_layout->setStretch(3, 0);
    viewport_ = new EmbeddedViewport(workspace);
    viewport_->bindSelectionModel(&selection_model_);
    workspace->setHole(viewport_);
    // BS-1s, decisions 2-4: above the viewport while the engine draws its
    // placeholder because the rendering setup was refused at start. Its
    // hooks reach panels made further down; they run only once it shows.
    file_open_action_ = [](const std::filesystem::path &path) {
        return QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(path.wstring())));
    };
    render_fallback_ = new RenderFallbackNotice(
        RenderFallbackNotice::Hooks{
            .reload = [this] {
                if (render_reload_) render_reload_->reloadNow();
            },
            .open_graph = [this](const QString &node) {
                if (graph_canvas_ == nullptr || render_tabs_ == nullptr) return;
                docks_[FramePlanDock]->show();
                docks_[FramePlanDock]->raise();
                render_tabs_->setCurrentWidget(graph_canvas_);
                graph_canvas_->revealNodeWhenPresent(node);
            },
            .open_file = [this](const QString &document) {
                const QString root = viewport_->projectRoot();
                if (root.isEmpty()) return;
                const auto path = std::filesystem::path{root.toStdWString()} /
                                  std::filesystem::path{document.toStdWString()};
                if (!file_open_action_ || !file_open_action_(path))
                    statusBar()->showMessage(tr("Could not open %1.").arg(document), 8000);
            },
            .restart = [this](const QString &reason) { requestEngineRestart(reason); },
            .read_status = [this] {
                QString error;
                (void)viewport_->requestRpc(QStringLiteral("get_status"), QJsonObject{}, &error);
            },
            // Decision 8: Show the document reads and restores it.
            .request = [this](const std::string &method, const nlohmann::json &params,
                              std::string *error) -> std::int64_t {
                const auto object =
                    QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
                QString message;
                const qint64 id =
                    viewport_->requestRpc(QString::fromStdString(method), object, &message);
                if (id == 0 && error != nullptr) *error = message.toStdString();
                return id;
            },
            .schedule = [this](int milliseconds, std::function<void()> task) {
                QTimer::singleShot(milliseconds, this, std::move(task));
            },
            .restored = [this](const nlohmann::json &commit) {
                // Every panel reads the documents again, as after a reload.
                renderDocumentsRestored(commit, false);
                // PB-1s: the restore committed; the list follows.
                if (problems_dock_ != nullptr) problems_dock_->readNow();
            },
            // PB-1s: a document whose reload was refused is written as the
            // person's own editor writes it; RL-1s's watch reloads it.
            .write_file = [this](const std::string &reference, const std::string &content) -> std::string {
                const QString root = viewport_->projectRoot();
                std::string relative = reference;
                constexpr std::string_view project = "project://";
                if (relative.starts_with(project)) relative.erase(0, project.size());
                if (root.isEmpty() || relative.empty() || relative.find("://") != std::string::npos)
                    return "it is not a file of this project";
                QFile file{QDir{root}.filePath(QString::fromStdString(relative))};
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return file.errorString().toStdString();
                const auto bytes = QByteArray::fromStdString(content);
                if (file.write(bytes) != bytes.size()) return file.errorString().toStdString();
                return {};
            },
            .shown_changed = [this](bool shown, const QString &line) {
                if (shown) {
                    statusBar()->showMessage(line, 15000);
                } else if (render_fallback_ != nullptr &&
                           render_fallback_->session().reading().state ==
                               RenderPipelineReading::State::Ready) {
                    statusBar()->showMessage(tr("The project's rendering setup runs again."), 8000);
                }
            },
        },
        workspace);
    workspace_layout->addWidget(render_fallback_);
    workspace_layout->addWidget(viewport_, 1);
    migration_panel_ = new ProjectMigrationWidget(
        [this](const QString &method, const QJsonObject &params,
               QString *error) {
            return viewport_->requestMigrationRpc(method, params, error);
        },
        workspace);
    migration_panel_->hide();
    // No stretch of its own: while the viewport is hidden its document list
    // takes the space, and once migrated it is one line above the viewport.
    workspace_layout->addWidget(migration_panel_);

    project_list_ = new QListWidget(this);
    project_list_->addItem(tr("No project open"));
    project_list_->setEnabled(false);
    project_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    docks_[ProjectDock] =
        makeDock(this, tr("Project"), QStringLiteral("pelican.projectDock"), project_list_);
    // HW-2s / AV-1s: the asset folders beside Project -- Reload when saved
    // outside on each row, and the contents check against a manifest.
    assets_dock_ = new AssetsDock(
        AssetsDock::Hooks{
            .send = [this](const QString &method, const QJsonObject &params,
                           QString *error) { return viewport_->requestRpc(method, params, error); },
            .restart = [this](const QString &reason) { requestEngineRestart(reason); },
            .render_documents =
                [this] {
                    std::vector<std::filesystem::path> paths;
                    if (render_reload_)
                        for (const auto &[reference, document] : render_reload_->documents())
                            paths.push_back(document.path);
                    return paths;
                },
            // PB-1s: the check's warnings and errors are rows of Problems.
            .checked =
                [this](const std::vector<AssetStoreVerification> &stores) {
                    if (problems_dock_ != nullptr) problems_dock_->setAssetIssues(stores);
                },
        },
        this);
    assets_dock_->setObjectName(QStringLiteral("pelican.assetsPanel"));
    docks_[AssetsPanelDock] =
        makeDock(this, tr("Assets"), QStringLiteral("pelican.assetsDock"), assets_dock_);
    // MT-1s: the materials beside Assets -- the list, the editor made from
    // the surface's declaration, New material… and Assign to selected
    // object. The Inspector is built after it, so the hooks read it late.
    materials_dock_ = new MaterialsDock(
        MaterialsDock::Hooks{
            .send = [this](const QString &method, const QJsonObject &params,
                           QString *error) { return viewport_->requestRpc(method, params, error); },
            .restart = [this](const QString &reason) { requestEngineRestart(reason); },
            .selected_object =
                [this]() -> std::optional<MaterialsDock::SelectedObject> {
                    const auto subject = inspector_ != nullptr ? inspector_->materialSubject() : std::nullopt;
                    if (!subject) return std::nullopt;
                    return MaterialsDock::SelectedObject{subject->first, subject->second};
                },
            .materials_changed =
                [this] {
                    if (inspector_ != nullptr) inspector_->materialsChanged();
                },
            .bound =
                [this](const std::string &) {
                    if (inspector_ != nullptr) inspector_->materialBindingsChanged();
                },
        },
        this);
    materials_dock_->setObjectName(QStringLiteral("pelican.materialsPanel"));
    docks_[MaterialsPanelDock] =
        makeDock(this, tr("Materials"), QStringLiteral("pelican.materialsDock"), materials_dock_);

    outliner_ = new OutlinerTree(this);
    // The tree itself is named, not only its dock: picking a scene object is
    // the first step of every attach, and a scenario has to be able to name
    // the thing it clicks in.
    outliner_->setObjectName(QStringLiteral("pelican.outliner"));
    outliner_->setHeaderLabel(tr("Scene / Object"));
    outliner_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    connect(outliner_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
                selectOutlinerItem(current);
            });
    // OL-1: Duplicate (Ctrl+D), Delete (Del) and a row dragged onto another.
    connect(outliner_->duplicateAction(), &QAction::triggered, this,
            [this] { duplicateSelectedObject(); });
    connect(outliner_->deleteAction(), &QAction::triggered, this,
            [this] { deleteSelectedObject(); });
    outliner_->setDropAction([this](const OutlinerTree::Drop &drop) {
        moveObject(drop.dragged, drop.onto, drop.preserve_local);
    });
    outliner_->setRefusedDropAction([this](const OutlinerTree::Drop &drop) {
        if (drop.onto_runtime) {
            const QString message = RuntimeOutliner::editRefusal();
            statusBar()->showMessage(message, 8000);
            setPanelNotice(outliner_notice_, message);
            return;
        }
        refuseObjectMove(drop.dragged, drop.onto);
    });
    updateOutlinerActions();
    // Q-7: Add object says here, under the tree it adds to, when its object
    // has to wait for an edit or cannot be added.
    auto *outliner_panel = new QWidget(this);
    auto *outliner_layout = new QVBoxLayout(outliner_panel);
    outliner_layout->setContentsMargins(0, 0, 0, 0);
    outliner_layout->setSpacing(2);
    outliner_layout->addWidget(outliner_, 1);
    outliner_notice_ = new QLabel(outliner_panel);
    outliner_notice_->setObjectName(QStringLiteral("pelican.outlinerNotice"));
    applyPanelNoticeStyle(outliner_notice_);
    outliner_notice_->hide();
    outliner_layout->addWidget(outliner_notice_);
    docks_[OutlinerDock] = makeDock(this, tr("Outliner"),
                                    QStringLiteral("pelican.outlinerDock"), outliner_panel);

    inspector_ = new InspectorWidget(viewport_, this);
    docks_[InspectorDock] = makeDock(this, tr("Inspector"),
                                     QStringLiteral("pelican.inspectorDock"),
                                     inspector_);
    // PM-3c: the objects the running game spawned, as greyed "(runtime)"
    // rows of the Outliner, shown read-only in the Inspector.
    runtime_outliner_ = new RuntimeOutliner(*viewport_, *outliner_,
                                            [this] { runtimeObjectsChanged(); }, this);

    auto *output = new QPlainTextEdit(this);
    output->setReadOnly(true);
    output->setPlainText(tr("Pelican Studio ready."));
    docks_[OutputDock] = makeDock(this, tr("Output"), QStringLiteral("pelican.outputDock"), output);

    engine_log_ = new QPlainTextEdit(this);
    engine_log_->setObjectName(QStringLiteral("pelican.engineLog"));
    engine_log_->setReadOnly(true);
    engine_log_->setUndoRedoEnabled(false);
    engine_log_->setLineWrapMode(QPlainTextEdit::NoWrap);
    engine_log_->setPlaceholderText(tr("Engine output will appear here."));
    docks_[EngineLogDock] = makeDock(
        this, tr("Engine Log"), QStringLiteral("pelican.engineLogDock"), engine_log_);
    // PB-1s: what is wrong with the project now -- the engine's list and the
    // contents check -- beside Output and the Engine Log; a click on a row
    // goes where it is fixed, and the status bar counts the rows.
    problems_dock_ = new ProblemsDock(
        ProblemsDock::Hooks{
            .send = [this](const QString &method, const QJsonObject &params,
                           QString *error) { return viewport_->requestRpc(method, params, error); },
            .read_status = [this] {
                QString error;
                (void)viewport_->requestRpc(QStringLiteral("get_status"), QJsonObject{}, &error);
            },
            .open = [this](const ProblemRow &row) { openProblem(row); },
            .restart = [this](const QString &reason) { requestEngineRestart(reason); },
            .counted = [this](std::size_t errors, std::size_t warnings) {
                if (problems_count_ == nullptr) return;
                problems_count_->setText(QString::fromStdString(problemsCountLine(errors, warnings)));
            },
        },
        this);
    problems_dock_->setObjectName(QStringLiteral("pelican.problemsPanel"));
    docks_[ProblemsPanelDock] =
        makeDock(this, tr("Problems"), QStringLiteral("pelican.problemsDock"), problems_dock_);
    // Decision 4: the count on the status bar; a click brings Problems forward.
    problems_count_ = new QPushButton(this);
    problems_count_->setObjectName(QStringLiteral("pelican.problemsCount"));
    problems_count_->setFlat(true);
    problems_count_->setToolTip(tr("Show the Problems window"));
    problems_count_->setText(QString::fromStdString(problemsCountLine(0, 0)));
    problems_count_->hide();
    connect(problems_count_, &QPushButton::clicked, this, [this] { showProblems(); });
    statusBar()->addPermanentWidget(problems_count_);
    render_fallback_->setSeeProblems(true);
    auto *render_tabs = new QTabWidget(this);
    render_tabs_ = render_tabs;
    render_tabs->setObjectName(
        QStringLiteral("pelican.renderTools"));
    frame_plan_ = new FramePlanWidget(viewport_, render_tabs);
    render_features_ = new RenderFeaturesWidget(
        viewport_, render_tabs);
    graph_canvas_ = new GraphCanvasWidget(
        graphCanvasReadCapability(*viewport_), render_tabs);
    render_tabs->addTab(frame_plan_, tr("Frame Plan"));
    render_tabs->addTab(render_features_, tr("Features"));
    // The common graph is a tab of the render tools too, not a new dock:
    // one more dock would be a new place to lose, and this is a read-only view
    // of the same render configuration the other two tabs already describe.
    render_tabs->addTab(graph_canvas_, tr("Graph"));
    // RSX, decision 3: the rendering section's keys no other panel writes.
    // Last, so the tabs before it keep their places (and Ctrl+Tab its steps).
    rendering_settings_ = new RenderingSettingsWidget(viewport_, render_tabs);
    render_tabs->addTab(rendering_settings_, tr("Rendering settings"));
    docks_[FramePlanDock] = makeDock(
        this, tr("Frame Plan"), QStringLiteral("pelican.framePlanDock"),
        withRenderBanner(render_tabs, QStringLiteral("renderTools")));
    fullscreen_pass_ = new FullscreenPassWidget(
        framePlanReadCapability(*viewport_),
        renderPassAuthoringCapability(*viewport_), this);
    // F-7. The form is taller than most screens leave a bottom dock (close
    // to a thousand pixels at its minimum), and a dock may not be smaller
    // than what it holds: showing it pushed the dock area up until the
    // viewport was a strip, and QMainWindow keeps the area's height when the
    // dock closes again. Scrolled, the dock's floor is the scroll area's, so
    // the dock takes the height it is given and hands it back.
    auto *pass_editor_scroll = new QScrollArea(this);
    pass_editor_scroll->setObjectName(
        QStringLiteral("pelican.fullscreenPassScroll"));
    pass_editor_scroll->setWidgetResizable(true);
    pass_editor_scroll->setFrameShape(QFrame::NoFrame);
    pass_editor_scroll->setWidget(
        withRenderBanner(fullscreen_pass_, QStringLiteral("passEditor")));
    docks_[FullscreenPassDock] = makeDock(
        this, tr("Pass Editor"),
        QStringLiteral("pelican.fullscreenPassDock"), pass_editor_scroll);
    fullscreen_pass_->setCommittedAction([this] { frame_plan_->refresh(); });
    // T-1: "New target: <pass>_color..." in the Pass Editor's colour output
    // opens the Frame Plan's New render target form on that name; the
    // target it makes comes back to the Pass Editor as that output.
    // VI-3b: the Pass Editor's Fix sockets is the Frame Plan's.
    fullscreen_pass_->setFixSocketsAction(
        [this](const QString &graph, const QString &pass) {
            frame_plan_->fixSockets(graph, pass);
        });
    // LP-4e: and its Decompose into loop is the Frame Plan's too.
    fullscreen_pass_->setDecomposeAction(
        [this](const QString &graph, const QString &pass) {
            frame_plan_->decomposeMaterialPass(graph, pass);
        });
    fullscreen_pass_->setNewRenderTargetAction([this](const QString &name) {
        frame_plan_->openNewRenderTarget(name);
    });
    frame_plan_->setRenderTargetCreatedAction([this](const QString &name) {
        fullscreen_pass_->renderTargetCreated(name);
    });
    // T-1: a shader band on the canvas completes to the shaders the Pass
    // Editor lists.
    frame_plan_->setInlineFieldChoices([this](const QString &field) {
        QStringList choices;
        if (field != QLatin1String{"shader.fragment"} &&
            field != QLatin1String{"shader.vertex"})
            return choices;
        std::optional<std::filesystem::path> root;
        if (project_model_) root = project_model_->projectRoot();
        for (const auto &reference : fullscreenShaderChoices(
                 field == QLatin1String{"shader.vertex"} ? FullscreenShaderStage::vertex
                                                         : FullscreenShaderStage::fragment,
                 root))
            choices.push_back(QString::fromStdString(reference));
        return choices;
    });
    // F-5: a shader reload's log also goes under the Pass Editor's form.
    frame_plan_->setShaderReloadLogAction([this](const QString &message) {
        fullscreen_pass_->showShaderReloadLog(message);
    });
    connect(render_features_, &RenderFeaturesWidget::renderConfigCommitted, this, [this] { frame_plan_->refresh(); });
    frame_plan_->setNodeOrderAction([this](const QString &graph, const QStringList &names, quint64 generation) {
        render_features_->editFrameNodeOrder(graph, names, generation);
    });
    frame_plan_->setTaskEditAction([this](const QString &task) { render_features_->editComputeTask(task); });
    // LP-4b: the Task Editor's View field reads the task's axes from the
    // frame plan this panel holds.
    render_features_->setTaskViewAxesProvider([this](const QString &task) {
        return frame_plan_->taskViewAxes(task);
    });
    frame_plan_->setEngineRestartAction([this](const QString &reason) { requestEngineRestart(reason); });
    // RSX, decision 1: every panel that sends a render edit restarts the
    // engine through the same restart when a commit asks for one.
    render_features_->setEngineRestartAction([this](const QString &reason) { requestEngineRestart(reason); });
    rendering_settings_->setEngineRestartAction([this](const QString &reason) { requestEngineRestart(reason); });
    fullscreen_pass_->setEngineRestartAction([this](const QString &reason) { requestEngineRestart(reason); });
    inspector_->setEngineRestartAction([this](const QString &reason) { requestEngineRestart(reason); });
    // "Open the source" in the Inspector lands on the same node in the Graph
    // tab: the producer it names is a root node of the common graph.
    inspector_->setGraphSourceRevealAction(
        [this, render_tabs](const QString &node, const QString &) {
            docks_[FramePlanDock]->show();
            docks_[FramePlanDock]->raise();
            render_tabs->setCurrentWidget(graph_canvas_);
            graph_canvas_->revealNode(node);
        });
    // Q-12. "Open in Graph" on a task a render.compute node made -- from the
    // Frame Plan's right-click and from the Task Editor -- lands on that node
    // in the Graph tab, in the scope and instance MG-3b's graph_node names.
    const auto open_graph_node = [this, render_tabs](const QString &scope,
                                                     const QString &node,
                                                     const QString &instance) {
        if (!openGraphNodeInTabs(*render_tabs, *graph_canvas_, scope, node,
                                 instance))
            return false;
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        return true;
    };
    frame_plan_->setGraphNodeOpenAction(open_graph_node);
    // LP-4c, decision 10: a pass a loop expanded opens the body node it
    // copies, entered through its loop.
    frame_plan_->setGraphLoopNodeOpenAction(
        [this, render_tabs](const QString &loop, qint64 element, const QString &node) {
            if (!graph_canvas_->revealLoopOrigin(loop, element, node)) return false;
            render_tabs->setCurrentWidget(graph_canvas_);
            docks_[FramePlanDock]->show();
            docks_[FramePlanDock]->raise();
            return true;
        });
    // LP-4d2, decision 5: a zone's Show in Frame Plan brings the Frame Plan
    // forward with the loop's copies picked.
    graph_canvas_->setFramePlanLoopAction([this, render_tabs](const QString &loop) {
        render_tabs->setCurrentWidget(frame_plan_);
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        frame_plan_->showLoopGroup(loop);
        return true;
    });
    // Q-18: a Graph tab edit that committed (a loop's count, a branch, a view
    // axis) reads the Frame Plan again, now or when it is next shown. Not
    // through the shared digest: that moves on every placement too -- the
    // Frame Plan's own node positions are the Graph tab's set_layout -- and
    // would read the whole plan again after each drag.
    graph_canvas_->setPlanCommitAction([this] { frame_plan_->planChangedByCommit(); });
    // UX-6a, decision 2: one sender of reload_shaders, the Frame Plan's
    // follower, for the shader files either tab's passes read; after a
    // reload the Graph reads its sockets again, in front or not.
    graph_canvas_->setShaderSourcesAction([this](NodeShaderSources sources) {
        frame_plan_->setExtraShaderSources(std::move(sources));
    });
    frame_plan_->setShadersReloadedAction([this] { graph_canvas_->shadersReloaded(); });
    // UX-6a, decision 4: the Physical plan's Remove TRANSFER_SRC edits the
    // image in the graph document.
    frame_plan_->setDropTransferSourceAction(
        [this](const QString &resource, const std::optional<FramePlanLoopOrigin> &origin) {
            if (!origin) return graph_canvas_->removeImageTransferSource(resource, {}, 0, {});
            return graph_canvas_->removeImageTransferSource(
                resource, QString::fromStdString(origin->loop), origin->element,
                QString::fromStdString(origin->node));
        });
    // GL-1b: a graph edit the engine can only apply by starting again goes
    // through the Frame Plan's restart (G-4 / G-5), not a second one.
    graph_canvas_->setEngineRestartAction([this](const QString &reason) { requestEngineRestart(reason); });
    // LP-4e: a decomposed material pass is edited as its loop, so the Graph
    // tab comes to the front with that loop selected.
    frame_plan_->setLoopDecomposedAction([this, render_tabs](const QString &loop) {
        render_tabs->setCurrentWidget(graph_canvas_);
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        graph_canvas_->revealNodeWhenPresent(loop);
    });
    // LP-4f2: a feature replaced by its loop version is edited as that loop,
    // so the Graph tab comes to the front with it selected -- as LP-4e's
    // decompose does. The Frame Plan's right-click on a feature's group asks
    // the Features tab, which owns the feature list and the request.
    render_features_->setLoopVersionAppliedAction([this, render_tabs](const QString &loop) {
        render_tabs->setCurrentWidget(graph_canvas_);
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        graph_canvas_->revealNodeWhenPresent(loop);
    });
    frame_plan_->setLoopVersionActions(
        [this](const QString &feature) { return render_features_->loopVersionOffered(feature); },
        [this, render_tabs](const QString &feature) {
            render_tabs->setCurrentWidget(render_features_);
            render_features_->replaceWithLoopVersion(feature);
        });
    render_features_->setGraphNodeOpenAction(open_graph_node);
    // UX-12b, decision 2: a feature refused for a pass the pipeline lacks
    // ("Place Deferred lighting first") places that pass on the Graph tab,
    // its New... open on the template of that name (UX-5b's path).
    render_features_->setPlaceInsertionAnchorAction([this, render_tabs](const QString &anchor) {
        render_tabs->setCurrentWidget(graph_canvas_);
        return graph_canvas_->placeNamedFullscreenPass(anchor);
    });
    // GL-1, decision 6: the Frame Plan's Open in Graph on any other pass
    // selects the node it is drawn under on the Graph canvas.
    frame_plan_->setGraphPassOpenAction([this, render_tabs](const QString &pass) {
        render_tabs->setCurrentWidget(graph_canvas_);
        if (!graph_canvas_->revealPlanPass(pass)) return false;
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        return true;
    });
    // GL-5c, decision 3: Edit pass / Edit task and a double-click open what
    // the Graph tab edits there (through the Open in Graph above).
    frame_plan_->setGraphEditsPassQuery(
        [this](const QString &pass) { return graph_canvas_->editsPlanPass(pass); });
    // GL-5c, decisions 1 and 2: on a graph root, the Frame Plan's Add material
    // pass / fullscreen pass / compute task is the Graph's palette, on its op.
    // A root the Graph tab has read and found not to be a graph keeps the
    // Frame Plan's own forms.
    frame_plan_->setGraphPlaceAction([this, render_tabs](const QString &op) {
        const GraphCanvasModel &model = graph_canvas_->model();
        if (model.contextReceived() && !model.graphRoot()) return false;
        render_tabs->setCurrentWidget(graph_canvas_);
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        graph_canvas_->placeFromPalette(op);
        return true;
    });
    // GL-2b, decision 3: a pass row of the Graph tab's Layers section picks
    // that plan node in the Frame Plan.
    graph_canvas_->setFramePlanNodeAction([this, render_tabs](const QString &node) {
        render_tabs->setCurrentWidget(frame_plan_);
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        frame_plan_->showPlanNode(node);
    });
    // UX-11, decision 4: a material pass's Materials drawn row picks the
    // Frame Plan's Materials rows of that pass.
    graph_canvas_->setFramePlanMaterialsAction([this, render_tabs](const QString &pass) {
        render_tabs->setCurrentWidget(frame_plan_);
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        frame_plan_->showMaterialRoutes(pass);
    });
    // Decision 4: "Expand to edit…" inside a folded view runs the flow that
    // already exists for it, with its confirmation.
    graph_canvas_->setFoldedExpandActions(
        [this, render_tabs](const QString &graph, const QString &pass) {
            render_tabs->setCurrentWidget(frame_plan_);
            frame_plan_->decomposeMaterialPass(graph, pass);
        },
        [this, render_tabs](const QString &feature) {
            render_tabs->setCurrentWidget(render_features_);
            render_features_->replaceWithLoopVersion(feature);
        });
    inspector_->setRayGroupAction([this, render_tabs](const QString &group) {
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        render_tabs->setCurrentWidget(frame_plan_);
        frame_plan_->openTasksForHitGroup(group);
    });
    frame_plan_->setTaskConnectionAction([this](const QString &task, const QStringList &outputs) {
        render_features_->editComputeTaskConnection(task, outputs);
    });
    frame_plan_->setPresetPassEditAction([this](const QString &graph, const QString &pass) {
        fullscreen_pass_->editPresetPass(graph, pass);
        docks_[FullscreenPassDock]->show();
        docks_[FullscreenPassDock]->raise();
    });
    frame_plan_->setPresetPassConnectionAction([this](const QString &graph, const QString &pass, const QStringList &outputs) {
        fullscreen_pass_->editPresetPassConnection(graph, pass, outputs);
        docks_[FullscreenPassDock]->show();
        docks_[FullscreenPassDock]->raise();
    });
    // N-8. The canvas edits through the authoring RPCs that already exist, and
    // keeps its placement in the same `layout` member the Graph tab writes --
    // through that tab's debounce, because `set_layout` replaces the whole
    // object and two writers would erase each other.
    frame_plan_->setEditCapability(framePlanEditCapability(*viewport_));
    // Q-13. One digest for the rendering document, shared by every panel
    // that edits it: a commit in any of them -- a node's placement included --
    // is news to the others, which read their context again rather than send
    // the revision they read before it.
    render_digest_ = std::make_shared<RenderConfigDigest>();
    frame_plan_->setRenderConfigDigest(render_digest_);
    fullscreen_pass_->setRenderConfigDigest(render_digest_);
    graph_canvas_->setRenderConfigDigest(render_digest_);
    render_features_->setRenderConfigDigest(render_digest_);
    rendering_settings_->setRenderConfigDigest(render_digest_);
    // UX-6b: one reader of the frame's GPU time and the plan's size for the
    // two panels that show them; every request passes it (createRenderHistory
    // hands it the observer's calls).
    plan_perf_ = new PlanPerfHub(
        *viewport_,
        PlanPerfHub::Hooks{
            .watched = [this] { return frame_plan_->isVisible() || graph_canvas_->isVisible(); },
            .changed =
                [this] {
                    const QString summary = plan_perf_->summary();
                    const QString delta = plan_perf_->delta();
                    frame_plan_->setPerfLines(summary, delta);
                    graph_canvas_->setPerfLines(summary, delta);
                    graph_canvas_->setPlanNodeTimes(plan_perf_->model().nodeMs());
                },
        },
        this);
    // RU-1: and one history of their edits.
    createRenderHistory(pass_editor_scroll);
    // RL-1s: and what is written to their files from outside Studio.
    createRenderReload();
    frame_plan_->setLayoutCapability(FramePlanLayoutCapability{
        .read = [this] { return graph_canvas_->scene().layoutMember("frame_plan"); },
        .write = [this](const nlohmann::json &frame_plan, QString *reason) {
            const GraphCanvasModel &model = graph_canvas_->model();
            if (!model.contextReceived()) {
                if (reason != nullptr)
                    *reason = tr("Node positions are not saved yet: the "
                                 "rendering document has not been read.");
                return false;
            }
            if (!model.graphRoot()) {
                if (reason != nullptr)
                    *reason = tr("Node positions are not saved: %1 is a legacy "
                                 "rendering document (its root is not "
                                 "pelican.graph), so it has nowhere to keep "
                                 "them.")
                                  .arg(QString::fromStdString(
                                      model.sourceReference().empty()
                                          ? std::string{"the rendering document"}
                                          : model.sourceReference()));
                return false;
            }
            graph_canvas_->scene().setLayoutMember("frame_plan", frame_plan);
            return true;
        },
    });
    frame_plan_->setPassInsertAction(
        [this](const QString &graph, const QString &after) {
            fullscreen_pass_->insertAuthoredPassAfter(graph, after);
            docks_[FullscreenPassDock]->show();
            docks_[FullscreenPassDock]->raise();
        });
    frame_plan_->setTaskInsertAction([this, render_tabs] {
        docks_[FramePlanDock]->show();
        docks_[FramePlanDock]->raise();
        render_tabs->setCurrentWidget(render_features_);
    });
    connect(viewport_, &EmbeddedViewport::engineOutputReceived, this,
            [this](const QString &output) { appendEngineOutput(output); });
    connect(viewport_, &EmbeddedViewport::engineProcessExitedWithFailure, this,
            [this](const QString &) {
                // MG-1: a project that was not opened (Don't Open ends its
                // migration player) has nothing in the log to look at.
                if (!project_model_ && !migration_panel_->active()) return;
                docks_[EngineLogDock]->show();
                docks_[EngineLogDock]->raise();
            });
    connect(viewport_, &EmbeddedViewport::viewportPointerPressed, this,
            [this](const QPoint &pixel_position) {
                beginViewportPointer(pixel_position);
            });
    connect(viewport_, &EmbeddedViewport::viewportPointerMoved, this,
            [this](const QPoint &pixel_position) {
                moveViewportPointer(pixel_position);
            });
    connect(viewport_, &EmbeddedViewport::viewportPointerReleased, this,
            [this](const QPoint &pixel_position) {
                releaseViewportPointer(pixel_position);
            });
    connect(viewport_, &EmbeddedViewport::pickObjectSucceeded, this,
            [this](qint64 request_id, const QByteArray &result_json) {
                completeViewportPick(request_id, result_json);
            });
    connect(viewport_, &EmbeddedViewport::pickObjectFailed, this,
            [this](qint64 request_id, const QString &message) {
                failViewportPick(request_id, message);
            });
    // MG-1. The player stopped at the gate. Either Studio already knew (the
    // offline open refused the project first) and the panel is waiting for
    // it, or the player's check was the first to say so.
    connect(viewport_, &EmbeddedViewport::projectMigrationRequired, this,
            [this](const QString &refusal) {
                if (!migration_panel_->active()) {
                    if (!project_model_) return;
                    const auto root = project_model_->projectRoot();
                    beginProjectMigration(root, refusal);
                    return;
                }
                migration_panel_->engineReady();
            });
    connect(migration_panel_, &ProjectMigrationWidget::migrated, this,
            [this](const QString &root, const QString &backup_line) {
                if (!backup_line.isEmpty()) {
                    appendEngineOutput(backup_line + QLatin1Char('\n'));
                }
                // Out of the reply that said so: opening the project may ask
                // a question of its own.
                QTimer::singleShot(0, this, [this, root] {
                    viewport_->show();
                    // The documents are current now: the offline open reads
                    // them, and the migration player gives way to an engine.
                    openProjectNow(root);
                });
            });
    connect(migration_panel_, &ProjectMigrationWidget::closeRequested, this,
            [this](const QString &) {
                endProjectMigration();
                viewport_->closeProject(tr("The project was not opened."));
                statusBar()->showMessage(
                    tr("The project was not opened; nothing was changed."),
                    8000);
            });
    connect(viewport_, &EmbeddedViewport::inspectorRpcRejected, this,
            [this](qint64 request_id, const QString &error_code,
                   const QString &message) {
                (void)migration_panel_->acceptRpcRejection(
                    request_id, error_code, message);
            });
    connect(viewport_, &EmbeddedViewport::engineRpcBecameAvailable, this,
            [this] {
                migration_panel_->engineOpenedWithoutMigrating();
                pending_outliner_request_ = 0;
                // UX-13c: whether this engine knows scene_outline is found
                // out once, by asking it.
                outliner_uses_scene_tree_ = false;
                outliner_ask_unconditionally_ = false;
                setOutlinerRefreshInterval(OutlinerRefreshMinimumIntervalMs);
                outliner_refresh_timer_->start();
                requestOutlinerRefresh();
                pending_gizmo_requests_.clear();
                gizmo_model_.startSession();
                dispatchGizmoModel();
                // RL-1s: what this engine composes is what is watched.
                if (render_reload_) render_reload_->engineReady();
                assets_dock_->engineAvailable();
                materials_dock_->engineAvailable();
                problems_dock_->engineAvailable();
                problems_count_->show();
            });
    connect(viewport_, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this](const QString &) {
                // RU-1: what the history had in flight went with the engine;
                // the history stays (its documents are on disk).
                if (render_history_) render_history_->abandonInFlight();
                if (render_reload_) render_reload_->engineGone();
                render_fallback_->engineGone();
                assets_dock_->engineUnavailable();
                materials_dock_->engineUnavailable();
                problems_dock_->engineUnavailable();
                problems_count_->hide();
                if (pending_navigation_ != Navigation::None) {
                    cancelNavigation(tr("The engine disconnected. The project remains open."));
                }
                outliner_refresh_timer_->stop();
                pending_outliner_request_ = 0;
                // A spawn still waiting went with the engine (Q-7), and so
                // did any Outliner edit (OL-1).
                setPanelNotice(outliner_notice_, {});
                pending_spawn_name_.reset();
                pending_created_selection_.reset();
                pending_outliner_edit_.reset();
                pending_gizmo_requests_.clear();
                gizmo_model_.stopSession();
                dispatchGizmoModel();
            });
    connect(viewport_, &EmbeddedViewport::engineInputActiveChanged, this,
            [this](bool) { updateModalTransformPolling(); });
    connect(viewport_, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 request_id, const QByteArray &result_json) {
                if (migration_panel_->acceptRpcResult(request_id, result_json))
                    return;
                // BS-1s, decision 8: Show the document's reads and restore. A
                // committed restore goes on, to be read as every commit.
                if (render_fallback_->receiveResult(request_id, result_json)) return;
                // HW-2s / AV-1s: the Assets dock's settings edits and checks.
                if (assets_dock_->receiveResult(request_id, result_json)) return;
                // MT-1s: the Materials dock's reads and writes.
                if (materials_dock_->receiveResult(request_id, result_json)) return;
                // PB-1s: the engine's list of problems.
                if (problems_dock_->receiveResult(request_id, result_json)) return;
                // PM-3a: Pause, Step and the reads that follow them.
                if (play_controls_ &&
                    play_controls_->receiveResult(request_id, result_json))
                    return;
                // RL-1s: the reload's own requests; and every committed render
                // edit's `documents`, which is what Studio wrote (decision 2).
                // Only an answer that can be one is parsed.
                if (render_reload_ && request_id != 0 &&
                    (render_reload_->owns(request_id) ||
                     result_json.contains("\"committed\""))) {
                    nlohmann::json result;
                    try {
                        result = nlohmann::json::parse(
                            result_json.constData(),
                            result_json.constData() + result_json.size());
                    } catch (const std::exception &) {
                        result = nlohmann::json{};
                    }
                    if (render_reload_->receiveResult(request_id, result)) return;
                    render_reload_->observeReply(result);
                    // BS-1s, decision 3: an edit that committed while the
                    // engine draws its placeholder may have ended it.
                    render_fallback_->observeReply(result);
                    // PB-1s: and a committed edit clears the render
                    // pipeline's problems.
                    if (!problems_dock_->session().rows().empty() && result.is_object() &&
                        (result.value("status", std::string{}) == "committed" ||
                         result.value("committed", false)))
                        problems_dock_->readNow();
                }
                // RU-1: the render history's own reads, restores and polls,
                // and the replies to the edits it follows.
                if (render_history_ && render_history_->waitingFor(request_id)) {
                    nlohmann::json result;
                    try {
                        result = nlohmann::json::parse(
                            result_json.constData(),
                            result_json.constData() + result_json.size());
                    } catch (const std::exception &error) {
                        render_history_->receiveFailure(request_id, error.what());
                        return;
                    }
                    render_history_->receiveResult(request_id, result);
                    return;
                }
                if (request_id != 0 && request_id == navigation_revision_request_) {
                    navigation_revision_request_ = 0;
                    try {
                        const auto result = nlohmann::json::parse(result_json.constData(),
                            result_json.constData() + result_json.size());
                        const auto &revision = result.at("scene_revision");
                        if (!revision.is_number_unsigned()) {
                            throw std::runtime_error("invalid scene revision");
                        }
                        inspector_->observeSceneRevision(revision.get<std::uint64_t>());
                        continueNavigation();
                    } catch (const std::exception &) {
                        cancelNavigation(tr("Could not check unsaved changes. The project remains open."));
                    }
                    return;
                }
                completeOutlinerRpc(request_id, result_json);
                completeGizmoRpc(request_id, result_json);
            });
    connect(viewport_, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 request_id, const QString &message) {
                if (migration_panel_->acceptRpcFailure(request_id, message))
                    return;
                if (render_fallback_->receiveFailure(request_id, message)) return;
                if (assets_dock_->receiveFailure(request_id, message)) return;
                if (materials_dock_->receiveFailure(request_id, message)) return;
                if (problems_dock_->receiveFailure(request_id, message)) return;
                if (play_controls_ &&
                    play_controls_->receiveFailure(request_id, message))
                    return;
                if (render_reload_ &&
                    render_reload_->receiveFailure(request_id, message.toStdString()))
                    return;
                if (render_history_ && render_history_->waitingFor(request_id)) {
                    render_history_->receiveFailure(request_id, message.toStdString());
                    return;
                }
                if (request_id != 0 && request_id == navigation_revision_request_) {
                    cancelNavigation(tr("Could not check unsaved changes: %1").arg(message));
                    return;
                }
                failOutlinerRpc(request_id, message);
                failGizmoRpc(request_id, message);
            });
    modal_transform_timer_ = new QTimer(this);
    modal_transform_timer_->setObjectName(
        QStringLiteral("pelican.modalTransformPollTimer"));
    modal_transform_timer_->setInterval(GizmoModalActivePollIntervalMs);
    connect(modal_transform_timer_, &QTimer::timeout, this, [this] {
        gizmo_model_.pollModalTransform(
            inspector_->gizmoTransformBinding());
        dispatchGizmoModel();
    });
    outliner_refresh_timer_ = new QTimer(this);
    outliner_refresh_timer_->setObjectName(
        QStringLiteral("pelican.outlinerRefreshTimer"));
    outliner_refresh_interval_ms_ = OutlinerRefreshMinimumIntervalMs;
    outliner_refresh_timer_->setInterval(outliner_refresh_interval_ms_);
    connect(outliner_refresh_timer_, &QTimer::timeout, this,
            [this] { requestOutlinerRefresh(); });
    inspector_->setSceneStateChangedAction([this] {
        // A successful save may replace the project and its engine. Wait until
        // the Inspector has finished handling the reply before doing either.
        QTimer::singleShot(0, this, [this] { sceneStateChanged(); });
    });
    inspector_->setSceneEditCompletedAction(
        [this](const SceneEditOutcome &outcome) { sceneEditCompleted(outcome); });
}

void MainWindow::createRenderHistory(QWidget *pass_editor) {
    render_history_.emplace(RenderEditHistory::Transport{
        .request = [this](const std::string &method, const nlohmann::json &params,
                          std::string *error) -> std::int64_t {
            const auto object =
                QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
            QString message;
            const qint64 id =
                viewport_->requestRpc(QString::fromStdString(method), object, &message);
            if (id == 0 && error != nullptr) *error = message.toStdString();
            return id;
        },
        .schedule = [this](int milliseconds, std::function<void()> task) {
            QTimer::singleShot(milliseconds, this, std::move(task));
        },
    });
    render_history_->changed = [this] { renderHistoryChanged(); };
    render_history_->restored = [this](const nlohmann::json &commit, bool restart) {
        renderDocumentsRestored(commit, restart);
    };
    // Every request any panel sends passes here; the history hears only the
    // render edits (and asks for the documents before each is written).
    const auto json_of = [](const QJsonObject &params) {
        return nlohmann::json::parse(
            QJsonDocument(params).toJson(QJsonDocument::Compact).toStdString());
    };
    viewport_->setRpcObserver(EmbeddedViewport::RpcObserver{
        .before = [this, json_of](const QString &method, const QJsonObject &params) {
            if (plan_perf_) plan_perf_->requestGoing(method, params);
            const auto name = method.toStdString();
            if (!render_history_ || !RenderEditHistory::followsMethod(name)) return;
            render_history_->beforeRequest(name, json_of(params));
        },
        .sent = [this](qint64 id, const QString &method, const QJsonObject &params) {
            if (plan_perf_) plan_perf_->requestSent(id, method, params);
            const auto name = method.toStdString();
            if (!render_history_ || !RenderEditHistory::followsMethod(name)) return;
            render_history_->requestSent(id, name, nlohmann::json::object());
        },
    });

    render_undo_action_ = new QAction(tr("Undo Render Edit"), this);
    render_undo_action_->setObjectName(QStringLiteral("pelican.edit.undoRenderEdit"));
    render_undo_action_->setShortcut(QKeySequence::Undo);
    render_redo_action_ = new QAction(tr("Redo Render Edit"), this);
    render_redo_action_->setObjectName(QStringLiteral("pelican.edit.redoRenderEdit"));
    render_redo_action_->setShortcuts(
        {QKeySequence{Qt::CTRL | Qt::Key_Y}, QKeySequence{Qt::CTRL | Qt::SHIFT | Qt::Key_Z}});
    // The keys belong to the panel that holds the focus: these panels and
    // what is inside them. A text field inside takes Ctrl+Z for its own text
    // first (it claims the key before any shortcut); the Graph tab and the
    // Inspector are not among them and keep their own Undo.
    for (QAction *action : {render_undo_action_, render_redo_action_})
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(render_undo_action_, &QAction::triggered, this,
            [this] { render_history_->undo(); });
    connect(render_redo_action_, &QAction::triggered, this,
            [this] { render_history_->redo(); });
    frame_plan_->setRenderHistoryActions(render_undo_action_, render_redo_action_);
    // GL-1e: a line into a rendering-section pass sent from the Graph tab is a
    // render edit too; the tab's own Undo reaches it through these.
    graph_canvas_->setRenderHistoryActions(render_undo_action_, render_redo_action_);
    pass_editor->addAction(render_undo_action_);
    pass_editor->addAction(render_redo_action_);
    render_features_->setRenderHistoryActions({render_undo_action_, render_redo_action_});
    rendering_settings_->setRenderHistoryActions({render_undo_action_, render_redo_action_});
    renderHistoryChanged();
}

void MainWindow::renderHistoryChanged() {
    if (!render_history_ || render_undo_action_ == nullptr) return;
    const auto &history = *render_history_;
    render_undo_action_->setEnabled(history.canUndo());
    render_redo_action_->setEnabled(history.canRedo());
    const auto undo_label = QString::fromStdString(history.undoLabel());
    const auto redo_label = QString::fromStdString(history.redoLabel());
    // RU-1b: an edit that created or removed files is undone like any other;
    // Undo stops only where the history says why.
    const auto *barrier = !history.undoStack().empty() && history.undoStack().back().barrier
                              ? &history.undoStack().back()
                              : nullptr;
    render_undo_action_->setToolTip(
        undo_label.isEmpty() ? tr("Nothing to undo")
        : barrier != nullptr
            ? tr("Cannot undo past \"%1\": %2")
                  .arg(undo_label, QString::fromStdString(barrier->barrier_reason))
            : tr("Undo %1").arg(undo_label));
    render_redo_action_->setToolTip(redo_label.isEmpty() ? tr("Nothing to redo")
                                                         : tr("Redo %1").arg(redo_label));
    if (history.lineSerial() != shown_render_history_line_) {
        shown_render_history_line_ = history.lineSerial();
        const auto line = QString::fromStdString(history.line());
        frame_plan_->showRenderHistoryLine(line);
        if (!line.isEmpty()) statusBar()->showMessage(line, 8000);
    }
}

void MainWindow::renderDocumentsRestored(const nlohmann::json &commit,
                                         bool restart_required) {
    // Every panel that edits the rendering document reads it again: the ones
    // that hold the root's digest are told through it, and the Frame Plan and
    // the Pass Editor also for a restore that left the root as it was (a
    // preset's pass).
    if (commit.contains("source_digest") && commit.at("source_digest").is_object())
        render_digest_->publish(
            commit.at("source_digest").value("hex", std::string{}), nullptr);
    frame_plan_->refresh();
    frame_plan_->refreshAuthoringContext();
    fullscreen_pass_->documentRestored();
    // G-4 / RS-1: the edit taken back (or the restore itself) is one the
    // engine runs only after a restart. RSX: the line every panel says.
    if (restart_required) {
        const auto completion = readRenderEditCompletion(commit);
        requestEngineRestart(QString::fromStdString(
            completion.restart_required ? completion.restart_line
                                        : renderEditRestartLine({})));
    }
}

void MainWindow::createRenderReload() {
    render_reload_.emplace(RenderDocumentReload::Transport{
        .request = [this](const std::string &method, const nlohmann::json &params,
                          std::string *error) -> std::int64_t {
            const auto object =
                QJsonDocument::fromJson(QByteArray::fromStdString(params.dump())).object();
            QString message;
            const qint64 id =
                viewport_->requestRpc(QString::fromStdString(method), object, &message);
            if (id == 0 && error != nullptr) *error = message.toStdString();
            return id;
        },
        .schedule = [this](int milliseconds, std::function<void()> task) {
            QTimer::singleShot(milliseconds, this, std::move(task));
        },
        // Decision 2: the engine's digests are the sha256 of the bytes on disk.
        .file_digest = [](const std::filesystem::path &path) -> std::optional<std::string> {
            QFile file(QString::fromStdWString(path.wstring()));
            if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!hash.addData(&file)) return std::nullopt;
            return hash.result().toHex().toStdString();
        },
    });
    // Decision 1: one watcher, the Frame Plan's, which already follows the
    // shaders (UX-6a); the documents ride beside them with their own hook.
    render_reload_->watchChanged = [this] {
        frame_plan_->setWatchedRenderDocuments(render_reload_->watchedFiles(),
                                               [this](const QString &) {
                                                   if (render_reload_)
                                                       render_reload_->fileChanged();
                                               });
    };
    render_reload_->notice = [this](const RenderDocumentReload::Notice &notice) {
        using Kind = RenderDocumentReload::Notice::Kind;
        InspectorNotice shown;
        shown.kind = notice.kind == Kind::Error         ? InspectorNoticeKind::Error
                     : notice.kind == Kind::Success     ? InspectorNoticeKind::Success
                     : notice.kind == Kind::Information ? InspectorNoticeKind::Information
                                                        : InspectorNoticeKind::None;
        // The line, then the rest for the tooltip and Copy message.
        shown.message =
            notice.detail.empty() ? notice.line : notice.line + "\n" + notice.detail;
        graph_canvas_->showDiskReloadNotice(shown);
        if (!notice.line.empty())
            statusBar()->showMessage(QString::fromStdString(notice.line), 8000);
        // PB-1s: a reload answered -- its refusal is listed, its commit
        // (no_change too) takes the render pipeline's problems away.
        if (problems_dock_ != nullptr) problems_dock_->readNow();
    };
    render_reload_->reloaded = [this](const nlohmann::json &commit) {
        renderDocumentsReloaded(commit);
    };
    // BS-1s, decision 4: a fixed setup only a restart can run.
    render_reload_->refusedWith = [this](const nlohmann::json &error) {
        if (render_fallback_ != nullptr) render_fallback_->observeReloadRefusal(error);
        if (problems_dock_ != nullptr) problems_dock_->readNow();
    };
    // IA-1s, decision 5: the images' initial assets are watched with the
    // documents.
    graph_canvas_->setInitialAssetsAction([this](std::vector<std::string> assets) {
        if (render_reload_) render_reload_->setInitialAssets(assets);
    });
    // Decision 5: by hand, whatever the digests say -- after a fix the
    // watcher missed, or to hear again why the file is refused.
    reload_from_disk_action_ = new QAction(tr("Reload from disk"), this);
    reload_from_disk_action_->setObjectName(QStringLiteral("pelican.graphCanvas.reloadFromDisk"));
    reload_from_disk_action_->setToolTip(
        tr("Read the rendering documents again from their files (reload_render_documents)"));
    connect(reload_from_disk_action_, &QAction::triggered, this, [this] {
        if (render_reload_) render_reload_->reloadNow();
    });
    graph_canvas_->setReloadFromDiskAction(reload_from_disk_action_);
}

void MainWindow::showProblems() {
    docks_[ProblemsPanelDock]->show();
    docks_[ProblemsPanelDock]->raise();
}

void MainWindow::openProblem(const ProblemRow &row) {
    using Place = ProblemRow::Place;
    const auto text = [](const std::string &value) { return QString::fromStdString(value); };
    const QString document = text(row.document);
    // A node of a refused reload is found in the text by its id.
    const QString node_text =
        row.node.empty() ? QString{} : QLatin1Char('"') + text(row.node) + QLatin1Char('"');
    switch (row.place) {
    case Place::GraphNode: {
        if (graph_canvas_ == nullptr || !showRenderToolsTab(QStringLiteral("graph"))) return;
        const QString node = text(row.node);
        const QString scope = row.definition.empty() ? QStringLiteral("root") : text(row.definition);
        if (graph_canvas_->canRevealGraphNode(scope, node)) {
            (void)graph_canvas_->revealGraphNode(scope, node, {});
            return;
        }
        if (row.definition.empty()) {
            // The Graph has not read the document yet: the node is selected
            // once it has.
            graph_canvas_->revealNodeWhenPresent(node);
            return;
        }
        if (!document.isEmpty()) {
            render_fallback_->showDocumentAt(document, 0, 0, node_text, row.source != "startup");
            return;
        }
        statusBar()->showMessage(tr("The Graph does not show %1 of %2.").arg(node, text(row.definition)), 8000);
        return;
    }
    case Place::Material:
        docks_[MaterialsPanelDock]->show();
        docks_[MaterialsPanelDock]->raise();
        if (!materials_dock_->revealMaterial(row.document, row.material))
            statusBar()->showMessage(tr("The Materials dock does not list %1.")
                                         .arg(row.material.empty() ? document : text(row.material)),
                                     8000);
        return;
    case Place::DocumentText:
        // A start's refusal: the engine holds the file as it is, and restores
        // it. A reload's: it runs the version before, so the file is written.
        render_fallback_->showDocumentAt(document, row.line, row.column, row.line > 0 ? QString{} : node_text,
                                         row.source != "startup");
        return;
    case Place::ShaderFile: {
        const QString root = viewport_->projectRoot();
        if (root.isEmpty() || document.isEmpty() || document.startsWith(QStringLiteral("engine://"))) {
            statusBar()->showMessage(tr("%1 is not a file of this project.").arg(document), 8000);
            return;
        }
        const auto path =
            std::filesystem::path{root.toStdWString()} / std::filesystem::path{document.toStdWString()};
        if (!file_open_action_ || !file_open_action_(path))
            statusBar()->showMessage(tr("Could not open %1.").arg(document), 8000);
        else if (row.line > 0)
            statusBar()->showMessage(tr("%1: line %2").arg(document).arg(row.line), 8000);
        return;
    }
    case Place::AssetFile:
        docks_[AssetsPanelDock]->show();
        docks_[AssetsPanelDock]->raise();
        if (!assets_dock_->revealFile(row.asset_path))
            statusBar()->showMessage(tr("The Assets dock does not show %1.").arg(document), 8000);
        return;
    case Place::RenderingSettings:
        (void)showRenderToolsTab(QStringLiteral("rendering_settings"));
        return;
    case Place::Features:
        (void)showRenderToolsTab(QStringLiteral("render_features"));
        return;
    case Place::Restart:
    case Place::None:
        return;
    }
}

void MainWindow::renderDocumentsReloaded(const nlohmann::json &commit) {
    // Decision 4: the reload is not an Undo entry, and what the histories
    // hold was written before the disk changed under them: both end here.
    if (render_history_) render_history_->cut("\xE2\x80\x94 reloaded from disk \xE2\x80\x94");
    graph_canvas_->documentsReloadedFromDisk();
    // BS-1s, decision 3: the placeholder may have given way.
    if (render_fallback_ != nullptr) render_fallback_->reloadCommitted();
    if (problems_dock_ != nullptr) problems_dock_->readNow();
    // The way back after any outside change (RU-1's restore): every panel
    // reads the documents again, and RSX's one reader hears a restart.
    renderDocumentsRestored(commit, readRenderEditCompletion(commit).restart_required);
}

void MainWindow::updateModalTransformPolling() {
    // Selection and toolbar work runs while the window is still being built,
    // before the clock this decides about exists.
    if (modal_transform_timer_ == nullptr || viewport_ == nullptr) return;
    const int interval = gizmoModalPollIntervalMs(
        viewport_->rpcReady(), gizmo_model_.gestureActive(),
        viewport_->engineInputActive());
    if (interval == 0) {
        modal_transform_timer_->stop();
        return;
    }
    if (modal_transform_timer_->interval() != interval) {
        modal_transform_timer_->setInterval(interval);
    }
    if (!modal_transform_timer_->isActive()) modal_transform_timer_->start();
}

void MainWindow::setOutlinerRefreshInterval(int interval_ms) {
    if (outliner_refresh_interval_ms_ == interval_ms) return;
    outliner_refresh_interval_ms_ = interval_ms;
    outliner_refresh_timer_->setInterval(interval_ms);
}

void MainWindow::applyStoredTheme() {
    const QString stored = storedStudioThemeName();
    if (!stored.isEmpty()) setCurrentStudioTheme(stored);
    applyStudioThemeToApplication();
}

void MainWindow::createThemeMenu(QMenu *view_menu) {
    QMenu *theme_menu = view_menu->addMenu(tr("&Theme"));
    theme_menu->setObjectName(QStringLiteral("pelican.themeMenu"));
    auto *group = new QActionGroup(theme_menu);
    group->setExclusive(true);
    const QString current = currentStudioThemeName();
    for (const QString &name : studioThemeNames()) {
        QAction *action = theme_menu->addAction(
            name.left(1).toUpper() + name.mid(1));
        // The name a scenario triggers. It is the theme's own id, so a third
        // theme in the table is addressable without touching this code.
        action->setObjectName(QStringLiteral("pelican.view.theme.") + name);
        action->setCheckable(true);
        action->setChecked(name == current);
        group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, name] { chooseTheme(name); });
    }
}

void MainWindow::chooseTheme(const QString &name) {
    if (!setCurrentStudioTheme(name)) return;
    applyStudioThemeToApplication();
    // Written the moment it is chosen: a Studio that is killed rather than
    // closed must still come back in the theme the user picked.
    storeStudioThemeName(name);
    for (QAction *action : findChildren<QAction *>()) {
        const QString prefix = QStringLiteral("pelican.view.theme.");
        if (!action->objectName().startsWith(prefix)) continue;
        action->setChecked(action->objectName().mid(prefix.size()) == name);
    }
    statusBar()->showMessage(tr("Theme: %1").arg(name), 3000);
}

void MainWindow::createMenus() {
    // Every action a scenario may have to fire carries an objectName, the way
    // every widget does. A menu entry has no place on screen until its popup
    // opens, so a name is the only handle a test has on it -- and firing it by
    // name is what lets a test skip the popup, which is a modal event loop.
    menuBar()->setObjectName(QStringLiteral("pelican.menuBar"));
    QMenu *file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->setObjectName(QStringLiteral("pelican.fileMenu"));
    QAction *new_project_action = file_menu->addAction(tr("&New Project..."));
    new_project_action->setObjectName(
        QStringLiteral("pelican.file.newProject"));
    connect(new_project_action, &QAction::triggered, this,
            [this]() { chooseNewProject(); });
    QAction *open_project_action = file_menu->addAction(tr("&Open Project..."));
    open_project_action->setObjectName(
        QStringLiteral("pelican.file.openProject"));
    connect(open_project_action, &QAction::triggered, this,
            [this]() { chooseProject(); });
    QAction *save_scene_action = file_menu->addAction(tr("&Save Scene"));
    save_scene_action_ = save_scene_action;
    save_scene_action->setObjectName(QStringLiteral("pelican.file.saveScene"));
    save_scene_action->setShortcut(QKeySequence::Save);
    connect(save_scene_action, &QAction::triggered, this, [this] {
        // PM-3b (12.4): the scene waits for Stop to be saved.
        if (play_controls_ && play_controls_->model().session()) {
            statusBar()->showMessage(
                tr("Stop the game first: Stop puts the scene back as it was when Play was "
                   "pressed, so it is saved from edit."),
                5000);
            return;
        }
        if (inspector_->saveScene()) {
            statusBar()->showMessage(tr("Saving scene..."), 3000);
        } else {
            statusBar()->showMessage(
                tr("Scene save is unavailable while an edit is active."),
                5000);
        }
    });
    // UX-8, decision 9: sounds copied into the project's assets/audio, the
    // Graph tab's Sound list's Import….
    QAction *import_sound_action = file_menu->addAction(tr("Import &Sound…"));
    import_sound_action->setObjectName(QStringLiteral("pelican.file.importSound"));
    connect(import_sound_action, &QAction::triggered, this,
            [this] { graph_canvas_->chooseSoundsToImport(); });
    file_menu->addSeparator();
    QAction *exit_action = file_menu->addAction(tr("E&xit"));
    exit_action->setObjectName(QStringLiteral("pelican.file.exit"));
    connect(exit_action, &QAction::triggered, this, &QWidget::close);

    // RU-1. Undo / Redo of the render edits. The entries carry Ctrl+Z and
    // Ctrl+Y for the menu to show; the keys themselves are taken only while
    // a render panel holds the focus (the Graph tab and the Inspector keep
    // their own).
    QMenu *edit_menu = menuBar()->addMenu(tr("&Edit"));
    edit_menu->setObjectName(QStringLiteral("pelican.editMenu"));
    edit_menu->addAction(render_undo_action_);
    edit_menu->addAction(render_redo_action_);

    QMenu *view_menu = menuBar()->addMenu(tr("&View"));
    view_menu->setObjectName(QStringLiteral("pelican.viewMenu"));
    createThemeMenu(view_menu);
    QMenu *panels_menu = view_menu->addMenu(tr("&Panels"));
    panels_menu->setObjectName(QStringLiteral("pelican.panelsMenu"));
    for (QDockWidget *dock : docks_) {
        QAction *toggle = dock->toggleViewAction();
        // `pelican.projectDock` is the dock; `pelican.view.panels.projectDock`
        // is the menu entry that shows and hides it. One name per thing, so a
        // scenario that fires the entry and then asks whether the dock is on
        // screen is asking about two different objects, as it should be.
        QString leaf = dock->objectName();
        if (leaf.startsWith(QStringLiteral("pelican.")))
            leaf = leaf.mid(QStringLiteral("pelican.").size());
        if (!leaf.isEmpty())
            toggle->setObjectName(
                QStringLiteral("pelican.view.panels.") + leaf);
        panels_menu->addAction(toggle);
    }
    // LP-4c: which tab of the render tools is in front, as named entries --
    // `pelican.view.renderTools.graph` and the like -- so a scenario that
    // edits in the Graph tab can look at what the Frame Plan made of it.
    QMenu *render_tools_menu = view_menu->addMenu(tr("&Render Tools"));
    render_tools_menu->setObjectName(QStringLiteral("pelican.renderToolsMenu"));
    const std::pair<QString, QString> render_tool_tabs[] = {
        {QStringLiteral("frame_plan"), tr("&Frame Plan")},
        {QStringLiteral("render_features"), tr("F&eatures")},
        {QStringLiteral("rendering_settings"), tr("&Rendering settings")},
        {QStringLiteral("graph"), tr("&Graph")},
    };
    for (const auto &[id, label] : render_tool_tabs) {
        QAction *show_tab = render_tools_menu->addAction(label);
        show_tab->setObjectName(QStringLiteral("pelican.view.renderTools.") + id);
        connect(show_tab, &QAction::triggered, this,
                [this, id = id]() { showRenderToolsTab(id); });
    }
    // HW-2s: the Assets dock brought to the front -- it shares a tab with
    // Project and the Outliner, and Panels only shows or hides it.
    QAction *show_assets = view_menu->addAction(tr("&Assets"));
    show_assets->setObjectName(QStringLiteral("pelican.view.assets"));
    connect(show_assets, &QAction::triggered, this, [this] {
        docks_[AssetsPanelDock]->show();
        docks_[AssetsPanelDock]->raise();
    });
    // GL-5c: and the Pass Editor, which the Frame Plan's Add no longer opens
    // on a graph root (its Add goes to the Graph's palette); its own New pass
    // and Save still add a pass there, and a preset root's Add still opens it.
    QAction *show_pass_editor = view_menu->addAction(tr("Pass &Editor"));
    show_pass_editor->setObjectName(QStringLiteral("pelican.view.passEditor"));
    connect(show_pass_editor, &QAction::triggered, this, [this] {
        docks_[FullscreenPassDock]->show();
        docks_[FullscreenPassDock]->raise();
    });
    // MT-1s: and the Materials dock, the tab after it.
    QAction *show_materials = view_menu->addAction(tr("&Materials"));
    show_materials->setObjectName(QStringLiteral("pelican.view.materials"));
    connect(show_materials, &QAction::triggered, this, [this] {
        docks_[MaterialsPanelDock]->show();
        docks_[MaterialsPanelDock]->raise();
    });
    // PB-1s: and Problems, a tab beside Output and the Engine Log.
    QAction *show_problems = view_menu->addAction(tr("P&roblems"));
    show_problems->setObjectName(QStringLiteral("pelican.view.problems"));
    connect(show_problems, &QAction::triggered, this, [this] { showProblems(); });

    QMenu *layout_menu = menuBar()->addMenu(tr("&Layout"));
    layout_menu->setObjectName(QStringLiteral("pelican.layoutMenu"));
    QAction *save_action = layout_menu->addAction(tr("&Save Layout As..."));
    save_action->setObjectName(QStringLiteral("pelican.layout.saveAs"));
    connect(save_action, &QAction::triggered, this, [this]() { saveLayoutPreset(); });

    restore_layout_menu_ = layout_menu->addMenu(tr("&Restore Layout"));
    restore_layout_menu_->setObjectName(
        QStringLiteral("pelican.layout.restoreMenu"));
    delete_layout_menu_ = layout_menu->addMenu(tr("&Delete Layout"));
    delete_layout_menu_->setObjectName(
        QStringLiteral("pelican.layout.deleteMenu"));
    connect(layout_menu, &QMenu::aboutToShow, this, [this]() { refreshLayoutMenus(); });

    layout_menu->addSeparator();
    QAction *default_action = layout_menu->addAction(tr("Reset to &Default"));
    default_action->setObjectName(
        QStringLiteral("pelican.layout.resetDefault"));
    connect(default_action, &QAction::triggered, this, [this]() {
        applyDefaultLayout();
        statusBar()->showMessage(tr("Default layout restored"), 3000);
    });

    layout_menu->addSeparator();
    QMenu *tool_layout_menu =
        layout_menu->addMenu(tr("&Frame Plan Layout"));
    tool_layout_menu->setObjectName(QStringLiteral("pelican.layout.toolMenu"));
    save_tool_layout_action_ =
        tool_layout_menu->addAction(tr("&Save Tool Layout As..."));
    save_tool_layout_action_->setObjectName(
        QStringLiteral("pelican.layout.tool.saveAs"));
    connect(save_tool_layout_action_, &QAction::triggered, this,
            [this]() { saveToolLayoutPreset(); });
    restore_tool_layout_menu_ =
        tool_layout_menu->addMenu(tr("&Restore Tool Layout"));
    restore_tool_layout_menu_->setObjectName(
        QStringLiteral("pelican.layout.tool.restoreMenu"));
    delete_tool_layout_menu_ =
        tool_layout_menu->addMenu(tr("&Delete Tool Layout"));
    delete_tool_layout_menu_->setObjectName(
        QStringLiteral("pelican.layout.tool.deleteMenu"));
    tool_layout_menu->addSeparator();
    QAction *default_tool_action =
        tool_layout_menu->addAction(tr("Reset Tool Layout to &Default"));
    default_tool_action->setObjectName(
        QStringLiteral("pelican.layout.tool.resetDefault"));
    connect(default_tool_action, &QAction::triggered, this, [this]() {
        frame_plan_->applyDefaultToolLayout();
        statusBar()->showMessage(tr("Default Frame Plan layout restored"),
                                 3000);
    });
    save_tool_layout_action_->setEnabled(false);
    restore_tool_layout_menu_->setEnabled(false);
    delete_tool_layout_menu_->setEnabled(false);
    default_tool_action->setEnabled(false);
    connect(tool_layout_menu, &QMenu::aboutToShow, this,
            [this, default_tool_action]() {
                const bool project_open = tool_layout_presets_.has_value();
                save_tool_layout_action_->setEnabled(project_open);
                restore_tool_layout_menu_->setEnabled(project_open);
                delete_tool_layout_menu_->setEnabled(project_open);
                default_tool_action->setEnabled(project_open);
            });

    auto *gizmo_toolbar = addToolBar(tr("Gizmo"));
    gizmo_toolbar->setObjectName(QStringLiteral("pelican.gizmoToolbar"));
    gizmo_toolbar->setMovable(true);
    auto *gizmo_group = new QActionGroup(gizmo_toolbar);
    gizmo_group->setExclusive(true);
    const auto add_mode = [&](const QString &label, const QString &object_name,
                              GizmoMode mode, const QString &tool_tip) {
        QAction *action = gizmo_toolbar->addAction(label);
        action->setObjectName(object_name);
        action->setCheckable(true);
        action->setToolTip(tool_tip);
        gizmo_group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode] { setGizmoMode(mode); });
        return action;
    };
    gizmo_mode_actions_[0] = add_mode(
        tr("Move"), QStringLiteral("pelican.gizmo.move"), GizmoMode::Translate,
        tr("Move the selection along one world axis"));
    gizmo_mode_actions_[1] = add_mode(
        tr("Rotate"), QStringLiteral("pelican.gizmo.rotate"), GizmoMode::Rotate,
        tr("Rotate the selection around one world axis"));
    gizmo_mode_actions_[2] = add_mode(
        tr("Scale"), QStringLiteral("pelican.gizmo.scale"), GizmoMode::Scale,
        tr("Scale the selection along one axis"));
    gizmo_mode_actions_[0]->setChecked(true);
    createPlayControls(gizmo_toolbar);

    createAddObjectMenu();
}

void MainWindow::createPlayControls(QToolBar *band) {
    gizmo_toolbar_ = band;
    play_controls_ = new PlayControls(
        *band, *this, *statusBar(),
        PlayControls::Hooks{
            .send = [this](const QString &method, const QJsonObject &params,
                           QString *error) {
                return viewport_->requestRpc(method, params, error);
            },
            .changed = [this] { updatePlayPresentation(); },
            .notice = [this](const QString &line) {
                statusBar()->showMessage(line, 8000);
            },
            .scene_idle = [this] { return !inspector_->sceneEditPending(); },
        },
        this);
    statusBar()->addPermanentWidget(play_controls_->retryButton());
    statusBar()->addPermanentWidget(play_controls_->statusLabel());
    // PM-3b: the engine starts in edit unless the configured arguments keep it
    // from doing so, and then says why.
    connect(viewport_, &EmbeddedViewport::engineLaunchNotice, this,
            [this](const QString &line) { statusBar()->showMessage(line, 15000); });
    // PM-3b (12.3): the Inspector's edits are named in Play changes, carry
    // "reverts on Stop" and a Keep link, and Take from game shows while the
    // game runs.
    inspector_->setPlaySessionHooks(InspectorWidget::PlaySessionHooks{
        .session = [this] { return play_controls_->model().session(); },
        .kept = [this](const std::string &transaction_id) {
            return play_controls_->model().kept(transaction_id);
        },
        .keep = [this](const std::string &transaction_id) {
            play_controls_->keep(QString::fromStdString(transaction_id));
        },
        .committed = [this](const CommittedEdit &edit) {
            play_controls_->noteCommittedEdit(edit);
        },
    });
    // 12.6: stopping the engine loses the run's scene edits; ask first.
    viewport_->setStopGuard([this](std::function<void()> stop) {
        return !askToEndPlaySession(tr("Stop the engine"), false, std::move(stop), {});
    });
    // UX-7, decision 10: read from the viewport's one parse of the reply.
    connect(viewport_, &EmbeddedViewport::engineStatusParsed, this,
            [this](qint64 request_id, const nlohmann::json &status) {
                play_controls_->observeStatus(request_id, status);
                // UX-9, decision 4: the voices on the Graph's bands.
                if (graph_canvas_ != nullptr) graph_canvas_->observeStatus(status);
                // HW-2s: the watch, the last reload and the contents check;
                // and, decision 7, whether the engine's own watch is open.
                assets_dock_->observeStatus(request_id, status);
                frame_plan_->observeEngineStatus(status);
                // BS-1s, decision 2: the engine's placeholder, and why.
                render_fallback_->observeStatus(status);
                // PB-1s, decision 2: the list, when its revision moved.
                problems_dock_->observeStatus(status);
            });
    connect(viewport_, &EmbeddedViewport::engineRpcRefused, this,
            [this](qint64 request_id, int code, const QString &message,
                   const QJsonValue &data) {
                play_controls_->receiveRefusal(request_id, code, message, data);
                assets_dock_->receiveRefusal(request_id, code, message, data);
                materials_dock_->receiveRefusal(request_id, code, message, data);
                // UX-13c: an engine without scene_outline (-32601).
                if (request_id != 0 && request_id == pending_outliner_request_ &&
                    code == -32601)
                    pending_outliner_unknown_method_ = true;
            });
    connect(viewport_, &EmbeddedViewport::engineRpcBecameAvailable, this,
            [this] { play_controls_->engineAvailable(); });
    // A restart, a project switch or a stop: the next engine starts playing,
    // and a paused frame from this one must not stay on screen (PM-3a).
    connect(viewport_, &EmbeddedViewport::engineRpcBecameUnavailable, this, [this](const QString &) {
        after_play_stop_ = nullptr;
        if (play_session_prompt_) {
            auto *prompt = play_session_prompt_.data();
            play_session_prompt_ = nullptr;
            prompt->reject();
        }
        play_controls_->engineUnavailable();
    });
    updatePlayPresentation();
}

QWidget *MainWindow::withRenderBanner(QWidget *panel, const QString &name) {
    // 12.4: while the game runs, what these panels send is written to files
    // and is not put back by Stop.
    auto *holder = new QWidget(this);
    holder->setObjectName(QStringLiteral("pelican.play.renderHolder.") + name);
    auto *layout = new QVBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *banner = new QLabel(
        tr("Changes here are written to files and stay after Stop."), holder);
    banner->setObjectName(QStringLiteral("pelican.play.renderBanner.") + name);
    banner->setWordWrap(true);
    banner->setContentsMargins(6, 3, 6, 3);
    banner->hide();
    layout->addWidget(banner);
    layout->addWidget(panel, 1);
    render_banners_.push_back(banner);
    return holder;
}

void MainWindow::stopPlayThen(std::function<void()> then) {
    if (!then) return;
    // A run is Playing or Paused as the controls show it; get_status's
    // play.session can still say no for a moment after Play, and then says
    // yes to the close that follows (which asks).
    const auto running = [this] {
        const PlayStateModel &model = play_controls_->model();
        return model.session() || model.playing() || model.paused();
    };
    if (play_controls_ == nullptr || !running()) {
        then();
        return;
    }
    // Stop is asked whenever the controls can send it (not while a read or
    // a change of theirs is in flight), and `then` runs once the run has
    // ended -- or after fifteen seconds whatever happened, so a Stop the
    // engine never answers does not hold the quit for ever.
    auto *poll = new QTimer(this);
    poll->setInterval(100);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto asked_at = std::make_shared<qint64>(-1);
    const auto step = [this, poll, clock, asked_at, running, then = std::move(then)] {
        const PlayStateModel &model = play_controls_->model();
        if ((!running() && !model.busy()) || clock->elapsed() > 15000) {
            poll->stop();
            poll->deleteLater();
            then();
            return;
        }
        if (running() && !model.busy() &&
            (*asked_at < 0 || clock->elapsed() - *asked_at > 2000) &&
            play_controls_->stopDiscarding())
            *asked_at = clock->elapsed();
    };
    connect(poll, &QTimer::timeout, this, step);
    poll->start();
    step();
}

bool MainWindow::askToEndPlaySession(const QString &action, bool stop_first,
                                     std::function<void()> proceed,
                                     std::function<void()> cancelled) {
    if (play_controls_ == nullptr || !play_controls_->model().session()) return false;
    if (play_session_prompt_) return true;
    auto *prompt = new QMessageBox(
        QMessageBox::Warning, tr("The Game Is Running"),
        tr("%1 ends the game's run. The scene edits made while it ran are lost (Keep "
           "marks are not used).")
            .arg(action),
        QMessageBox::NoButton, this);
    prompt->setObjectName(QStringLiteral("pelican.playSessionPrompt"));
    QPushButton *end = prompt->addButton(tr("End the Run"), QMessageBox::AcceptRole);
    end->setObjectName(QStringLiteral("pelican.playSessionPrompt.end"));
    QPushButton *cancel = prompt->addButton(QMessageBox::Cancel);
    cancel->setObjectName(QStringLiteral("pelican.playSessionPrompt.cancel"));
    prompt->setDefaultButton(cancel);
    prompt->setEscapeButton(cancel);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    play_session_prompt_ = prompt;
    connect(prompt, &QMessageBox::finished, this,
            [this, prompt, end, stop_first, proceed = std::move(proceed),
             cancelled = std::move(cancelled)](int) mutable {
                if (play_session_prompt_ != prompt) return;
                play_session_prompt_ = nullptr;
                if (prompt->clickedButton() != end) {
                    if (cancelled) cancelled();
                    return;
                }
                if (!stop_first) {
                    if (proceed) proceed();
                    return;
                }
                // Stopped first, with nothing kept, so the scene a Save after
                // this writes is the one from before Play.
                after_play_stop_ = std::move(proceed);
                if (!play_controls_->stopDiscarding()) {
                    after_play_stop_ = nullptr;
                    statusBar()->showMessage(tr("The game could not be stopped now."), 8000);
                    if (cancelled) cancelled();
                }
            });
    prompt->open();
    return true;
}

void MainWindow::updatePlayPresentation() {
    if (play_controls_ == nullptr || viewport_frame_ == nullptr) return;
    const PlayStateModel &model = play_controls_->model();
    const bool paused = model.paused();
    const bool playing = model.playing();
    const StudioTheme &theme = currentStudioTheme();
    // 12.2: edit keeps the window's colours; playing is bluish, paused
    // yellowish.
    const QColor frame = paused ? theme.play.paused_frame
                         : playing ? theme.play.playing_frame
                                   : QColor{};
    const QColor band = paused ? theme.play.paused_band
                        : playing ? theme.play.playing_band
                                  : QColor{};
    static_cast<PlayTintFrame *>(viewport_frame_)->setTint(frame);
    if (gizmo_toolbar_ != nullptr) {
        gizmo_toolbar_->setStyleSheet(
            band.isValid() ? QStringLiteral("QToolBar { background: %1; }").arg(band.name())
                           : QString{});
    }
    // 12.4: Save waits for Stop, and the render panels say their edits stay.
    const bool session = model.session();
    if (save_scene_action_ != nullptr) {
        save_scene_action_->setEnabled(!session);
        save_scene_action_->setToolTip(
            session ? tr("Stop the game first: while it runs, the scene is saved only after "
                         "Stop puts it back (play_session_active).")
                    : tr("Save the scene to its file"));
    }
    for (QLabel *banner : render_banners_) {
        banner->setVisible(session);
        banner->setStyleSheet(
            QStringLiteral("QLabel { background: %1; color: %2; }")
                .arg(theme.play.paused_band.name(), palette().color(QPalette::WindowText).name()));
    }
    if (inspector_ != nullptr) inspector_->playSessionChanged();
    if (inspector_ != nullptr && (!model.supported() || model.editing()))
        clean_in_edit_ = !inspector_->hasUnsavedSceneChanges();
    if (!shown_play_session_ && session) clean_at_play_ = clean_in_edit_;
    if (model.stopSerial() != shown_stop_serial_) {
        shown_stop_serial_ = model.stopSerial();
        // Stop put back the scene as it was when Play was pressed; if that
        // was the saved one and nothing was kept, there is nothing to save.
        if (clean_at_play_ && model.lastStop() && model.lastStop()->kept == 0 &&
            inspector_ != nullptr)
            inspector_->adoptRestoredRevisionAsSaved();
    }
    if (shown_play_session_ && !session) {
        // Stop put the scene back under a new revision: read it again now
        // rather than on the next watch (Q-16 would find it too).
        setOutlinerRefreshInterval(OutlinerRefreshMinimumIntervalMs);
        requestOutlinerRefresh();
        if (inspector_ != nullptr) inspector_->refreshFromEngine();
    }
    shown_play_session_ = session;
    if (after_play_stop_ && !model.busy()) {
        auto proceed = std::move(after_play_stop_);
        after_play_stop_ = nullptr;
        if (!session) {
            // After this call returns: it may close the window.
            QTimer::singleShot(0, this, std::move(proceed));
        } else if (pending_navigation_ != Navigation::None) {
            cancelNavigation(tr("The game could not be stopped. The project remains open."));
        }
    }
    // 12.6: the Timeline goes to a time only while the game's clock stands
    // still. An engine that reports no play state keeps the scrub it had.
    if (graph_canvas_ != nullptr) {
        graph_canvas_->setTimelineScrubBlocked(
            playing ? tr("The game is playing, so its clock moves on its own. "
                         "Pause it (Ctrl+Shift+P) to go to a time.")
                    : QString{});
    }
}

QStringList MainWindow::renderToolsTabIds() {
    return {QStringLiteral("frame_plan"), QStringLiteral("render_features"),
            QStringLiteral("rendering_settings"), QStringLiteral("graph")};
}

bool MainWindow::showRenderToolsTab(const QString &id) {
    QWidget *tab = nullptr;
    if (id == QLatin1String{"frame_plan"}) tab = frame_plan_;
    else if (id == QLatin1String{"render_features"}) tab = render_features_;
    else if (id == QLatin1String{"rendering_settings"}) tab = rendering_settings_;
    else if (id == QLatin1String{"graph"}) tab = graph_canvas_;
    if (tab == nullptr || render_tabs_ == nullptr) return false;
    docks_[FramePlanDock]->show();
    docks_[FramePlanDock]->raise();
    render_tabs_->setCurrentWidget(tab);
    return true;
}

void MainWindow::chooseProject() {
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Open Pelican Project"), {}, QFileDialog::ShowDirsOnly);
    if (!directory.isEmpty()) {
        openProject(directory);
    }
}

void MainWindow::chooseNewProject() {
    if (new_project_dialog_) {
        new_project_dialog_->raise();
        new_project_dialog_->activateWindow();
        return;
    }
    // T-1: the folder the last Create used is kept beside the layouts.
    auto *dialog = new NewProjectDialog(
        this, [this](const QString &root) { openProject(root); },
        application_config_directory_);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    new_project_dialog_ = dialog;
    dialog->open();
}

void MainWindow::openProject(const QString &path) {
    if (project_model_) {
        std::error_code error;
        if (std::filesystem::equivalent(filesystemPath(path), project_model_->projectRoot(), error) &&
            !error) return;
    }
    requestNavigation(Navigation::Project, path);
}

void MainWindow::requestEngineRestart(const QString &reason) {
    if (pending_navigation_ != Navigation::None) {
        // Another question is already on screen; the setting waits for the
        // next start rather than restarting under it.
        const QString declined = tr("The engine was not restarted. The setting is saved and applies the next time the engine starts.");
        statusBar()->showMessage(declined);
        frame_plan_->engineRestartDeclined(declined);
        graph_canvas_->engineRestartDeclined(declined);
        render_features_->engineRestartDeclined(declined);
        rendering_settings_->engineRestartDeclined(declined);
        return;
    }
    // Set first: with nothing unsaved the restart happens inside this call.
    pending_restart_reason_ = reason;
    // Q-9. The restart has begun as far as anyone waiting for the engine is
    // concerned. The engine keeps answering while the unsaved-edits question
    // below is asked (it is asked of that engine), but it is the one about to
    // be stopped: without the hold, `wait_engine` agreed with it and the next
    // request found the engine gone.
    viewport_->setRestartPending(true);
    requestNavigation(Navigation::Restart);
}

void MainWindow::requestNavigation(Navigation destination, const QString &path) {
    if (pending_navigation_ != Navigation::None) return;
    pending_navigation_ = destination;
    pending_project_path_ = path;
    checkNavigationRevision();
}

void MainWindow::checkNavigationRevision() {
    if (pending_navigation_ == Navigation::None) return;
    if (project_model_ && viewport_->rpcReady()) {
        QString error;
        navigation_revision_request_ = viewport_->requestRpc(
            QStringLiteral("get_scene_revision"), {}, &error);
        if (navigation_revision_request_ == 0) {
            cancelNavigation(tr("Could not check unsaved changes: %1").arg(error));
        }
        return;
    }
    continueNavigation();
}

void MainWindow::continueNavigation() {
    if (pending_navigation_ == Navigation::None) return;
    // PM-3b (12.6): the run ends first, asked.
    if (!play_session_confirmed_ && play_controls_ && play_controls_->model().session()) {
        const QString action = pending_navigation_ == Navigation::Restart
                                   ? tr("Restarting the engine")
                               : pending_navigation_ == Navigation::Close
                                   ? tr("Closing Pelican Studio")
                                   : tr("Opening another project");
        askToEndPlaySession(
            action, true,
            [this] {
                play_session_confirmed_ = true;
                // The scene is new (Stop put it back): ask about it again.
                checkNavigationRevision();
            },
            [this] { cancelNavigation(); });
        return;
    }
    if (!inspector_->hasUnsavedSceneChanges() && !inspector_->sceneEditPending()) {
        completeNavigation();
        return;
    }
    if (navigation_prompt_) return;
    const bool restarting = pending_navigation_ == Navigation::Restart;
    auto *prompt = new QMessageBox(QMessageBox::Warning, tr("Unsaved Scene Changes"),
        restarting ? tr("Save your scene changes before the engine restarts?")
                   : tr("Save your scene changes before leaving this project?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    if (restarting) {
        prompt->setInformativeText(tr("The engine restarts to apply a setting it reads only at start, and the scene it holds is read again from disk. Cancel keeps this engine running; the setting is saved and applies the next time the engine starts."));
    }
    navigation_prompt_ = prompt;
    prompt->setObjectName(QStringLiteral("pelican.unsavedScenePrompt"));
    // Named, so a scenario answers the prompt the way a person does.
    prompt->button(QMessageBox::Save)->setObjectName(QStringLiteral("pelican.unsavedScenePrompt.save"));
    prompt->button(QMessageBox::Discard)->setObjectName(QStringLiteral("pelican.unsavedScenePrompt.discard"));
    prompt->button(QMessageBox::Cancel)->setObjectName(QStringLiteral("pelican.unsavedScenePrompt.cancel"));
    prompt->setDefaultButton(QMessageBox::Save);
    prompt->setEscapeButton(QMessageBox::Cancel);
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    connect(prompt, &QMessageBox::finished, this, [this, prompt](int answer) {
        if (navigation_prompt_ != prompt) return;
        navigation_prompt_ = nullptr;
        answerNavigationPrompt(answer);
    });
    prompt->open();
}

void MainWindow::answerNavigationPrompt(int answer) {
    // Engine failure may cancel the navigation while the prompt is visible.
    // A later button click cannot resurrect that request.
    if (pending_navigation_ == Navigation::None) return;
    if (answer == QMessageBox::Discard) {
        completeNavigation();
    } else if (answer == QMessageBox::Save) {
        navigation_save_completion_ = inspector_->sceneSaveCompletion();
        navigation_waiting_for_save_ = true;
        if (!inspector_->sceneSaveInFlight() && !inspector_->saveScene()) {
            cancelNavigation(tr("Scene save is unavailable. Finish any active edit and retry. The project remains open."));
        } else {
            statusBar()->showMessage(pending_navigation_ == Navigation::Restart
                                         ? tr("Saving scene before the engine restarts...")
                                         : tr("Saving scene before leaving the project..."));
        }
    } else {
        cancelNavigation();
    }
}

void MainWindow::sceneStateChanged() {
    setWindowModified(inspector_->hasUnsavedSceneChanges());
    if (play_controls_ != nullptr &&
        (!play_controls_->model().supported() || play_controls_->model().editing()))
        clean_in_edit_ = !inspector_->hasUnsavedSceneChanges();
    if (!navigation_waiting_for_save_ ||
        inspector_->sceneSaveCompletion() == navigation_save_completion_) return;
    navigation_waiting_for_save_ = false;
    if (!inspector_->lastSceneSaveSucceeded()) {
        cancelNavigation(tr("The scene could not be saved. The project remains open."));
        return;
    }
    // An edit by another RPC client can land while saving. Ask once more so
    // successful persistence of revision N never discards a newer revision.
    checkNavigationRevision();
}

void MainWindow::cancelNavigation(const QString &message) {
    const auto destination = pending_navigation_;
    resetNavigation();
    if (destination == Navigation::Restart) {
        // The commit that asked for the restart is on disk: say so, on the
        // Frame Plan that made it and here, until something else is said.
        const QString declined = tr("The engine was not restarted. The setting is saved and applies the next time the engine starts.");
        statusBar()->showMessage(message.isEmpty() ? declined : message + QLatin1Char(' ') + declined);
        frame_plan_->engineRestartDeclined(declined);
        graph_canvas_->engineRestartDeclined(declined);
        render_features_->engineRestartDeclined(declined);
        rendering_settings_->engineRestartDeclined(declined);
        return;
    }
    if (!message.isEmpty()) statusBar()->showMessage(message, 8000);
}

void MainWindow::resetNavigation() {
    // Q-9. Declined, or about to go ahead -- completeNavigation restarts the
    // engine right after this, and restartEngine closes the gate itself.
    if (pending_navigation_ == Navigation::Restart) {
        viewport_->setRestartPending(false);
    }
    pending_navigation_ = Navigation::None;
    play_session_confirmed_ = false;
    if (play_session_prompt_ && after_play_stop_ == nullptr) {
        auto *prompt = play_session_prompt_.data();
        play_session_prompt_ = nullptr;
        prompt->reject();
    }
    pending_project_path_.clear();
    pending_restart_reason_.clear();
    navigation_revision_request_ = 0;
    navigation_waiting_for_save_ = false;
    if (navigation_prompt_) {
        auto *prompt = navigation_prompt_.data();
        navigation_prompt_ = nullptr;
        prompt->reject();
    }
}

void MainWindow::completeNavigation() {
    const auto destination = pending_navigation_;
    const auto path = pending_project_path_;
    const auto restart_reason = pending_restart_reason_;
    resetNavigation();
    if (destination == Navigation::Restart) {
        statusBar()->showMessage(restart_reason, 8000);
        viewport_->restartEngine(restart_reason);
    } else if (destination == Navigation::Close) {
        close_authorized_ = true;
        close();
    } else if (destination == Navigation::Project) {
        openProjectNow(path);
    }
}

void MainWindow::openProjectNow(const QString &path) {
    try {
        ProjectOutlinerModel opened_project =
            ProjectOutlinerModel::open(filesystemPath(path));
        saveLastToolLayout();
        outliner_refresh_timer_->stop();
        pending_outliner_request_ = 0;
        inspector_->resetSceneSaveState();
        pending_spawn_name_.reset();
        pending_created_selection_.reset();
        pending_outliner_edit_.reset();
        endGizmoSessionForProjectSwitch(opened_project.projectRoot());
        // RU-1: nothing in the render history is about the next project.
        if (render_history_) render_history_->clear();
        project_model_ = std::move(opened_project);
        // RL-1s: nor anything the reload watched.
        if (render_reload_) render_reload_->open(project_model_->projectRoot());
        assets_dock_->open(project_model_->projectRoot());
        materials_dock_->open(project_model_->projectRoot());
        tool_layout_presets_.emplace(
            application_config_directory_,
            displayPath(project_model_->projectRoot()));
        const QString tool_layout_notice = restoreLastToolLayout();
        selection_model_.bindProject(&*project_model_);
        populateOutliner();
        refreshSelectionViews();
        frame_plan_->setProjectRoot(project_model_->projectRoot());
        fullscreen_pass_->setProjectRoot(project_model_->projectRoot());
        graph_canvas_->setProjectRoot(project_model_->projectRoot());
        const QString opened_root = displayPath(project_model_->projectRoot());
        // MG-1: a migration just applied to this project leaves its one line
        // on screen; any other project closes the panel.
        if (migration_panel_->active() &&
            !(migration_panel_->stage() ==
                  ProjectMigrationWidget::Stage::migrated &&
              migration_panel_->projectRoot() == opened_root)) {
            endProjectMigration();
        }
        viewport_->show();
        viewport_->openProject(opened_root);
        if (viewport_->migrationGateOpen()) {
            // The player on this project is the migration server, which
            // holds nothing the editor can use. It gives way to an engine the
            // way every restart does.
            requestEngineRestart(
                tr("The project was migrated; the engine starts again to "
                   "open it."));
        }
        statusBar()->showMessage(
            tr("Opened %1. %2").arg(opened_root, tool_layout_notice), 5000);
    } catch (const ProjectMigrationRequiredError &required) {
        beginProjectMigration(required.projectRoot(),
                              QString::fromUtf8(required.what()));
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Could Not Open Project"),
                             QString::fromUtf8(error.what()));
    }
}

void MainWindow::beginProjectMigration(
    const std::filesystem::path &project_root, const QString &refusal) {
    leaveCurrentProject(project_root);
    const QString root = displayPath(project_root);
    migration_panel_->begin(root, refusal);
    viewport_->hide();
    migration_panel_->show();
    statusBar()->showMessage(
        tr("%1 was saved in an earlier format.").arg(root), 8000);
    const bool engine_on_this_project =
        viewport_->engineRunning() &&
        viewport_->projectRoot() == QDir(root).absolutePath();
    if (engine_on_this_project && !viewport_->migrationGateOpen()) {
        // An engine holds this project (or a player not yet heard from). It
        // is stopped first: a migration is applied only by a player that
        // started no engine (MG-1b), and this one starts again as that.
        requestEngineRestart(
            tr("The engine starts again to read the migration plan."));
    } else {
        viewport_->openProject(root);
    }
    // Already at the gate (the player said so first): ask now. Otherwise the
    // player's projectMigrationRequired asks.
    if (viewport_->migrationGateOpen()) migration_panel_->engineReady();
}

void MainWindow::endProjectMigration() {
    migration_panel_->end();
    migration_panel_->hide();
    viewport_->show();
}

void MainWindow::leaveCurrentProject(
    const std::filesystem::path &next_project_root) {
    saveLastToolLayout();
    outliner_refresh_timer_->stop();
    pending_outliner_request_ = 0;
    inspector_->resetSceneSaveState();
    pending_spawn_name_.reset();
    pending_created_selection_.reset();
    pending_outliner_edit_.reset();
    endGizmoSessionForProjectSwitch(next_project_root);
    if (render_history_) render_history_->clear();
    if (render_reload_) render_reload_->open({});
    assets_dock_->open({});
    materials_dock_->open({});
    selection_model_.bindProject(nullptr);
    project_model_.reset();
    tool_layout_presets_.reset();
    {
        const QSignalBlocker block_outliner{outliner_};
        outliner_->clear();
    }
    project_list_->clear();
    project_list_->addItem(tr("No project open"));
    project_list_->setEnabled(false);
    refreshSelectionViews();
    setWindowModified(false);
}

void MainWindow::setShaderSourceOpenAction(
    std::function<bool(const std::filesystem::path &)> action) {
    frame_plan_->setShaderSourceOpenAction(action);
    // BS-1s: the fallback notice's Open file opens the same way.
    file_open_action_ = action;
    fullscreen_pass_->setShaderSourceOpenAction(std::move(action));
}

void MainWindow::populateOutliner() {
    const QSignalBlocker block_outliner{outliner_};
    project_list_->clear();
    const auto &model = *project_model_;
    if (model.projectName()) {
        project_list_->addItem(QString::fromStdString(*model.projectName()));
    }
    project_list_->addItem(displayPath(model.projectRoot()));
    project_list_->setEnabled(true);

    // OL-1: a row that was open stays open, and one that was closed stays
    // closed, by the object's authoring id -- through a duplicate, a delete,
    // a move and an undo alike. A row not seen before opens if it is a scene
    // and stays closed if it is an object.
    QSet<QString> expanded;
    QSet<QString> collapsed;
    for (QTreeWidgetItemIterator row{outliner_}; *row != nullptr; ++row) {
        ((*row)->isExpanded() ? expanded : collapsed).insert(rowIdentity(*row));
    }

    outliner_->clear();
    for (const auto &scene : model.scenes()) {
        auto *scene_item = new QTreeWidgetItem(
            outliner_, QStringList{QString::fromStdString(scene.scene_id)});
        scene_item->setData(0, OutlinerSceneIdRole,
                            QString::fromStdString(scene.scene_id));

        std::vector<QTreeWidgetItem *> object_items;
        object_items.reserve(scene.objects.size());
        for (const auto &object : scene.objects) {
            auto *item = new QTreeWidgetItem(
                QStringList{QString::fromStdString(object.display_name)});
            item->setData(0, OutlinerSceneIdRole,
                          QString::fromStdString(object.key.scene_id));
            item->setData(
                0, OutlinerDeclarationIndexRole,
                QVariant::fromValue<qulonglong>(object.key.declaration_index));
            item->setData(
                0, OutlinerAuthoringIdRole,
                QVariant::fromValue<qulonglong>(object.key.authoring_object_id));
            object_items.push_back(item);
        }

        for (std::size_t index = 0; index < scene.objects.size(); ++index) {
            const auto parent = scene.objects[index].parent_declaration_index;
            if (parent) {
                object_items[*parent]->addChild(object_items[index]);
            } else {
                scene_item->addChild(object_items[index]);
            }
        }
    }
    for (QTreeWidgetItemIterator row{outliner_}; *row != nullptr; ++row) {
        const QString identity = rowIdentity(*row);
        (*row)->setExpanded(
            expanded.contains(identity) ||
            (!collapsed.contains(identity) && (*row)->parent() == nullptr));
    }
    // PM-3c: after the scene's own objects.
    if (runtime_outliner_ != nullptr) runtime_outliner_->appendRows();
}

void MainWindow::createAddObjectMenu() {
    add_object_menu_ = new QMenu(tr("Add Object"), this);
    add_object_menu_->setObjectName(QStringLiteral("pelican.outliner.addObjectMenu"));
    const std::array entries{
        std::pair{AddObjectKind::Empty, QStringLiteral("empty")},
        std::pair{AddObjectKind::Cube, QStringLiteral("cube")},
        std::pair{AddObjectKind::Plane, QStringLiteral("plane")},
        std::pair{AddObjectKind::Sphere, QStringLiteral("sphere")},
        std::pair{AddObjectKind::Floor, QStringLiteral("floor")},
    };
    for (const auto &[kind, id] : entries) {
        // The shapes read as the Inspector's model list reads them; an Empty
        // and a Floor (a Plane already placed) by their own names.
        const auto base = addObjectBaseName(kind);
        const std::string label =
            kind == AddObjectKind::Empty || kind == AddObjectKind::Floor
                ? std::string{base}
                : builtinModelLabel(addObjectDeclaration(kind, std::string{base})
                                        .at("components").at(1).at("model")
                                        .get<std::string>());
        QAction *action =
            add_object_menu_->addAction(QString::fromStdString(label));
        action->setObjectName(QStringLiteral("pelican.outliner.addObject.") + id);
        connect(action, &QAction::triggered, this,
                [this, kind = kind] { addObject(kind); });
    }

    // OL-1: the right-click menu is the row's first -- Duplicate and Delete,
    // switched off with nothing selected -- and then Add Object.
    outliner_menu_ = new QMenu(this);
    outliner_menu_->setObjectName(QStringLiteral("pelican.outliner.contextMenu"));
    outliner_menu_->addAction(outliner_->duplicateAction());
    outliner_menu_->addAction(outliner_->deleteAction());
    outliner_menu_->addSeparator();
    outliner_menu_->addMenu(add_object_menu_);
    outliner_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(outliner_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
                updateOutlinerActions();
                outliner_menu_->popup(outliner_->viewport()->mapToGlobal(position));
            });

    auto *scene_toolbar = addToolBar(tr("Scene"));
    scene_toolbar->setObjectName(QStringLiteral("pelican.sceneToolbar"));
    scene_toolbar->setMovable(true);
    auto *add_button = new QToolButton(scene_toolbar);
    add_button->setObjectName(QStringLiteral("pelican.outliner.addObjectButton"));
    add_button->setText(tr("Add Object"));
    add_button->setToolTip(tr("Add an empty or a primitive shape at the origin, "
                              "or a wide floor just under it"));
    add_button->setMenu(add_object_menu_);
    add_button->setPopupMode(QToolButton::InstantPopup);
    scene_toolbar->addWidget(add_button);
}

void MainWindow::addObject(AddObjectKind kind) {
    if (!project_model_ || !viewport_->rpcReady()) {
        statusBar()->showMessage(
            tr("Add Object needs an open project with its engine running."), 5000);
        return;
    }
    // The selected object's scene, else the first the engine reported. The
    // new object is never parented to the selection: reparenting is OL-1.
    const auto &scenes = project_model_->scenes();
    const auto &selected = selection_model_.selected();
    const auto scene = std::find_if(scenes.begin(), scenes.end(),
        [&selected](const OutlinerScene &candidate) {
            return !selected || candidate.scene_id == selected->scene_id;
        });
    if (scene == scenes.end()) {
        statusBar()->showMessage(
            tr("Add Object is unavailable until the Outliner has a scene."), 5000);
        return;
    }
    std::vector<std::string> taken;
    taken.reserve(scene->objects.size() + 1);
    for (const auto &object : scene->objects) taken.push_back(object.display_name);
    // A spawn the Outliner has not shown yet still holds its name.
    if (pending_spawn_name_ && pending_spawn_name_->first == scene->scene_id)
        taken.push_back(pending_spawn_name_->second);
    const std::string name = uniqueObjectName(addObjectBaseName(kind), taken);

    QString error;
    const auto outcome = inspector_->spawnObject(
        scene->scene_id, addObjectDeclaration(kind, name), &error);
    const QString shown_name = QString::fromStdString(name);
    if (outcome == SpawnOutcome::Refused) {
        const QString message = tr("Could not add %1: %2").arg(shown_name, error);
        statusBar()->showMessage(message, 8000);
        setPanelNotice(outliner_notice_, oneLine(message), message);
        return;
    }
    // OL-1: the new object is selected by the id its committed edit names
    // (sceneEditCompleted); the name is held only so the next Add Object
    // numbers past it.
    pending_spawn_name_ = std::pair{scene->scene_id, name};
    // Ask quickly: the new object is selected as soon as the Outliner has it.
    setOutlinerRefreshInterval(OutlinerRefreshMinimumIntervalMs);
    if (outcome == SpawnOutcome::Queued) {
        // Q-7: an Inspector edit is being committed. The object goes in
        // right after it, on the revision it leaves.
        const QString message = tr("Adding %1 after the current edit.").arg(shown_name);
        statusBar()->showMessage(message, 8000);
        setPanelNotice(outliner_notice_, message);
        return;
    }
    setPanelNotice(outliner_notice_, {});
    statusBar()->showMessage(tr("Adding %1...").arg(shown_name), 3000);
}

void MainWindow::selectCreatedObject() {
    if (!pending_created_selection_ || !project_model_) return;
    const OutlinerObjectKey key{.scene_id = pending_created_selection_->first,
                                .authoring_object_id = pending_created_selection_->second};
    // Not in the Outliner yet: the next scene_tree brings it.
    if (project_model_->findObject(key) == nullptr) return;
    pending_created_selection_.reset();
    setPanelNotice(outliner_notice_, {});
    const SelectionUpdate update = selection_model_.selectFromOutliner(key);
    if (update.kind == SelectionUpdateKind::failed) {
        statusBar()->showMessage(QString::fromStdString(update.message), 5000);
        return;
    }
    viewport_->setPickingNotice({});
    refreshSelectionViews();
}

void MainWindow::updateOutlinerActions() {
    const auto &selected = selection_model_.selected();
    const bool has_object = project_model_ && selected &&
                            project_model_->findObject(*selected) != nullptr;
    outliner_->duplicateAction()->setEnabled(has_object);
    outliner_->deleteAction()->setEnabled(has_object);
    // PM-3c: a runtime row is selected, and they are off because of it.
    const bool runtime = runtime_outliner_ != nullptr && !has_object &&
                         runtime_outliner_->model().selectedObject() != nullptr;
    outliner_->setEditRefusal(runtime ? RuntimeOutliner::editRefusal() : QString{});
}

bool MainWindow::submitOutlinerEdit(SceneEditKind kind, const OutlinerObject &subject,
                                    nlohmann::json operation, const QString &description,
                                    OutlinerEdit pending) {
    if (!project_model_ || !viewport_->rpcReady()) {
        statusBar()->showMessage(
            tr("%1 needs an open project with its engine running.").arg(description), 5000);
        return false;
    }
    QString error;
    const auto outcome = inspector_->submitSceneEdit(
        kind, subject.key.authoring_object_id, std::move(operation),
        description.toStdString(), &error);
    if (outcome == SpawnOutcome::Refused) {
        statusBar()->showMessage(error, 8000);
        setPanelNotice(outliner_notice_, oneLine(error), error);
        return false;
    }
    pending_outliner_edit_ = std::move(pending);
    setPanelNotice(outliner_notice_, {});
    setOutlinerRefreshInterval(OutlinerRefreshMinimumIntervalMs);
    statusBar()->showMessage(description + QStringLiteral("..."), 3000);
    return true;
}

void MainWindow::duplicateSelectedObject() {
    const auto &selected = selection_model_.selected();
    const OutlinerObject *object =
        project_model_ && selected ? project_model_->findObject(*selected) : nullptr;
    if (object == nullptr) {
        statusBar()->showMessage(tr("Select an object in the Outliner to duplicate it."), 5000);
        return;
    }
    (void)submitOutlinerEdit(
        SceneEditKind::Duplicate, *object, duplicateObjectOperation(*object),
        tr("Duplicate %1").arg(QString::fromStdString(object->display_name)),
        {.subject_id = object->key.authoring_object_id, .subject_name = object->display_name});
}

void MainWindow::deleteSelectedObject() {
    const auto &selected = selection_model_.selected();
    const OutlinerObject *object =
        project_model_ && selected ? project_model_->findObject(*selected) : nullptr;
    if (object == nullptr) {
        statusBar()->showMessage(tr("Select an object in the Outliner to delete it."), 5000);
        return;
    }
    // Asked once, and only when the object takes others with it: a lone
    // object comes back with one Undo.
    const std::size_t children = project_model_->descendantCount(*object);
    if (children == 0) {
        deleteObject(object->key);
        return;
    }
    const QString name = QString::fromStdString(object->display_name);
    auto *prompt = new QMessageBox(
        QMessageBox::Question, tr("Delete Object"),
        children == 1 ? tr("Delete %1? The 1 object under it is deleted too.").arg(name)
                      : tr("Delete %1? The %2 objects under it are deleted too.")
                            .arg(name)
                            .arg(static_cast<qulonglong>(children)),
        QMessageBox::NoButton, this);
    prompt->setObjectName(QStringLiteral("pelican.outliner.deleteConfirm"));
    prompt->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *confirm = prompt->addButton(tr("Delete"), QMessageBox::DestructiveRole);
    confirm->setObjectName(QStringLiteral("pelican.outliner.deleteConfirm.delete"));
    QPushButton *cancel = prompt->addButton(QMessageBox::Cancel);
    cancel->setObjectName(QStringLiteral("pelican.outliner.deleteConfirm.cancel"));
    prompt->setDefaultButton(cancel);
    const OutlinerObjectKey key = object->key;
    connect(prompt, &QMessageBox::buttonClicked, this,
            [this, confirm, key](QAbstractButton *button) {
                if (button == confirm) deleteObject(key);
            });
    // open(), not exec(): a scenario answers it as a person does.
    prompt->open();
}

void MainWindow::deleteObject(const OutlinerObjectKey &key) {
    const OutlinerObject *object = project_model_ ? project_model_->findObject(key) : nullptr;
    if (object == nullptr) return;
    (void)submitOutlinerEdit(
        SceneEditKind::Destroy, *object, destroyObjectOperation(*object),
        tr("Delete %1").arg(QString::fromStdString(object->display_name)),
        {.subject_id = object->key.authoring_object_id,
         .subject_name = object->display_name,
         .select_after = selectionAfterDestroy(*project_model_, *object)});
}

void MainWindow::moveObject(const OutlinerObjectKey &dragged,
                            const std::optional<OutlinerObjectKey> &onto,
                            bool preserve_local) {
    if (!project_model_) return;
    const OutlinerDropPlan plan = planOutlinerDrop(
        *project_model_, dragged, onto,
        preserve_local ? ReparentPreserve::Local : ReparentPreserve::World);
    if (plan.kind == OutlinerDropKind::Unchanged) return;
    if (plan.kind == OutlinerDropKind::Refused) {
        const QString message = QString::fromStdString(plan.refusal);
        statusBar()->showMessage(message, 8000);
        setPanelNotice(outliner_notice_, oneLine(message), message);
        return;
    }
    const OutlinerObject *child = project_model_->findObject(dragged);
    const OutlinerObject *parent = onto ? project_model_->findObject(*onto) : nullptr;
    const QString child_name = QString::fromStdString(child->display_name);
    (void)submitOutlinerEdit(
        SceneEditKind::Reparent, *child, plan.operation,
        parent != nullptr
            ? tr("Move %1 under %2").arg(child_name, QString::fromStdString(parent->display_name))
            : tr("Move %1 to the scene root").arg(child_name),
        {.subject_id = child->key.authoring_object_id,
         .subject_name = child->display_name,
         .target_name = parent != nullptr ? parent->display_name : std::string{}});
}

void MainWindow::refuseObjectMove(const OutlinerObjectKey &dragged,
                                  const std::optional<OutlinerObjectKey> &onto) {
    if (!project_model_) return;
    const OutlinerObject *child = project_model_->findObject(dragged);
    if (child == nullptr) return;
    QString message;
    if (onto) {
        const OutlinerDropPlan plan =
            planOutlinerDrop(*project_model_, dragged, onto, ReparentPreserve::World);
        message = QString::fromStdString(plan.refusal);
    }
    if (message.isEmpty())
        message = tr("%1 cannot be moved into another scene.")
                      .arg(QString::fromStdString(child->display_name));
    statusBar()->showMessage(message, 8000);
    setPanelNotice(outliner_notice_, oneLine(message), message);
}

void MainWindow::sceneEditCompleted(const SceneEditOutcome &outcome) {
    std::optional<OutlinerEdit> edit;
    if ((outcome.kind == SceneEditKind::Duplicate || outcome.kind == SceneEditKind::Destroy ||
         outcome.kind == SceneEditKind::Reparent) &&
        pending_outliner_edit_ && pending_outliner_edit_->subject_id == outcome.subject_id) {
        edit = std::move(pending_outliner_edit_);
        pending_outliner_edit_.reset();
    }
    std::string spawned_name;
    if (outcome.kind == SceneEditKind::Spawn && pending_spawn_name_) {
        spawned_name = pending_spawn_name_->second;
        pending_spawn_name_.reset();
    }
    if (!outcome.committed) {
        // An undo or redo the engine refused is the Inspector's to report,
        // as it always was.
        if (outcome.kind == SceneEditKind::Undo || outcome.kind == SceneEditKind::Redo) return;
        const QString message = QString::fromStdString(sceneEditRefusalSentence(
            outcome, edit ? edit->subject_name : spawned_name,
            edit ? edit->target_name : std::string{}));
        statusBar()->showMessage(message, 8000);
        setPanelNotice(outliner_notice_, oneLine(message), message);
        return;
    }
    switch (outcome.kind) {
    case SceneEditKind::Spawn:
    case SceneEditKind::Duplicate:
    case SceneEditKind::Undo:
    case SceneEditKind::Redo:
        // What the edit made (a copy's root, a new object, an object an undo
        // brought back) is selected by the id the engine gave it.
        if (const auto created = createdObjectToSelect(
                outcome.created_objects,
                outcome.kind == SceneEditKind::Duplicate ? outcome.subject_id : 0)) {
            pending_created_selection_ =
                std::pair{created->scene_id, created->authoring_object_id};
            selectCreatedObject();
        }
        break;
    case SceneEditKind::Destroy:
        if (edit && edit->select_after && project_model_ &&
            project_model_->findObject(*edit->select_after) != nullptr) {
            const SelectionUpdate update =
                selection_model_.selectFromOutliner(*edit->select_after);
            if (update.applied()) {
                viewport_->setPickingNotice({});
                refreshSelectionViews();
            }
        }
        break;
    case SceneEditKind::Reparent:
        // The selection is the object, by id, wherever it now hangs.
        break;
    }
    setOutlinerRefreshInterval(OutlinerRefreshMinimumIntervalMs);
    requestOutlinerRefresh();
}

void MainWindow::requestOutlinerRefresh() {
    if (!project_model_ || pending_outliner_request_ != 0 ||
        !viewport_->rpcReady()) {
        return;
    }
    // UX-13c: scene_outline, not scene_tree: ids, order, names and parents
    // only. Once a scene is held, the poll names its revision and an
    // unchanged scene is answered with not_modified and no objects.
    QJsonObject params;
    if (!outliner_uses_scene_tree_ && !outliner_ask_unconditionally_ &&
        project_model_->sceneRevision() && project_model_->scenes().size() == 1) {
        params.insert(QStringLiteral("scene_id"),
                      QString::fromStdString(project_model_->scenes().front().scene_id));
        params.insert(QStringLiteral("if_scene_revision"),
                      static_cast<qint64>(*project_model_->sceneRevision()));
    }
    QString error;
    pending_outliner_is_tree_ = outliner_uses_scene_tree_;
    pending_outliner_unknown_method_ = false;
    pending_outliner_request_ = viewport_->requestRpc(
        outliner_uses_scene_tree_ ? QStringLiteral("scene_tree")
                                  : QStringLiteral("scene_outline"),
        params, &error);
    if (pending_outliner_request_ == 0 && !error.isEmpty()) {
        statusBar()->showMessage(
            tr("Outliner refresh is unavailable: %1").arg(error), 5000);
    }
}

void MainWindow::completeOutlinerRpc(
    qint64 request_id, const QByteArray &result_json) {
    if (request_id != pending_outliner_request_) return;
    pending_outliner_request_ = 0;
    try {
        const auto result = nlohmann::json::parse(std::string{
            result_json.constData(),
            static_cast<std::size_t>(result_json.size())});
        if (const auto revision = result.find("scene_revision");
            revision != result.end() && revision->is_number_unsigned()) {
            inspector_->observeSceneRevision(revision->get<std::uint64_t>());
        }
        const bool changed = pending_outliner_is_tree_
                                 ? project_model_->updateSceneTree(result)
                                 : project_model_->updateSceneOutline(result);
        outliner_ask_unconditionally_ = false;
        if (!changed) {
            // The engine answered with the revision Studio already has
            // (scene_outline: not_modified, nothing rebuilt). Ask again, but
            // less often: a scene that is not moving does not need to be
            // asked about four times a second.
            setOutlinerRefreshInterval(
                std::min(outliner_refresh_interval_ms_ * 2,
                         OutlinerRefreshMaximumIntervalMs));
            return;
        }
        setOutlinerRefreshInterval(OutlinerRefreshMinimumIntervalMs);
        // OL-1: the selected object is kept by its authoring id, wherever
        // the new tree declares it; cleared only when it is gone.
        (void)selection_model_.followProject();
        populateOutliner();
        refreshSelectionViews();
        selectCreatedObject();
    } catch (const std::exception &error) {
        outliner_ask_unconditionally_ = true;
        const auto message =
            tr("Invalid %1 response: %2")
                .arg(pending_outliner_is_tree_ ? QStringLiteral("scene_tree")
                                               : QStringLiteral("scene_outline"),
                     QString::fromUtf8(error.what()));
        statusBar()->showMessage(message, 8000);
        appendEngineOutput(tr("[Studio outliner] %1\n").arg(message));
    }
}

void MainWindow::failOutlinerRpc(qint64 request_id,
                                 const QString &message) {
    if (request_id != pending_outliner_request_) return;
    pending_outliner_request_ = 0;
    if (!pending_outliner_is_tree_ && pending_outliner_unknown_method_) {
        // UX-13c, decision 5: an engine from before scene_outline. Read it
        // with scene_tree from now on, and at once.
        outliner_uses_scene_tree_ = true;
        requestOutlinerRefresh();
        return;
    }
    outliner_ask_unconditionally_ = true;
    statusBar()->showMessage(
        tr("Outliner %1 failed: %2")
            .arg(pending_outliner_is_tree_ ? QStringLiteral("scene_tree")
                                           : QStringLiteral("scene_outline"),
                 message),
        5000);
}

void MainWindow::selectOutlinerItem(QTreeWidgetItem *item) {
    // PM-3c: a runtime row is no declared object: the declared selection is
    // cleared (the gizmo goes), and the Inspector shows the spawned one.
    const auto runtime = outlinerRuntimeEntity(item);
    if (runtime && !runtime_outliner_->model().select(*runtime)) return;
    if (!runtime) runtime_outliner_->model().clearSelection();
    const SelectionUpdate update =
        selection_model_.selectFromOutliner(outlinerObjectKey(item));
    if (update.kind == SelectionUpdateKind::failed) {
        statusBar()->showMessage(QString::fromStdString(update.message), 5000);
        return;
    }
    viewport_->setPickingNotice({});
    refreshSelectionViews();
}

void MainWindow::runtimeObjectsChanged() {
    // The rows were put back (with the selected one current): show the
    // selection again -- a new reading of the spawned object, or, when it is
    // gone (Stop), nothing.
    if (selection_model_.selected()) return;
    if (runtime_outliner_->model().selectedObject() != nullptr || inspector_->showsRuntimeObject())
        refreshSelectionViews();
}

void MainWindow::beginViewportPick(const QPoint &pixel_position) {
    const ViewportPickToken token = selection_model_.beginViewportPick();
    QString error;
    const qint64 request_id = viewport_->pickObject(pixel_position, &error);
    if (request_id == 0) {
        const SelectionUpdate update = selection_model_.failViewportPick(
            token, error.toStdString());
        if (update.kind == SelectionUpdateKind::failed) {
            presentPickingFailure(QString::fromStdString(update.message));
        }
        return;
    }
    pending_pick_tokens_.insert(request_id, token);
}

void MainWindow::completeViewportPick(qint64 request_id,
                                      const QByteArray &result_json) {
    const auto pending = pending_pick_tokens_.find(request_id);
    if (pending == pending_pick_tokens_.end()) {
        return;
    }
    const ViewportPickToken token = pending.value();
    pending_pick_tokens_.erase(pending);

    const SelectionUpdate update = selection_model_.completeViewportPick(
        token,
        std::string_view{result_json.constData(),
                         static_cast<std::size_t>(result_json.size())});
    if (update.kind == SelectionUpdateKind::stale) {
        return;
    }
    if (update.kind == SelectionUpdateKind::failed) {
        presentPickingFailure(QString::fromStdString(update.message));
        return;
    }
    // PM-3c: a pick answers for the whole selection. It does not land on a
    // spawned object's row: pick_object names a model instance, not the
    // entity get_runtime_objects lists.
    runtime_outliner_->model().clearSelection();
    viewport_->setPickingNotice({});
    refreshSelectionViews();
}

void MainWindow::failViewportPick(qint64 request_id,
                                  const QString &message) {
    const auto pending = pending_pick_tokens_.find(request_id);
    if (pending == pending_pick_tokens_.end()) {
        return;
    }
    const ViewportPickToken token = pending.value();
    pending_pick_tokens_.erase(pending);

    const SelectionUpdate update =
        selection_model_.failViewportPick(token, message.toStdString());
    if (update.kind == SelectionUpdateKind::failed) {
        presentPickingFailure(QString::fromStdString(update.message));
    }
}

void MainWindow::beginViewportPointer(const QPoint &pixel_position) {
    gizmo_model_.pointerPressed(
        {.x = pixel_position.x(), .y = pixel_position.y()},
        inspector_->gizmoTransformBinding());
    dispatchGizmoModel();
}

void MainWindow::moveViewportPointer(const QPoint &pixel_position) {
    gizmo_model_.pointerMoved(
        {.x = pixel_position.x(), .y = pixel_position.y()});
    dispatchGizmoModel();
}

void MainWindow::releaseViewportPointer(const QPoint &pixel_position) {
    gizmo_model_.pointerReleased(
        {.x = pixel_position.x(), .y = pixel_position.y()});
    dispatchGizmoModel();
}

void MainWindow::completeGizmoRpc(qint64 request_id,
                                  const QByteArray &result_json) {
    const auto pending = pending_gizmo_requests_.find(request_id);
    if (pending == pending_gizmo_requests_.end()) return;
    const std::uint64_t model_request_id = pending.value();
    pending_gizmo_requests_.erase(pending);
    gizmo_model_.receiveRpcResult(
        model_request_id,
        std::string_view{result_json.constData(),
                         static_cast<std::size_t>(result_json.size())});
    dispatchGizmoModel();
}

void MainWindow::failGizmoRpc(qint64 request_id, const QString &message) {
    const auto pending = pending_gizmo_requests_.find(request_id);
    if (pending == pending_gizmo_requests_.end()) return;
    const std::uint64_t model_request_id = pending.value();
    pending_gizmo_requests_.erase(pending);
    gizmo_model_.receiveRpcFailure(model_request_id,
                                   message.toStdString());
    dispatchGizmoModel();
}

void MainWindow::endGizmoSessionForProjectSwitch(
    const std::filesystem::path &next_project_root) {
    // Reopening the project that is open keeps its engine
    // (EmbeddedViewport::openProject), so the session goes on too.
    if (!viewport_->rpcReady()) return;
    if (project_model_ &&
        QDir(displayPath(project_model_->projectRoot())).absolutePath() ==
            QDir(displayPath(next_project_root)).absolutePath())
        return;
    // The engine on its way out fails every request it has not answered
    // ("exited before returning an RPC response"). Those are the switch, not
    // a fault: forget them, and ask nothing more of that engine.
    // engineRpcBecameAvailable starts the session with the new one.
    pending_gizmo_requests_.clear();
    gizmo_model_.stopSession();
    updateModalTransformPolling();
}

void MainWindow::dispatchGizmoModel() {
    // Whatever this dispatch does to the gizmo, the poll has to follow it:
    // a gesture that just began needs asking about, and one that just ended
    // does not.
    const auto follow_with_poll_decision = qScopeGuard(
        [this] { updateModalTransformPolling(); });
    std::unordered_set<std::uint64_t> rejected_gestures;
    for (;;) {
        bool progressed = false;

        auto edit_actions = gizmo_model_.takeEditActions();
        progressed = progressed || !edit_actions.empty();
        for (auto &action : edit_actions) {
            switch (action.kind) {
            case GizmoEditActionKind::Begin: {
                const bool started =
                    inspector_->beginGizmoEdit(action.field_key);
                gizmo_model_.confirmEditStarted(
                    action.gesture_id, started,
                    started ? std::string{}
                            : "The Inspector is not ready for a gizmo edit.");
                break;
            }
            case GizmoEditActionKind::Preview:
                if (!inspector_->previewGizmoEdit(
                        action.field_key, std::move(action.value))) {
                    rejected_gestures.insert(action.gesture_id);
                    gizmo_model_.cancelActiveEdit(
                        "The Inspector rejected the gizmo preview value.");
                }
                break;
            case GizmoEditActionKind::Finish:
                inspector_->finishGizmoEdit(action.field_key,
                                             action.commit &&
                                                 !rejected_gestures.contains(
                                                     action.gesture_id));
                break;
            }
        }

        auto fallback_picks = gizmo_model_.takeFallbackPicks();
        progressed = progressed || !fallback_picks.empty();
        for (const GizmoPixelPosition position : fallback_picks) {
            beginViewportPick(QPoint{position.x, position.y});
        }

        auto requests = gizmo_model_.takeRpcRequests();
        progressed = progressed || !requests.empty();
        for (auto &request : requests) {
            QJsonParseError parse_error;
            const QJsonDocument params_document = QJsonDocument::fromJson(
                QByteArray::fromStdString(request.params.dump()),
                &parse_error);
            if (parse_error.error != QJsonParseError::NoError ||
                !params_document.isObject()) {
                gizmo_model_.receiveRpcFailure(
                    request.request_id,
                    "Studio generated invalid gizmo RPC parameters.");
                continue;
            }
            QString error;
            const qint64 transport_request_id = viewport_->requestRpc(
                QString::fromStdString(request.method),
                params_document.object(), &error);
            if (transport_request_id == 0) {
                gizmo_model_.receiveRpcFailure(request.request_id,
                                               error.toStdString());
                continue;
            }
            pending_gizmo_requests_.insert(
                transport_request_id,
                static_cast<quint64>(request.request_id));
        }

        if (!progressed) break;
    }
    presentGizmoNotice();
    updateGizmoToolbar();
}

void MainWindow::setGizmoMode(GizmoMode mode) {
    gizmo_model_.setMode(mode);
    dispatchGizmoModel();
    statusBar()->showMessage(
        tr("Gizmo mode: %1")
            .arg(QString::fromUtf8(gizmoModeName(mode).data(),
                                   static_cast<qsizetype>(
                                       gizmoModeName(mode).size()))),
        2500);
}

void MainWindow::updateGizmoToolbar() {
    const auto revision = gizmo_model_.bindingRevision();
    const bool mode_changed = std::none_of(
        gizmo_mode_actions_.begin(), gizmo_mode_actions_.end(),
        [this](const QAction *action) {
            return action != nullptr && action->isChecked() &&
                   action == gizmo_mode_actions_[
                                 static_cast<std::size_t>(gizmo_model_.mode())];
        });
    if (presented_gizmo_binding_revision_ == revision && !mode_changed) {
        return;
    }
    presented_gizmo_binding_revision_ = revision;
    constexpr std::array modes{
        GizmoMode::Translate, GizmoMode::Rotate, GizmoMode::Scale};
    const std::array labels{tr("Move"), tr("Rotate"), tr("Scale")};
    for (std::size_t index = 0; index < modes.size(); ++index) {
        QAction *action = gizmo_mode_actions_[index];
        if (action == nullptr) continue;
        const auto display = gizmo_model_.bindingDisplay(modes[index]);
        action->setText(
            display.empty()
                ? labels[index]
                : tr("%1 (%2)")
                      .arg(labels[index],
                           QString::fromUtf8(
                               display.data(),
                               static_cast<qsizetype>(display.size()))));
        const QSignalBlocker blocker{action};
        action->setChecked(gizmo_model_.mode() == modes[index]);
    }
}

void MainWindow::presentGizmoNotice() {
    if (presented_gizmo_notice_revision_ ==
        gizmo_model_.noticeRevision()) {
        return;
    }
    presented_gizmo_notice_revision_ = gizmo_model_.noticeRevision();
    QString message = QString::fromStdString(gizmo_model_.notice().message);
    if (message.contains(QStringLiteral("gizmo"), Qt::CaseInsensitive) &&
        message.contains(QStringLiteral("requires"), Qt::CaseInsensitive)) {
        message = tr("Gizmo is unavailable: the required gizmo feature is not "
                     "active in this player session.");
    }
    viewport_->setGizmoNotice(message);
    if (!message.isEmpty()) {
        appendEngineOutput(tr("[Studio gizmo] %1\n").arg(message));
        statusBar()->showMessage(message, 8000);
    }
}

void MainWindow::refreshSelectionViews() {
    const QSignalBlocker block_outliner{outliner_};
    const auto &selection = selection_model_.selected();
    gizmo_model_.setSelection(selection);
    dispatchGizmoModel();
    updateOutlinerActions();
    if (!selection) {
        // PM-3c: a spawned object, read-only.
        if (const RuntimeObject *runtime = runtime_outliner_ != nullptr
                                               ? runtime_outliner_->model().selectedObject()
                                               : nullptr) {
            // Each reading comes here; the tree is scrolled only when the
            // row is not already the current one.
            QTreeWidgetItem *row = runtime_outliner_->rowOf(runtime->entity);
            if (row != nullptr && outliner_->currentItem() != row) {
                outliner_->setCurrentItem(row);
                outliner_->scrollToItem(row);
            }
            inspector_->showRuntimeObject(*runtime);
            return;
        }
        outliner_->setCurrentItem(nullptr);
        outliner_->clearSelection();
        inspector_->setSelection(std::nullopt, tr("None"));
        return;
    }
    if (runtime_outliner_ != nullptr) runtime_outliner_->model().clearSelection();

    QTreeWidgetItem *selected_item = nullptr;
    for (QTreeWidgetItemIterator iterator{outliner_}; *iterator != nullptr;
         ++iterator) {
        const auto key = outlinerObjectKey(*iterator);
        if (key && sameObject(*key, *selection)) {
            selected_item = *iterator;
            break;
        }
    }
    if (selected_item != nullptr) {
        outliner_->setCurrentItem(selected_item);
        outliner_->scrollToItem(selected_item);
    }

    const OutlinerObject *object =
        project_model_ ? project_model_->findObject(*selection) : nullptr;
    const QString display_name =
        object != nullptr ? QString::fromStdString(object->display_name)
                          : tr("Unknown object");
    inspector_->setSelection(
        selection,
        tr("%1\n%2 / declaration %3")
            .arg(display_name)
            .arg(QString::fromStdString(selection->scene_id))
            .arg(static_cast<qulonglong>(selection->declaration_index)));
}

void MainWindow::presentPickingFailure(const QString &message) {
    QString notice = message;
    if (message.contains(QStringLiteral("picking"), Qt::CaseInsensitive) &&
        message.contains(QStringLiteral("requires"), Qt::CaseInsensitive)) {
        notice = tr("Picking is unavailable: the required picking feature is "
                    "not active in this player session. The current selection "
                    "was preserved.");
    } else {
        notice = tr("Picking failed: %1 The current selection was preserved.")
                     .arg(message);
    }
    viewport_->setPickingNotice(notice);
    statusBar()->showMessage(notice, 8000);
    appendEngineOutput(tr("[Studio selection] %1\n").arg(notice));
}

void MainWindow::appendEngineOutput(const QString &output) {
    if (output.isEmpty()) {
        return;
    }

    QScrollBar *scroll_bar = engine_log_->verticalScrollBar();
    const bool following_tail = scroll_bar->value() >= scroll_bar->maximum();
    const int previous_scroll_position = scroll_bar->value();

    if (engine_log_buffer_.append(output)) {
        engine_log_->setPlainText(engine_log_buffer_.text());
    } else {
        QTextCursor tail(engine_log_->document());
        tail.movePosition(QTextCursor::End);
        tail.insertText(output);
    }

    if (following_tail) {
        QTextCursor tail(engine_log_->document());
        tail.movePosition(QTextCursor::End);
        engine_log_->setTextCursor(tail);
        engine_log_->ensureCursorVisible();
    } else {
        scroll_bar->setValue(std::min(previous_scroll_position, scroll_bar->maximum()));
    }
}

void MainWindow::refreshLayoutMenus() {
    restore_layout_menu_->clear();
    delete_layout_menu_->clear();

    const QStringList names = userPresetNames(
        layout_presets_.presetNames(), lastPanelLayoutName());
    if (names.isEmpty()) {
        QAction *empty_restore = restore_layout_menu_->addAction(tr("No saved layouts"));
        empty_restore->setEnabled(false);
        QAction *empty_delete = delete_layout_menu_->addAction(tr("No saved layouts"));
        empty_delete->setEnabled(false);
    } else {
        for (const QString &name : names) {
            QAction *restore_action = restore_layout_menu_->addAction(name);
            connect(restore_action, &QAction::triggered, this,
                    [this, name]() { restoreLayoutPreset(name); });

            QAction *delete_action = delete_layout_menu_->addAction(name);
            connect(delete_action, &QAction::triggered, this,
                    [this, name]() { deleteLayoutPreset(name); });
        }
    }

    restore_tool_layout_menu_->clear();
    delete_tool_layout_menu_->clear();
    const QStringList tool_names =
        tool_layout_presets_
            ? userPresetNames(tool_layout_presets_->presetNames(),
                              lastToolLayoutName())
            : QStringList{};
    if (tool_names.isEmpty()) {
        QAction *empty_restore = restore_tool_layout_menu_->addAction(
            tr("No saved tool layouts"));
        empty_restore->setEnabled(false);
        QAction *empty_delete = delete_tool_layout_menu_->addAction(
            tr("No saved tool layouts"));
        empty_delete->setEnabled(false);
    } else {
        for (const QString &name : tool_names) {
            QAction *restore_action =
                restore_tool_layout_menu_->addAction(name);
            connect(restore_action, &QAction::triggered, this,
                    [this, name]() { restoreToolLayoutPreset(name); });

            QAction *delete_action =
                delete_tool_layout_menu_->addAction(name);
            connect(delete_action, &QAction::triggered, this,
                    [this, name]() { deleteToolLayoutPreset(name); });
        }
    }
}

void MainWindow::saveLayoutPreset() {
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Layout"), tr("Layout name:"), QLineEdit::Normal, {}, &accepted);
    if (!accepted) {
        return;
    }

    const QString normalized_name = name.trimmed();
    if (normalized_name == lastPanelLayoutName()) {
        QMessageBox::warning(
            this, tr("Reserved Layout Name"),
            tr("'%1' is reserved for the last panel session.")
                .arg(normalized_name));
        return;
    }
    if (layout_presets_.presetNames().contains(normalized_name)) {
        const auto answer = QMessageBox::question(
            this, tr("Replace Layout"), tr("A layout named '%1' already exists. Replace it?").arg(normalized_name));
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    const LayoutSnapshot snapshot{
        saveGeometry(),
        saveState(LayoutPresetManager::WindowStateVersion),
    };
    QString error;
    if (!layout_presets_.savePreset(normalized_name, snapshot, &error)) {
        QMessageBox::warning(this, tr("Could Not Save Layout"), error);
        return;
    }

    statusBar()->showMessage(tr("Layout '%1' saved").arg(normalized_name), 3000);
}

void MainWindow::restoreLayoutPreset(const QString &name) {
    QString error;
    const LayoutRestoreResult result = applyLayoutPreset(name, &error);

    if (result == LayoutRestoreResult::Restored) {
        statusBar()->showMessage(tr("Layout '%1' restored").arg(name), 3000);
        return;
    }

    const QString message =
        tr("Layout '%1' could not be restored: %2 The default layout was "
           "restored.")
            .arg(name, error);
    qWarning().noquote() << message;
    statusBar()->showMessage(tr("Layout '%1' not restored; default restored")
                                 .arg(name),
                             6000);
    statusBar()->setToolTip(message);
}

void MainWindow::deleteLayoutPreset(const QString &name) {
    const auto answer = QMessageBox::question(
        this, tr("Delete Layout"), tr("Delete the layout '%1'?").arg(name));
    if (answer != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!layout_presets_.deletePreset(name, &error)) {
        QMessageBox::warning(this, tr("Could Not Delete Layout"), error);
        return;
    }
    statusBar()->showMessage(tr("Layout '%1' deleted").arg(name), 3000);
}

LayoutRestoreResult MainWindow::applyLayoutPreset(const QString &name,
                                                   QString *error) {
    return layout_presets_.restorePreset(
        name,
        [this](const LayoutSnapshot &snapshot) {
            // Establish the current default first. Qt leaves docks that are
            // absent from an older state untouched, so newly added docks keep
            // their default location while known docks regain saved state.
            applyDefaultLayout();
            const bool geometry_restored =
                restoreGeometry(snapshot.geometry);
            const bool state_restored = restoreState(
                snapshot.window_state,
                LayoutPresetManager::WindowStateVersion);
            return geometry_restored && state_restored;
        },
        [this]() { applyDefaultLayout(); }, error);
}

void MainWindow::saveLastPanelLayout() {
    const LayoutSnapshot snapshot{
        saveGeometry(),
        saveState(LayoutPresetManager::WindowStateVersion),
    };
    QString error;
    if (!layout_presets_.savePreset(lastPanelLayoutName(), snapshot,
                                    &error)) {
        qWarning().noquote()
            << tr("Could not save panel layout '%1': %2")
                   .arg(lastPanelLayoutName(), error);
    }
}

void MainWindow::restoreLastPanelLayout() {
    QString error;
    const LayoutRestoreResult result =
        applyLayoutPreset(lastPanelLayoutName(), &error);
    if (result == LayoutRestoreResult::Restored) {
        statusBar()->showMessage(
            tr("Panel layout '%1' restored")
                .arg(lastPanelLayoutName()));
        return;
    }
    if (result == LayoutRestoreResult::DefaultMissing) {
        statusBar()->showMessage(
            tr("No saved panel layout '%1'; default restored")
                .arg(lastPanelLayoutName()));
        return;
    }

    const QString message =
        tr("Panel layout '%1' could not be restored: %2 The default panel layout was restored.")
            .arg(lastPanelLayoutName(), error);
    qWarning().noquote() << message;
    statusBar()->showMessage(
        tr("Panel layout '%1' not restored; default restored")
            .arg(lastPanelLayoutName()));
    statusBar()->setToolTip(message);
}

void MainWindow::saveToolLayoutPreset() {
    if (!tool_layout_presets_) {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, tr("Save Frame Plan Layout"), tr("Tool layout name:"),
        QLineEdit::Normal, {}, &accepted);
    if (!accepted) {
        return;
    }

    const QString normalized_name = name.trimmed();
    if (normalized_name == lastToolLayoutName()) {
        QMessageBox::warning(
            this, tr("Reserved Tool Layout Name"),
            tr("'%1' is reserved for the last tool session.")
                .arg(normalized_name));
        return;
    }
    if (tool_layout_presets_->presetNames().contains(normalized_name)) {
        const auto answer = QMessageBox::question(
            this, tr("Replace Tool Layout"),
            tr("A tool layout named '%1' already exists. Replace it?")
                .arg(normalized_name));
        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    QString error;
    if (!tool_layout_presets_->savePreset(
            normalized_name, frame_plan_->toolLayoutSnapshot(), &error)) {
        QMessageBox::warning(this, tr("Could Not Save Tool Layout"),
                             error);
        return;
    }
    statusBar()->showMessage(
        tr("Frame Plan layout '%1' saved").arg(normalized_name), 3000);
}

void MainWindow::restoreToolLayoutPreset(const QString &name) {
    if (!tool_layout_presets_) {
        return;
    }

    QString error;
    const ToolLayoutRestoreResult result =
        tool_layout_presets_->restorePreset(
            name,
            [this](const ToolLayoutSnapshot &snapshot) {
                return frame_plan_->restoreToolLayout(snapshot);
            },
            [this]() { frame_plan_->applyDefaultToolLayout(); }, &error);
    if (result == ToolLayoutRestoreResult::Restored) {
        statusBar()->showMessage(
            tr("Frame Plan layout '%1' restored").arg(name), 3000);
        return;
    }

    const QString message =
        tr("Frame Plan layout '%1' could not be restored: %2 The default tool "
           "layout was restored.")
            .arg(name, error);
    qWarning().noquote() << message;
    statusBar()->showMessage(
        tr("Frame Plan layout '%1' not restored; default restored").arg(name),
        6000);
    statusBar()->setToolTip(message);
}

void MainWindow::deleteToolLayoutPreset(const QString &name) {
    if (!tool_layout_presets_) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, tr("Delete Tool Layout"),
        tr("Delete the Frame Plan layout '%1'?").arg(name));
    if (answer != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!tool_layout_presets_->deletePreset(name, &error)) {
        QMessageBox::warning(this, tr("Could Not Delete Tool Layout"),
                             error);
        return;
    }
    statusBar()->showMessage(
        tr("Frame Plan layout '%1' deleted").arg(name), 3000);
}

void MainWindow::saveLastToolLayout() {
    if (!tool_layout_presets_) {
        return;
    }
    QString error;
    if (!tool_layout_presets_->savePreset(
            lastToolLayoutName(), frame_plan_->toolLayoutSnapshot(),
            &error)) {
        qWarning().noquote()
            << tr("Could not save Frame Plan layout '%1': %2")
                   .arg(lastToolLayoutName(), error);
    }
}

QString MainWindow::restoreLastToolLayout() {
    if (!tool_layout_presets_) {
        frame_plan_->applyDefaultToolLayout();
        return tr("No project-specific Frame Plan layout is active.");
    }

    QString error;
    const ToolLayoutRestoreResult result =
        tool_layout_presets_->restorePreset(
            lastToolLayoutName(),
            [this](const ToolLayoutSnapshot &snapshot) {
                return frame_plan_->restoreToolLayout(snapshot);
            },
            [this]() { frame_plan_->applyDefaultToolLayout(); }, &error);
    if (result == ToolLayoutRestoreResult::Restored) {
        return tr("Frame Plan layout '%1' restored.")
            .arg(lastToolLayoutName());
    }
    if (result == ToolLayoutRestoreResult::DefaultMissing) {
        return tr("No saved Frame Plan layout '%1'; default tool layout")
            .arg(lastToolLayoutName());
    }

    const QString message =
        tr("Frame Plan layout '%1' could not be restored: %2 The default tool layout was restored.")
            .arg(lastToolLayoutName(), error);
    qWarning().noquote() << message;
    statusBar()->setToolTip(message);
    return tr("Frame Plan layout '%1' not restored; default restored")
        .arg(lastToolLayoutName());
}

void MainWindow::applyDefaultLayout() {
    for (QDockWidget *dock : docks_) {
        removeDockWidget(dock);
        dock->setFloating(false);
        dock->show();
    }

    addDockWidget(Qt::LeftDockWidgetArea, docks_[ProjectDock]);
    addDockWidget(Qt::LeftDockWidgetArea, docks_[OutlinerDock]);
    tabifyDockWidget(docks_[ProjectDock], docks_[OutlinerDock]);
    // HW-2s: Assets is the tab next to Project.
    addDockWidget(Qt::LeftDockWidgetArea, docks_[AssetsPanelDock]);
    tabifyDockWidget(docks_[ProjectDock], docks_[AssetsPanelDock]);
    // MT-1s: Materials is the tab after Assets.
    addDockWidget(Qt::LeftDockWidgetArea, docks_[MaterialsPanelDock]);
    tabifyDockWidget(docks_[AssetsPanelDock], docks_[MaterialsPanelDock]);
    docks_[ProjectDock]->raise();

    addDockWidget(Qt::RightDockWidgetArea, docks_[InspectorDock]);
    addDockWidget(Qt::BottomDockWidgetArea, docks_[OutputDock]);
    addDockWidget(Qt::BottomDockWidgetArea, docks_[EngineLogDock]);
    addDockWidget(Qt::BottomDockWidgetArea, docks_[FramePlanDock]);
    addDockWidget(Qt::BottomDockWidgetArea, docks_[FullscreenPassDock]);
    // PB-1s: Problems is the tab after the Engine Log.
    addDockWidget(Qt::BottomDockWidgetArea, docks_[ProblemsPanelDock]);
    tabifyDockWidget(docks_[OutputDock], docks_[EngineLogDock]);
    tabifyDockWidget(docks_[EngineLogDock], docks_[ProblemsPanelDock]);
    tabifyDockWidget(docks_[ProblemsPanelDock], docks_[FramePlanDock]);
    tabifyDockWidget(docks_[FramePlanDock], docks_[FullscreenPassDock]);
    docks_[FramePlanDock]->raise();
    resizeDocks({docks_[ProjectDock], docks_[InspectorDock]}, {280, 320}, Qt::Horizontal);
    resizeDocks({docks_[FullscreenPassDock]}, {320}, Qt::Vertical);
    resize(1280, 800);
}

} // namespace PelicanStudio
