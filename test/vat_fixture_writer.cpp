#include "vat_fixture.hpp"

#include <exception>
#include <iostream>

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: vat_fixture_writer <path.glb>\n";
        return 2;
    }

    try {
        Pelican::TestVatFixture::writeTinyVatGlb(argv[1]);
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << "\n";
        return 1;
    }
    return 0;
}
