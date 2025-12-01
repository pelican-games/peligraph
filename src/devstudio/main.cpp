#include "view/uimain.hpp"
#include <argparse/argparse.hpp>
#include <iostream>

int main(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Studio");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return -1;
    }

    return PelicanStudio::uimain(argc, argv);
}