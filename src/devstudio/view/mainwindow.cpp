#include "mainwindow.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QFont>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

#include <exception>
#include <filesystem>
#include <vector>

namespace PelicanStudio {
namespace {

QString layoutPresetDirectory() {
    const QString application_config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(application_config).filePath(QStringLiteral("layouts"));
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

} // namespace

MainWindow::MainWindow() : layout_presets_(layoutPresetDirectory()) {
    setWindowTitle(tr("Pelican Studio"));
    setDockOptions(
        QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
        QMainWindow::GroupedDragging);
    setDockNestingEnabled(true);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

    createWorkspace();
    createMenus();
    applyDefaultLayout();
    statusBar()->showMessage(tr("Ready"));
}

void MainWindow::createWorkspace() {
    auto *workspace = new QFrame(this);
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

    project_list_ = new QListWidget(this);
    project_list_->addItem(tr("No project open"));
    project_list_->setEnabled(false);
    project_list_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    docks_[ProjectDock] =
        makeDock(this, tr("Project"), QStringLiteral("pelican.projectDock"), project_list_);

    outliner_ = new QTreeWidget(this);
    outliner_->setHeaderLabel(tr("Scene / Object"));
    outliner_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    outliner_->setDragDropMode(QAbstractItemView::NoDragDrop);
    docks_[OutlinerDock] =
        makeDock(this, tr("Outliner"), QStringLiteral("pelican.outlinerDock"), outliner_);

    auto *inspector = new QWidget(this);
    auto *inspector_layout = new QFormLayout(inspector);
    inspector_layout->addRow(tr("Selection"), new QLabel(tr("None"), inspector));
    inspector_layout->addRow(tr("Properties"), new QLabel(tr("No editable properties"), inspector));
    docks_[InspectorDock] = makeDock(this, tr("Inspector"), QStringLiteral("pelican.inspectorDock"), inspector);

    auto *output = new QPlainTextEdit(this);
    output->setReadOnly(true);
    output->setPlainText(tr("Pelican Studio ready."));
    docks_[OutputDock] = makeDock(this, tr("Output"), QStringLiteral("pelican.outputDock"), output);
}

void MainWindow::createMenus() {
    QMenu *file_menu = menuBar()->addMenu(tr("&File"));
    QAction *exit_action = file_menu->addAction(tr("E&xit"));
    connect(exit_action, &QAction::triggered, this, &QWidget::close);
    QAction *open_project_action = file_menu->addAction(tr("&Open Project..."));
    connect(open_project_action, &QAction::triggered, this,
            [this]() { chooseProject(); });

    QMenu *view_menu = menuBar()->addMenu(tr("&View"));
    QMenu *panels_menu = view_menu->addMenu(tr("&Panels"));
    for (QDockWidget *dock : docks_) {
        panels_menu->addAction(dock->toggleViewAction());
    }

    QMenu *layout_menu = menuBar()->addMenu(tr("&Layout"));
    QAction *save_action = layout_menu->addAction(tr("&Save Layout As..."));
    connect(save_action, &QAction::triggered, this, [this]() { saveLayoutPreset(); });

    restore_layout_menu_ = layout_menu->addMenu(tr("&Restore Layout"));
    delete_layout_menu_ = layout_menu->addMenu(tr("&Delete Layout"));
    connect(layout_menu, &QMenu::aboutToShow, this, [this]() { refreshLayoutMenus(); });

    layout_menu->addSeparator();
    QAction *default_action = layout_menu->addAction(tr("Reset to &Default"));
    connect(default_action, &QAction::triggered, this, [this]() {
        applyDefaultLayout();
        statusBar()->showMessage(tr("Default layout restored"), 3000);
    });
}

void MainWindow::chooseProject() {
    const QString directory = QFileDialog::getExistingDirectory(
        this, tr("Open Pelican Project"), {}, QFileDialog::ShowDirsOnly);
    if (!directory.isEmpty()) {
        openProject(directory);
    }
}

void MainWindow::openProject(const QString &path) {
    try {
        project_model_ = ProjectOutlinerModel::open(filesystemPath(path));
        populateOutliner();
        statusBar()->showMessage(
            tr("Opened %1").arg(displayPath(project_model_->projectRoot())),
            5000);
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Could Not Open Project"),
                             QString::fromUtf8(error.what()));
    }
}

void MainWindow::populateOutliner() {
    project_list_->clear();
    const auto &model = *project_model_;
    if (model.projectName()) {
        project_list_->addItem(QString::fromStdString(*model.projectName()));
    }
    project_list_->addItem(displayPath(model.projectRoot()));
    project_list_->setEnabled(true);

    outliner_->clear();
    constexpr int scene_id_role = Qt::UserRole;
    constexpr int declaration_index_role = Qt::UserRole + 1;
    for (const auto &scene : model.scenes()) {
        auto *scene_item = new QTreeWidgetItem(
            outliner_, QStringList{QString::fromStdString(scene.scene_id)});
        scene_item->setData(0, scene_id_role,
                            QString::fromStdString(scene.scene_id));

        std::vector<QTreeWidgetItem *> object_items;
        object_items.reserve(scene.objects.size());
        for (const auto &object : scene.objects) {
            auto *item = new QTreeWidgetItem(
                QStringList{QString::fromStdString(object.display_name)});
            item->setData(0, scene_id_role,
                          QString::fromStdString(object.key.scene_id));
            item->setData(
                0, declaration_index_role,
                QVariant::fromValue<qulonglong>(object.key.declaration_index));
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
    outliner_->expandToDepth(0);
}

void MainWindow::refreshLayoutMenus() {
    restore_layout_menu_->clear();
    delete_layout_menu_->clear();

    const QStringList names = layout_presets_.presetNames();
    if (names.isEmpty()) {
        QAction *empty_restore = restore_layout_menu_->addAction(tr("No saved layouts"));
        empty_restore->setEnabled(false);
        QAction *empty_delete = delete_layout_menu_->addAction(tr("No saved layouts"));
        empty_delete->setEnabled(false);
        return;
    }

    for (const QString &name : names) {
        QAction *restore_action = restore_layout_menu_->addAction(name);
        connect(restore_action, &QAction::triggered, this, [this, name]() { restoreLayoutPreset(name); });

        QAction *delete_action = delete_layout_menu_->addAction(name);
        connect(delete_action, &QAction::triggered, this, [this, name]() { deleteLayoutPreset(name); });
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
    const LayoutRestoreResult result = layout_presets_.restorePreset(
        name,
        [this](const LayoutSnapshot &snapshot) {
            const bool geometry_restored = restoreGeometry(snapshot.geometry);
            const bool state_restored = restoreState(snapshot.window_state, LayoutPresetManager::WindowStateVersion);
            return geometry_restored && state_restored;
        },
        [this]() { applyDefaultLayout(); }, &error);

    if (result == LayoutRestoreResult::Restored) {
        statusBar()->showMessage(tr("Layout '%1' restored").arg(name), 3000);
        return;
    }

    QMessageBox::warning(
        this, tr("Layout Could Not Be Restored"),
        tr("%1\n\nThe default layout has been restored.").arg(error));
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

void MainWindow::applyDefaultLayout() {
    for (QDockWidget *dock : docks_) {
        removeDockWidget(dock);
        dock->setFloating(false);
        dock->show();
    }

    addDockWidget(Qt::LeftDockWidgetArea, docks_[ProjectDock]);
    addDockWidget(Qt::LeftDockWidgetArea, docks_[OutlinerDock]);
    tabifyDockWidget(docks_[ProjectDock], docks_[OutlinerDock]);
    docks_[ProjectDock]->raise();

    addDockWidget(Qt::RightDockWidgetArea, docks_[InspectorDock]);
    addDockWidget(Qt::BottomDockWidgetArea, docks_[OutputDock]);
    resizeDocks({docks_[ProjectDock], docks_[InspectorDock]}, {280, 320}, Qt::Horizontal);
    resizeDocks({docks_[OutputDock]}, {180}, Qt::Vertical);
    resize(1280, 800);
}

} // namespace PelicanStudio
