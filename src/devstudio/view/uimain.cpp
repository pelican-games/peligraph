#include "uimain.hpp"
#include "../guitrace.hpp"
#include "mainwindow.hpp"
#include "studioscreen.hpp"
#include "../automation/automationserver.hpp"
#include <QApplication>
#include <QString>
#include <QStringList>
#include <filesystem>
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
    class TracedApplication final : public QApplication {
    public:
        using QApplication::QApplication;
        bool notify(QObject *receiver, QEvent *event) override {
            if (!GuiTrace::sink) return QApplication::notify(receiver, event);
            const GuiTraceSpan trace{"qt_notify", double(event->type())};
            const GuiTraceSpan receiver_trace{receiver->metaObject()->className(), double(event->type())};
            return QApplication::notify(receiver, event);
        }
    };
    TracedApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Pelican"));
    QCoreApplication::setApplicationName(QStringLiteral("Pelican Studio"));

    std::cout << "starting Pelican Studio..." << std::endl;

    MainWindow window;
    // OT-1: after the saved geometry was restored (the constructor), so the
    // screen asked for is the one the window opens on.
    placeOnRequestedScreen(window);
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
        host.before_quit = [&window](std::function<void()> quit) {
            window.stopPlayThen(std::move(quit));
        };
        automation = std::make_unique<AutomationServer>(std::move(host));
        // A scenario drives Studio, not the desktop: a shader "opened" under
        // automation is written to the log instead of starting an editor.
        window.setShaderSourceOpenAction(
            [](const std::filesystem::path &path) {
                std::cout << "automation: shader source open "
                          << path.generic_string() << std::endl;
                return true;
            });
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
