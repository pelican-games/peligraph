#include "engineprocess.hpp"

#include <QCoreApplication>

#include <iostream>

int main(int argc, char *argv[]) {
    QCoreApplication application{argc, argv};
    if (argc != 2) {
        std::cerr << "expected the engine child executable path\n";
        return 2;
    }

    PelicanStudio::EngineProcess process;
    QString error;
    if (!process.start({QString::fromLocal8Bit(argv[1]), {QStringLiteral("hang")}, {}}, &error)) {
        std::cerr << error.toLocal8Bit().constData() << '\n';
        return 3;
    }
    if (!process.waitForStarted(5000)) {
        std::cerr << "engine child did not start\n";
        return 4;
    }

    std::cout << process.processId() << '\n' << std::flush;
    return application.exec();
}
