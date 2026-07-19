#include "vrma_fixture.hpp"

#include <iostream>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << "usage: vrma_fixture_writer <kind> <output.vrma>\n";
        return 2;
    }
    try {
        Pelican::TestVrmaFixture::writeGlb(
            argv[2], Pelican::TestVrmaFixture::kindFromString(argv[1]));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
