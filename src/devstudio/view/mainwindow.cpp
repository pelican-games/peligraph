#include "mainwindow.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
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

    auto *project_list = new QListWidget(this);
    project_list->addItem(tr("No project open"));
    project_list->setEnabled(false);
    docks_[ProjectDock] = makeDock(this, tr("Project"), QStringLiteral("pelican.projectDock"), project_list);

    auto *outliner = new QTreeWidget(this);
    outliner->setHeaderLabel(tr("Scene"));
    docks_[OutlinerDock] = makeDock(this, tr("Outliner"), QStringLiteral("pelican.outlinerDock"), outliner);

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
