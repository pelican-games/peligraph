#include "distconfig.hpp"
#include "importcommand.hpp"
#include "projectinit.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char *argv[]) {
    if (argc > 1 && std::string_view{argv[1]} == "import") {
        return Pelican::DevCli::runImportCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "dist-config") {
        return Pelican::DevCli::runDistConfigCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "project") {
        return Pelican::DevCli::runProjectCommand(argc - 1, argv + 1);
    }

    std::cerr << "usage: pelican_cli <import|dist-config|project> ..." << std::endl;
    return -1;
}
