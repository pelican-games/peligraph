#pragma once

#include <filesystem>

namespace Pelican::DevCli {

struct ImportCommandResult {
    int verified_outputs = 0;
    int registered_models = 0;
    int skipped_models = 0;
};

ImportCommandResult importDelivery(const std::filesystem::path &delivery_dir,
                                   const std::filesystem::path &project_arg);

int runImportCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
