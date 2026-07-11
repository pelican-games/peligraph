#pragma once

#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string_view>

namespace Pelican::DevCli {

nlohmann::json extractGltfScene(const std::filesystem::path &glb_path,
                                std::string_view source_reference = {});

int runGltfImportCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
