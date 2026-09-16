#include "graphcatalogcommand.hpp"

#include "../project/graphmodule.hpp"

#include <iostream>
#include <string_view>

namespace Pelican::DevCli {

int runGraphCatalogCommand(int argc, char *argv[]) {
    if (argc > 1) {
        std::cerr << "usage: pelican_cli graph-catalog" << std::endl;
        (void)argv;
        return -1;
    }
    std::cout << builtinGraphModuleRegistry().catalog().dump(2) << std::endl;
    return 0;
}

} // namespace Pelican::DevCli
