#include "materialcommand.hpp"

#include "../project/materiallowering.hpp"
#include "../project/materialformat.hpp"
#include "../project/surfaceformat.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace Pelican::DevCli {

int runDumpLoweredMaterialCommand(int argc, char *argv[]) {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: pelican_cli dump-lowered-material <surface> [material.json] [material-name]"
                  << std::endl;
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
        auto lowered = lowerSurfaceDefaults(surface, path.generic_string());
        if (argc >= 3) {
            const std::filesystem::path material_path{argv[2]};
            std::ifstream material_file{material_path, std::ios_base::binary};
            if (!material_file.is_open()) {
                throw std::runtime_error("material '" + material_path.string() +
                                         "' could not be opened");
            }
            const auto json = nlohmann::json::parse(material_file);
            MaterialSurfaceCatalog catalog;
            if (const auto materials = json.find("materials");
                materials != json.end() && materials->is_array()) {
                for (const auto &entry : *materials) {
                    if (entry.is_object() && entry.contains("surface") &&
                        entry.at("surface").is_string()) {
                        catalog.emplace(entry.at("surface").get<std::string>(), surface);
                    }
                }
            }
            const auto document = parseMaterialFormatJson(json, catalog);
            const MaterialDefinition *selected = nullptr;
            for (const auto &material : document.materials) {
                if ((argc == 4 && material.name == argv[3]) ||
                    (argc == 3 && document.materials.size() == 1)) {
                    selected = &material;
                    break;
                }
            }
            if (selected == nullptr) {
                throw std::runtime_error(argc == 4
                    ? "material name '" + std::string{argv[3]} + "' was not found"
                    : "material document contains multiple materials; provide material-name");
            }
            if (!selected->surface) {
                throw std::runtime_error("material '" + selected->name +
                                         "' has no .surface reference");
            }
            lowered = lowerMaterial(*selected, surface);
        }
        std::cout << dumpLoweredMaterial(lowered);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "dump-lowered-material: " << error.what() << std::endl;
        return 1;
    }
}

} // namespace Pelican::DevCli
