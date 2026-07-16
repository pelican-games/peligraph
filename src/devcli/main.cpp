#include "assetscommand.hpp"
#include "bakecameracommand.hpp"
#include "distconfig.hpp"
#include "importcommand.hpp"
#include "materialcommand.hpp"
#include "projectinit.hpp"
#include "vrmcommand.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char *argv[]) {
    if (argc > 1 && std::string_view{argv[1]} == "assets") {
        return Pelican::DevCli::runAssetsCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "bake-camera") {
        return Pelican::DevCli::runBakeCameraCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "import") {
        return Pelican::DevCli::runImportCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "dist-config") {
        return Pelican::DevCli::runDistConfigCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "project") {
        return Pelican::DevCli::runProjectCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "dump-lowered-material") {
        return Pelican::DevCli::runDumpLoweredMaterialCommand(argc - 1, argv + 1);
    }
    if (argc > 1 && std::string_view{argv[1]} == "vrm") {
        return Pelican::DevCli::runVrmCommand(argc - 1, argv + 1);
    }

    std::cerr << "usage: pelican_cli <assets|bake-camera|import|dist-config|project|dump-lowered-material|vrm> ..."
              << std::endl;
    return -1;
}
