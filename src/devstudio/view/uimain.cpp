#include "uimain.hpp"
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>

namespace PelicanStudio {

int uimain(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    engine.loadFromModule("pelican_studio", "MainWindow");
    return app.exec();
}

} // namespace PelicanStudio
