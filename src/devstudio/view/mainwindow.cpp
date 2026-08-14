#include "mainwindow.hpp"
#include "frameplanwidget.hpp"
#include "inspectorwidget.hpp"
#include "../viewport/embeddedviewport.hpp"

#include <QAction>
#include <QActionGroup>
#include <QAbstractItemView>
#include <QCoreApplication>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFrame>
#include <QFont>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTextCursor>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <limits>
#include <unordered_set>
#include <vector>

namespace PelicanStudio {
namespace {

constexpr int SceneIdRole = Qt::UserRole;
constexpr int DeclarationIndexRole = Qt::UserRole + 1;

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

std::optional<OutlinerObjectKey> objectKey(QTreeWidgetItem *item) {
    if (item == nullptr) {
        return std::nullopt;
    }
    const QVariant scene_id = item->data(0, SceneIdRole);
    const QVariant declaration_index =
        item->data(0, DeclarationIndexRole);
    if (!scene_id.isValid() || !declaration_index.isValid()) {
        return std::nullopt;
    }
    bool valid_index = false;
    const qulonglong index = declaration_index.toULongLong(&valid_index);
    if (!valid_index || index > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return OutlinerObjectKey{
        .scene_id = scene_id.toString().toStdString(),
        .declaration_index = static_cast<std::size_t>(index),
    };
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

    workspace_hint->hide();
    workspace_layout->setStretch(0, 0);
    workspace_layout->setStretch(3, 0);
    viewport_ = new EmbeddedViewport(workspace);
    viewport_->bindSelectionModel(&selection_model_);
    workspace_layout->addWidget(viewport_, 1);

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
    connect(outliner_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
                selectOutlinerItem(current);
            });
    docks_[OutlinerDock] =
        makeDock(this, tr("Outliner"), QStringLiteral("pelican.outlinerDock"), outliner_);

    inspector_ = new InspectorWidget(viewport_, this);
    docks_[InspectorDock] = makeDock(this, tr("Inspector"),
                                     QStringLiteral("pelican.inspectorDock"),
                                     inspector_);

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
    frame_plan_ = new FramePlanWidget(viewport_, this);
    docks_[FramePlanDock] = makeDock(
        this, tr("Frame Plan"), QStringLiteral("pelican.framePlanDock"),
        frame_plan_);
    connect(viewport_, &EmbeddedViewport::engineOutputReceived, this,
            [this](const QString &output) { appendEngineOutput(output); });
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
    connect(viewport_, &EmbeddedViewport::engineRpcBecameAvailable, this,
            [this] {
                pending_gizmo_requests_.clear();
                gizmo_model_.startSession();
                modal_transform_timer_->start();
                dispatchGizmoModel();
            });
    connect(viewport_, &EmbeddedViewport::engineRpcBecameUnavailable, this,
            [this](const QString &) {
                modal_transform_timer_->stop();
                pending_gizmo_requests_.clear();
                gizmo_model_.stopSession();
                dispatchGizmoModel();
            });
    connect(viewport_, &EmbeddedViewport::inspectorRpcSucceeded, this,
            [this](qint64 request_id, const QByteArray &result_json) {
                completeGizmoRpc(request_id, result_json);
            });
    connect(viewport_, &EmbeddedViewport::inspectorRpcFailed, this,
            [this](qint64 request_id, const QString &message) {
                failGizmoRpc(request_id, message);
            });
    modal_transform_timer_ = new QTimer(this);
    modal_transform_timer_->setInterval(16);
    connect(modal_transform_timer_, &QTimer::timeout, this, [this] {
        gizmo_model_.pollModalTransform(
            inspector_->gizmoTransformBinding());
        dispatchGizmoModel();
    });
}

void MainWindow::createMenus() {
    QMenu *file_menu = menuBar()->addMenu(tr("&File"));
    QAction *exit_action = file_menu->addAction(tr("E&xit"));
    connect(exit_action, &QAction::triggered, this, &QWidget::close);
    QAction *open_project_action = file_menu->addAction(tr("&Open Project..."));
    connect(open_project_action, &QAction::triggered, this,
            [this]() { chooseProject(); });
    QAction *save_scene_action = file_menu->addAction(tr("&Save Scene"));
    save_scene_action->setShortcut(QKeySequence::Save);
    connect(save_scene_action, &QAction::triggered, this, [this] {
        if (inspector_->saveScene()) {
            statusBar()->showMessage(tr("Saving scene..."), 3000);
        } else {
            statusBar()->showMessage(
                tr("Scene save is unavailable while an edit is active."),
                5000);
        }
    });

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

    auto *gizmo_toolbar = addToolBar(tr("Gizmo"));
    gizmo_toolbar->setObjectName(QStringLiteral("pelican.gizmoToolbar"));
    gizmo_toolbar->setMovable(true);
    auto *gizmo_group = new QActionGroup(gizmo_toolbar);
    gizmo_group->setExclusive(true);
    const auto add_mode = [&](const QString &label, GizmoMode mode,
                              const QString &tool_tip) {
        QAction *action = gizmo_toolbar->addAction(label);
        action->setCheckable(true);
        action->setToolTip(tool_tip);
        gizmo_group->addAction(action);
        connect(action, &QAction::triggered, this,
                [this, mode] { setGizmoMode(mode); });
        return action;
    };
    gizmo_mode_actions_[0] = add_mode(
        tr("Move"), GizmoMode::Translate,
        tr("Move the selection along one world axis"));
    gizmo_mode_actions_[1] = add_mode(
        tr("Rotate"), GizmoMode::Rotate,
        tr("Rotate the selection around one world axis"));
    gizmo_mode_actions_[2] = add_mode(
        tr("Scale"), GizmoMode::Scale,
        tr("Scale the selection along one axis"));
    gizmo_mode_actions_[0]->setChecked(true);
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
        selection_model_.bindProject(&*project_model_);
        populateOutliner();
        refreshSelectionViews();
        viewport_->openProject(displayPath(project_model_->projectRoot()));
        statusBar()->showMessage(
            tr("Opened %1").arg(displayPath(project_model_->projectRoot())),
            5000);
    } catch (const std::exception &error) {
        QMessageBox::warning(this, tr("Could Not Open Project"),
                             QString::fromUtf8(error.what()));
    }
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

    outliner_->clear();
    for (const auto &scene : model.scenes()) {
        auto *scene_item = new QTreeWidgetItem(
            outliner_, QStringList{QString::fromStdString(scene.scene_id)});
        scene_item->setData(0, SceneIdRole,
                            QString::fromStdString(scene.scene_id));

        std::vector<QTreeWidgetItem *> object_items;
        object_items.reserve(scene.objects.size());
        for (const auto &object : scene.objects) {
            auto *item = new QTreeWidgetItem(
                QStringList{QString::fromStdString(object.display_name)});
            item->setData(0, SceneIdRole,
                          QString::fromStdString(object.key.scene_id));
            item->setData(
                0, DeclarationIndexRole,
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

void MainWindow::selectOutlinerItem(QTreeWidgetItem *item) {
    const SelectionUpdate update =
        selection_model_.selectFromOutliner(objectKey(item));
    if (update.kind == SelectionUpdateKind::failed) {
        statusBar()->showMessage(QString::fromStdString(update.message), 5000);
        return;
    }
    viewport_->setPickingNotice({});
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

void MainWindow::dispatchGizmoModel() {
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
    if (!selection) {
        outliner_->setCurrentItem(nullptr);
        outliner_->clearSelection();
        inspector_->setSelection(std::nullopt, tr("None"));
        return;
    }

    QTreeWidgetItem *selected_item = nullptr;
    for (QTreeWidgetItemIterator iterator{outliner_}; *iterator != nullptr;
         ++iterator) {
        if (objectKey(*iterator) == selection) {
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
    addDockWidget(Qt::BottomDockWidgetArea, docks_[EngineLogDock]);
    addDockWidget(Qt::BottomDockWidgetArea, docks_[FramePlanDock]);
    tabifyDockWidget(docks_[OutputDock], docks_[EngineLogDock]);
    tabifyDockWidget(docks_[EngineLogDock], docks_[FramePlanDock]);
    docks_[FramePlanDock]->raise();
    resizeDocks({docks_[ProjectDock], docks_[InspectorDock]}, {280, 320}, Qt::Horizontal);
    resizeDocks({docks_[FramePlanDock]}, {240}, Qt::Vertical);
    resize(1280, 800);
}

} // namespace PelicanStudio
