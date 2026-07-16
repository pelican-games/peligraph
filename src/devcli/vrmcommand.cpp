#include "vrmcommand.hpp"

#include "../core/model/vrmsemantic.hpp"

#include <tiny_gltf.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace Pelican::DevCli {

int runVrmCommand(int argc, char *argv[]) {
    if (argc != 3 || std::string_view{argv[1]} != "dump") {
        std::cerr << "usage: pelican_cli vrm dump <model.vrm>" << std::endl;
        return 2;
    }

    const std::filesystem::path path{argv[2]};
    try {
        tinygltf::TinyGLTF loader;
        tinygltf::Model model;
        std::string errors;
        std::string warnings;
        if (!loader.LoadBinaryFromFile(&model, &errors, &warnings, path.string())) {
            throw std::runtime_error("failed to parse GLB container '" + path.string() +
                                     "': " + errors);
        }
        if (!warnings.empty()) std::cerr << "WARNING: " << warnings << '\n';

        const auto decoded = decodeVrmSemantic(model, path.generic_string());
        for (const auto &diagnostic : decoded.diagnostics) {
            std::cerr << (diagnostic.severity == VrmDiagnosticSeverity::info ? "INFO: "
                                                                              : "WARNING: ")
                      << diagnostic.message << '\n';
        }
#ifdef _WIN32
        // Canonical dump bytes use LF on every platform, including redirected
        // Windows stdout. Diagnostics intentionally remain normal text mode.
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        if (decoded.semantic) {
            std::cout << dumpVrmSemanticCanonical(*decoded.semantic);
        } else {
            std::cout << "null\n";
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "vrm dump: " << error.what() << std::endl;
        return 1;
    }
}

} // namespace Pelican::DevCli
