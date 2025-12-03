#include "mainwindow.hpp"
#include <QObject>
#include <QQmlContext>
#include <QQuickWidget>
#include <iostream>
#include "viewmodel/testbackend.hpp"

namespace PelicanStudio {

MainWindow::MainWindow() {
    engine = new QQmlEngine(this);
    engine->rootContext()->setContextProperty("backend", new TestBackend());

    // for test
    QQuickWidget *centralQml = new QQuickWidget(engine, this);
    centralQml->setSource(QUrl(QStringLiteral("qrc:/qt/qml/pelican_studio/ProjectEdit.qml")));
    centralQml->setResizeMode(QQuickWidget::SizeRootObjectToView);
    setCentralWidget(centralQml);
}

} // namespace PelicanStudio
