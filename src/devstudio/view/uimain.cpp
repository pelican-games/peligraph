#include "uimain.hpp"
#include "mainwindow.hpp"
#include "../automation/automationserver.hpp"
#include <QApplication>
#include <QString>
#include <QStringList>
#include <iostream>
#include <memory>

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

    // The automation channel is opened before the project so a driver can
    // attach, then watch the engine come up, rather than racing it. Without
    // --automation nothing below runs and no socket exists.
    std::unique_ptr<AutomationServer> automation;
    if (!options.automation.empty()) {
        AutomationHost host;
        host.window = &window;
        host.viewport = window.engineViewport();
        host.open_project = [&window](const QString &directory) {
            window.openProject(directory);
        };
        automation = std::make_unique<AutomationServer>(std::move(host));
        QString error;
        const auto name = QString::fromStdString(options.automation);
        if (!automation->listen(name, &error)) {
            std::cerr << "pelican_studio: --automation " << options.automation
                      << " could not listen: " << error.toStdString()
                      << std::endl;
            return 3;
        }
        // The driver waits for this line rather than polling a socket name it
        // has to guess the platform spelling of.
        std::cout << "automation listening: " << automation->endpoint().toStdString()
                  << std::endl;
    }

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
