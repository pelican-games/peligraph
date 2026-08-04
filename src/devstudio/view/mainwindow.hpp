#pragma once

#include "layoutpreset.hpp"
#include "../model/project.hpp"
#include "../viewport/enginelogbuffer.hpp"

#include <QMainWindow>

#include <array>
#include <optional>

class QDockWidget;
class QListWidget;
class QMenu;
class QPlainTextEdit;
class QString;
class QTreeWidget;

namespace PelicanStudio {

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
        DockCount,
    };

    LayoutPresetManager layout_presets_;
    std::array<QDockWidget *, DockCount> docks_{};
    QListWidget *project_list_ = nullptr;
    QTreeWidget *outliner_ = nullptr;
    QMenu *restore_layout_menu_ = nullptr;
    QMenu *delete_layout_menu_ = nullptr;
    QPlainTextEdit *engine_log_ = nullptr;
    EngineLogBuffer engine_log_buffer_;
    std::optional<ProjectOutlinerModel> project_model_;

    void createWorkspace();
    void createMenus();
    void chooseProject();
    void openProject(const QString &path);
    void populateOutliner();
    void appendEngineOutput(const QString &output);
    void refreshLayoutMenus();
    void saveLayoutPreset();
    void restoreLayoutPreset(const QString &name);
    void deleteLayoutPreset(const QString &name);
    void applyDefaultLayout();
};

} // namespace PelicanStudio
