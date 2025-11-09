#include <exception>
#include <iostream>
#include <pelican_core.hpp>

int main() {
    Pelican::PelicanCore pl;
    pl.run();

    std::cout << "finished" << std::endl;

    return 0;
}
