#include "materialcommand.hpp"

#include "../project/materiallowering.hpp"
#include "../project/materialformat.hpp"
#include "../project/renderconfigsection.hpp"
#include "../project/surfaceformat.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican::DevCli {

namespace {

std::string readTextFile(const std::filesystem::path &path, std::string_view what) {
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error(std::string{what} + " '" + path.string() +
                                 "' could not be opened");
    }
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
}

// G-4: the project's rendering document decides which route an automatic
// material takes, exactly as the engine reads it at start.
MaterialLoweringCapabilities projectLoweringCapabilities(
    const std::filesystem::path &project_dir) {
    const auto project = nlohmann::json::parse(
        readTextFile(project_dir / "project.json", "project"));
    const auto basic = project.find("basic_config");
    if (basic == project.end() || !basic->is_object() ||
        !basic->contains("rendering_config_json") ||
        !basic->at("rendering_config_json").is_string()) {
        throw std::runtime_error("project '" + project_dir.string() +
                                 "' has no basic_config.rendering_config_json");
    }
    auto reference = basic->at("rendering_config_json").get<std::string>();
    constexpr std::string_view project_scheme = "project://";
    if (reference.starts_with(project_scheme)) {
        reference.erase(0, project_scheme.size());
    }
    const auto rendering = nlohmann::json::parse(
        readTextFile(project_dir / reference, "rendering config"));
    MaterialLoweringCapabilities capabilities;
    capabilities.route_default =
        readMaterialRouteDefault(renderingSectionOrEmpty(rendering));
    return capabilities;
}

} // namespace

int runDumpLoweredMaterialCommand(int argc, char *argv[]) {
    std::optional<std::filesystem::path> project_dir;
    if (argc >= 3 && std::string_view{argv[1]} == "--project") {
        project_dir = std::filesystem::path{argv[2]};
        argv[2] = argv[0];
        argc -= 2;
        argv += 2;
    }
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: pelican_cli dump-lowered-material [--project <dir>] <surface> "
                     "[material.json] [material-name]"
                  << std::endl;
        return 2;
    }

    const std::filesystem::path path{argv[1]};
    try {
        const auto capabilities = project_dir
                                      ? projectLoweringCapabilities(*project_dir)
                                      : MaterialLoweringCapabilities{};
        std::ifstream file{path, std::ios_base::binary};
        if (!file.is_open()) {
            throw std::runtime_error("surface '" + path.string() + "' could not be opened");
        }
        const std::string source{std::istreambuf_iterator<char>{file},
                                 std::istreambuf_iterator<char>{}};
        const auto surface = parseSurfaceFormat(source, path.string());
        auto lowered = lowerSurfaceDefaults(surface, path.generic_string(), capabilities);
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
            lowered = lowerMaterial(*selected, surface, capabilities);
        }
        std::cout << dumpLoweredMaterial(lowered);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "dump-lowered-material: " << error.what() << std::endl;
        return 1;
    }
}

} // namespace Pelican::DevCli
