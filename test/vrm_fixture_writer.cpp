#include "vrm_fixture.hpp"

#include <iostream>

int main(int argc, char *argv[]) {
    if (argc != 3) {
        std::cerr << "usage: vrm_fixture_writer <kind> <output.vrm>\n";
        return 2;
    }
    try {
        Pelican::TestVrmFixture::writeGlb(argv[2],
                                         Pelican::TestVrmFixture::kindFromString(argv[1]));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
