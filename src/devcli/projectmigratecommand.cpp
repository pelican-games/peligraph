#include "projectmigratecommand.hpp"

#include "../project/projectmigration.hpp"

#include <argparse/argparse.hpp>

#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <string>

namespace Pelican::DevCli {
namespace {

std::filesystem::path projectRootFromArgument(const std::string &argument) {
    auto path = std::filesystem::path{argument};
    if (path.is_relative()) path = std::filesystem::current_path() / path;
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error)) {
        if (path.filename() != "project.json") {
            throw std::runtime_error("--project file must be named project.json: " +
                                     path.string());
        }
        return path.parent_path();
    }
    if (!std::filesystem::is_directory(path, error) ||
        !std::filesystem::is_regular_file(path / "project.json", error)) {
        throw std::runtime_error(
            "--project must point to a directory holding project.json, or to "
            "project.json: " +
            path.string());
    }
    return path;
}

} // namespace

int runProjectMigrateCommand(int argc, char *argv[],
                             const DocumentMigrationRegistry &registry,
                             std::ostream &out, std::ostream &err) {
    argparse::ArgumentParser program("Pelican Cli project migrate");
    program.add_argument("--project")
        .required()
        .metavar("dir|project.json")
        .help("the project to move to this engine's document versions");
    program.add_argument("--dry-run")
        .default_value(false)
        .implicit_value(true)
        .help("print the plan and write nothing");
    try {
        program.parse_args(argc, argv);
        const auto root = projectRootFromArgument(program.get<std::string>("--project"));
        if (program.get<bool>("--dry-run")) {
            auto plan = planProjectMigration(root, registry).toJson();
            plan["dry_run"] = true;
            out << plan.dump(2) << std::endl;
        } else {
            auto result = applyProjectMigration(root, registry).toJson();
            result["dry_run"] = false;
            out << result.dump(2) << std::endl;
        }
    } catch (const std::exception &error) {
        err << error.what() << std::endl;
        return -1;
    }
    return 0;
}

} // namespace Pelican::DevCli
