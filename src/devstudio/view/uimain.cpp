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

    // necessary for exiting if error
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, []() { 
            std::cerr << "failed to launch Qt application" << std::endl;
            QCoreApplication::exit(-1); 
        },
        Qt::QueuedConnection);

    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/pelican_studio/MainWindow.qml")));
    return app.exec();
}

} // namespace PelicanStudio
