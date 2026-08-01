#pragma once

#include "layoutpreset.hpp"

#include <QMainWindow>

#include <array>

class QDockWidget;
class QMenu;

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
        DockCount,
    };

    LayoutPresetManager layout_presets_;
    std::array<QDockWidget *, DockCount> docks_{};
    QMenu *restore_layout_menu_ = nullptr;
    QMenu *delete_layout_menu_ = nullptr;

    void createWorkspace();
    void createMenus();
    void refreshLayoutMenus();
    void saveLayoutPreset();
    void restoreLayoutPreset(const QString &name);
    void deleteLayoutPreset(const QString &name);
    void applyDefaultLayout();
};

} // namespace PelicanStudio
