#include "uimain.hpp"
#include "mainwindow.hpp"
#include <QApplication>
#include <iostream>

namespace PelicanStudio {

int uimain(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Pelican"));
    QCoreApplication::setApplicationName(QStringLiteral("Pelican Studio"));

    std::cout << "starting Pelican Studio..." << std::endl;

    MainWindow window;
    window.show();
    return app.exec();
}

} // namespace PelicanStudio
