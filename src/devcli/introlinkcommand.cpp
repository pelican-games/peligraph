#include "introlinkcommand.hpp"

#include "introlink.hpp"

#include <argparse/argparse.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace Pelican::DevCli {
namespace {

namespace fs = std::filesystem;

void usage() {
    std::cerr << "usage: pelican_cli intro-link bake <project> --out <header> [--shaders <dir>] [--glslc <path>]\n"
                 "       pelican_cli intro-link serve <project> [--port N] [--shaders <dir>] [--glslc <path>]"
                 " [--exit-after-client]"
              << std::endl;
}

void addCommonArguments(argparse::ArgumentParser &program) {
    program.add_argument("project").help("project directory or project.json");
    program.add_argument("--shaders")
        .default_value(std::string{"shaders"})
        .metavar("dir")
        .help("shader directory, relative to the project (top-level .vert/.frag/.comp)");
    program.add_argument("--glslc")
        .default_value(std::string{})
        .metavar("path")
        .help("glslc to compile with (default: $VULKAN_SDK/Bin, then PATH)");
}

int runBake(int argc, char *argv[]) {
    argparse::ArgumentParser program("pelican_cli intro-link bake");
    addCommonArguments(program);
    program.add_argument("--out").required().metavar("header").help("C header to write");
    program.parse_args(argc, argv);

    const auto root = IntroLink::resolveProjectRoot(program.get<std::string>("project"));
    const auto shaders_dir = IntroLink::resolveShadersDir(root, program.get<std::string>("--shaders"));
    const auto tracks = IntroLink::collectTracks(root);
    const auto sources = IntroLink::collectShaders(root, shaders_dir);
    std::map<std::string, IntroLink::ShaderBinary> binaries;
    if (!sources.empty()) {
        const auto glslc = IntroLink::findGlslc(program.get<std::string>("--glslc"));
        for (const auto &[name, source] : sources) {
            auto result = IntroLink::compileShader(glslc, shaders_dir, source);
            if (!result.ok) {
                throw IntroLink::Error("intro_link.shader_compile_failed@1",
                                       source.relative + " (glslc says below)\n" + result.log);
            }
            binaries.emplace(name, IntroLink::ShaderBinary{std::move(result.words), result.variant});
        }
    }

    const auto header = IntroLink::renderBakedHeader(tracks, binaries);
    fs::path out{program.get<std::string>("--out")};
    if (out.is_relative()) out = fs::current_path() / out;
    if (out.has_parent_path()) fs::create_directories(out.parent_path());
    std::ofstream file{out, std::ios::binary | std::ios::trunc};
    file << header;
    file.close();
    if (!file) throw std::runtime_error("failed to write " + out.string());
    std::cout << "wrote " << out.generic_string() << " (" << tracks.size() << " tracks, " << binaries.size()
              << " shaders)" << std::endl;
    // What the header puts into the intro, one line each (IL-3).
    std::size_t key_bytes = 0, spirv_bytes = 0;
    for (const auto &[name, track] : tracks) {
        const auto bytes = track.keys.size() * IntroLink::kBakedKeyBytes;
        key_bytes += bytes;
        std::cout << "track " << name << ": " << track.keys.size() << " keys, " << bytes << " bytes" << std::endl;
    }
    for (const auto &[name, shader] : binaries) {
        spirv_bytes += shader.words.size() * 4;
        std::cout << "shader " << name << ": " << shader.words.size() * 4 << " bytes (" << shader.variant << ")"
                  << std::endl;
    }
    std::cout << "total: " << key_bytes << " bytes of keys + " << spirv_bytes << " bytes of SPIR-V = "
              << key_bytes + spirv_bytes << " bytes" << std::endl;
    return 0;
}

int runServe(int argc, char *argv[]) {
    argparse::ArgumentParser program("pelican_cli intro-link serve");
    addCommonArguments(program);
    program.add_argument("--port").default_value(47611).scan<'i', int>().metavar("N").help(
        "TCP port on 127.0.0.1 (0 picks a free one); the first stdout line says `port <N>`");
    program.add_argument("--exit-after-client")
        .default_value(false)
        .implicit_value(true)
        .help("exit 0 when the first client disconnects");
    program.parse_args(argc, argv);

    IntroLink::ServeOptions options;
    options.root = IntroLink::resolveProjectRoot(program.get<std::string>("project"));
    options.shaders_dir = IntroLink::resolveShadersDir(options.root, program.get<std::string>("--shaders"));
    options.port = program.get<int>("--port");
    if (options.port < 0 || options.port > 65535) {
        throw IntroLink::Error("intro_link.port_unavailable@1",
                               "--port " + std::to_string(options.port) + " is outside 0..65535");
    }
    options.exit_after_client = program.get<bool>("--exit-after-client");
    options.glslc = IntroLink::findGlslc(program.get<std::string>("--glslc"));
    return IntroLink::runServe(options);
}

} // namespace

int runIntroLinkCommand(int argc, char *argv[]) {
    if (argc < 2) {
        usage();
        return -1;
    }
    const std::string_view verb{argv[1]};
    try {
        if (verb == "bake") return runBake(argc - 1, argv + 1);
        if (verb == "serve") return runServe(argc - 1, argv + 1);
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    std::cerr << "unknown intro-link subcommand: " << verb << std::endl;
    usage();
    return -1;
}

} // namespace Pelican::DevCli
