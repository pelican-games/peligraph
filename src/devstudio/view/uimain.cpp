#include "uimain.hpp"
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <iostream>

namespace PelicanStudio {

int uimain(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine engine;

    std::cout << "starting Pelican Studio..." << std::endl;

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/pelican_studio/view/MainWindow.qml")));
    return app.exec();
}

} // namespace PelicanStudio
