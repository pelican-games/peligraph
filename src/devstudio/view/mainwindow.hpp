#pragma once

#include "layoutpreset.hpp"
#include "../model/project.hpp"
#include "../model/selection.hpp"
#include "../viewport/enginelogbuffer.hpp"

#include <QByteArray>
#include <QHash>
#include <QMainWindow>
#include <QPoint>

#include <array>
#include <optional>

class QDockWidget;
class QLabel;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QString;
class QTreeWidget;
class QTreeWidgetItem;

namespace PelicanStudio {

class EmbeddedViewport;
class FramePlanWidget;
class InspectorWidget;

class MainWindow : public QMainWindow {
  public:
    MainWindow();

  private:
    enum DockIndex {
        ProjectDock,
        OutlinerDock,
        InspectorDock,
        OutputDock,
        EngineLogDock,
        FramePlanDock,
        DockCount,
    };

    LayoutPresetManager layout_presets_;
    std::array<QDockWidget *, DockCount> docks_{};
    QListWidget *project_list_ = nullptr;
    QTreeWidget *outliner_ = nullptr;
    EmbeddedViewport *viewport_ = nullptr;
    InspectorWidget *inspector_ = nullptr;
    FramePlanWidget *frame_plan_ = nullptr;
    QMenu *restore_layout_menu_ = nullptr;
    QMenu *delete_layout_menu_ = nullptr;
    QPlainTextEdit *engine_log_ = nullptr;
    EngineLogBuffer engine_log_buffer_;
    std::optional<ProjectOutlinerModel> project_model_;
    SelectionModel selection_model_;
    QHash<qint64, ViewportPickToken> pending_pick_tokens_;

    void createWorkspace();
    void createMenus();
    void chooseProject();
    void openProject(const QString &path);
    void populateOutliner();
    void selectOutlinerItem(QTreeWidgetItem *item);
    void beginViewportPick(const QPoint &pixel_position);
    void completeViewportPick(qint64 request_id,
                              const QByteArray &result_json);
    void failViewportPick(qint64 request_id, const QString &message);
    void refreshSelectionViews();
    void presentPickingFailure(const QString &message);
    void appendEngineOutput(const QString &output);
    void refreshLayoutMenus();
    void saveLayoutPreset();
    void restoreLayoutPreset(const QString &name);
    void deleteLayoutPreset(const QString &name);
    void applyDefaultLayout();
};

} // namespace PelicanStudio
