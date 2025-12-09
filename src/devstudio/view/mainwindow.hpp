#pragma once

#include <QMainWindow>
#include <QQmlEngine>

namespace PelicanStudio {

class MainWindow : public QMainWindow {
    QQmlEngine *engine;

  public:
    MainWindow();
};

} // namespace PelicanStudio