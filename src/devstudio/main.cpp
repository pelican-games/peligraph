#include "mainwindow.hpp"
#include <QtWidgets/QApplication>
#include <argparse/argparse.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

int main(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Studio");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return -1;
    }

    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}