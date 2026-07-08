#pragma once

#include <filesystem>

namespace Pelican::DevCli {

struct ProjectInitResult {
    std::filesystem::path project_root;
    int files_written = 0;
};

ProjectInitResult initializeProjectTemplate(const std::filesystem::path &project_dir);

int runProjectCommand(int argc, char *argv[]);

} // namespace Pelican::DevCli
