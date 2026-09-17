#include "uimain.hpp"
#include "mainwindow.hpp"
#include <QApplication>
#include <QString>
#include <QStringList>
#include <iostream>

namespace PelicanStudio {

bool isRenderToolsTab(const std::string &name) {
    return MainWindow::renderToolsTabIds().contains(
        QString::fromStdString(name));
}

std::string renderToolsTabList() {
    return MainWindow::renderToolsTabIds()
        .join(QStringLiteral(", "))
        .toStdString();
}

int uimain(int argc, char **argv, LaunchOptions options) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Pelican"));
    QCoreApplication::setApplicationName(QStringLiteral("Pelican Studio"));

    std::cout << "starting Pelican Studio..." << std::endl;

    MainWindow window;
    window.show();
    // Opening a project restores that project's tool layout, which can change
    // which render-tools tab is in front. Ask for the tab afterwards, so the
    // one the command line named is the one the user sees.
    if (!options.project.empty())
        window.openProject(QString::fromStdString(options.project));
    if (!options.tab.empty())
        window.showRenderToolsTab(QString::fromStdString(options.tab));
    return app.exec();
}

} // namespace PelicanStudio
