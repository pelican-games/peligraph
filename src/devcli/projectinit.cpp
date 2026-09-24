#include "projectinit.hpp"

#include "../project/projecttemplate.hpp"

#include <argparse/argparse.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican::DevCli {

namespace {

std::string templateChoices() {
    std::string choices;
    for (const ProjectTemplate kind : projectTemplates()) {
        if (!choices.empty()) {
            choices += "|";
        }
        choices += projectTemplateName(kind);
    }
    return choices;
}

ProjectInitResult runProjectInitCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli project init");
    program.add_argument("dir").help("project directory to create");
    program.add_argument("--template")
        .default_value(std::string{projectTemplateName(ProjectTemplate::Standard)})
        .help("the template to start from: " + templateChoices());

    program.parse_args(argc, argv);
    const auto name = program.get<std::string>("--template");
    const auto kind = projectTemplateFromName(name);
    if (!kind) {
        throw std::runtime_error("unknown project template: " + name + " (expected " + templateChoices() + ")");
    }
    return initializeProjectTemplate(program.get<std::string>("dir"), *kind);
}

} // namespace

int runProjectCommand(int argc, char *argv[]) {
    if (argc > 1 && std::string_view{argv[1]} == "init") {
        try {
            const auto result = runProjectInitCommand(argc - 1, argv + 1);
            std::cout << "created project " << result.project_root.string() << " (" << result.files_written
                      << " files)" << std::endl;
        } catch (const std::exception &err) {
            std::cerr << err.what() << std::endl;
            return -1;
        }
        return 0;
    }

    std::cerr << "unknown project subcommand";
    if (argc > 1) {
        std::cerr << ": " << argv[1];
    }
    std::cerr << std::endl;
    std::cerr << "usage: pelican_cli project init <dir> [--template " << templateChoices() << "]" << std::endl;
    return -1;
}

} // namespace Pelican::DevCli
