#include "battery/embed.hpp"
#include <argparse/argparse.hpp>
#include <filesystem>
#include <fstream>
#include <mustache.hpp>
#include <sstream>
using namespace kainjow;

std::filesystem::path appended(std::filesystem::path path, auto child) {
    path.append(child);
    return path;
}

void create_file(std::filesystem::path path, const std::string &data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream{path} << data;
}

int main(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Studio");
    program.add_argument("--build").default_value(false).implicit_value(true).help("build project");
    program.add_argument("--prebuild").default_value(false).implicit_value(true).help("prebuild project");
    program.add_argument("dir").help("project directory");
    program.add_argument("-o").metavar("outdir").default_value("").help("specifies output directory");

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return -1;
    }

    bool mode_prebuild = program.get<bool>("--prebuild");
    bool mode_build = program.get<bool>("--build");

    std::filesystem::path pelican_path = std::filesystem::absolute(argv[0]).parent_path();
    std::filesystem::path project_path = std::filesystem::absolute(program.get("dir"));
    std::filesystem::path output_path = std::filesystem::absolute(program.get("-o"));
    if (output_path == "") {
        output_path = std::filesystem::absolute(appended(project_path, "output"));
    }
    std::filesystem::path build_path = std::filesystem::absolute(appended(project_path, "build"));
    std::filesystem::path cmake_build_path = std::filesystem::absolute(appended(build_path, "cmake_build"));

    mustache::data dat;
    dat.set("project_name", "myproject");
    dat.set("executable_name", "hoge");
    dat.set("settings", "{}");

    create_file(appended(build_path, "CMakeLists.txt"),
                mustache::mustache{b::embed<"CMakeLists.txt.template">().str()}.render(dat));
    create_file(appended(build_path, "main.cpp"),
                mustache::mustache{b::embed<"main.cpp.template">().str()}.render(dat));

    {
        std::ostringstream build_script;
        build_script << "cmake " << build_path.string() << " -B " << cmake_build_path.string()
                     << " -DPELICAN_OUTPUT_DIR=" << output_path.string()
                     << " -DPELICAN_LIB_DIR=" << appended(pelican_path, "lib/bin").string()
                     << " -DPELICAN_INCLUDE_DIR=" << appended(pelican_path, "lib/include").string();
        std::system(build_script.str().c_str());
    }
    {
        std::ostringstream build_script;
        build_script << "cmake --build " << cmake_build_path.string() << " --config Release";
        std::system(build_script.str().c_str());
    }
}