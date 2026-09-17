#include "view/uimain.hpp"
#include <argparse/argparse.hpp>
#include <iostream>

int main(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Studio");
    program.add_argument("--project")
        .default_value(std::string{})
        .metavar("dir")
        .help("open this project directory at startup");
    program.add_argument("--tab")
        .default_value(std::string{})
        .metavar("name")
        .help("bring one render-tools tab to the front: " +
              PelicanStudio::renderToolsTabList());

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return -1;
    }

    PelicanStudio::LaunchOptions options;
    options.project = program.get<std::string>("--project");
    options.tab = program.get<std::string>("--tab");
    // A misspelled tab has to fail on the console. Opening the window and
    // showing whatever happened to be in front would look like it worked.
    if (!options.tab.empty() && !PelicanStudio::isRenderToolsTab(options.tab)) {
        std::cerr << "pelican_studio: unknown --tab \"" << options.tab
                  << "\"; expected one of " << PelicanStudio::renderToolsTabList()
                  << std::endl;
        return 2;
    }

    return PelicanStudio::uimain(argc, argv, std::move(options));
}
