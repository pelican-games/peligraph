#include "materialcommand.hpp"

#include "../project/materiallowering.hpp"
#include "../project/surfaceformat.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace Pelican::DevCli {

int runDumpLoweredMaterialCommand(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "usage: pelican_cli dump-lowered-material <surface>" << std::endl;
        return 2;
    }

    const std::filesystem::path path{argv[1]};
    try {
        std::ifstream file{path, std::ios_base::binary};
        if (!file.is_open()) {
            throw std::runtime_error("surface '" + path.string() + "' could not be opened");
        }
        const std::string source{std::istreambuf_iterator<char>{file},
                                 std::istreambuf_iterator<char>{}};
        const auto surface = parseSurfaceFormat(source, path.string());
        const auto lowered = lowerSurfaceDefaults(surface, path.generic_string());
        std::cout << dumpLoweredMaterial(lowered);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "dump-lowered-material: " << error.what() << std::endl;
        return 1;
    }
}

} // namespace Pelican::DevCli
