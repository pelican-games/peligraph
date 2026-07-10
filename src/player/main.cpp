#include <argparse/argparse.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <pelican_core.hpp>

#include "../core/build_features.hpp"
#include "../core/container.hpp"
#include "../core/launchconfig.hpp"
#include "../core/loader/assetsverification.hpp"
#include "../core/loader/pathresolver.hpp"
#include "../core/loader/projectsrc.hpp"
#include "../core/log.hpp"

namespace {

struct ParsedLaunchConfig {
    Pelican::EngineLaunchConfig engine;
    std::filesystem::path project_root;
    std::optional<std::string> project_json;
    std::optional<std::filesystem::path> user_dir_override;
    bool project_explicit = false;
    bool ignore_engine_version = false;
};

uint32_t parsePositiveUint(const std::string &value, const std::string &name) {
    size_t parsed_chars = 0;
    const int parsed_value = std::stoi(value, &parsed_chars, 10);
    if (parsed_chars != value.size() || parsed_value <= 0) {
        throw std::runtime_error(name + " must be a positive integer");
    }
    return static_cast<uint32_t>(parsed_value);
}

vk::Extent2D parseExtent(const std::string &value) {
    const auto separator = value.find_first_of("xX");
    if (separator == std::string::npos || separator == 0 || separator == value.size() - 1) {
        throw std::runtime_error("--size must be formatted as WxH");
    }

    return vk::Extent2D{parsePositiveUint(value.substr(0, separator), "width"),
                        parsePositiveUint(value.substr(separator + 1), "height")};
}

std::vector<double> parseCommaNumbers(const std::string &value, size_t expected_count, const std::string &name) {
    std::vector<double> values;
    std::istringstream stream{value};
    std::string token;
    while (std::getline(stream, token, ',')) {
        if (token.empty()) {
            throw std::runtime_error(name + " contains an empty value");
        }
        size_t parsed_chars = 0;
        values.push_back(std::stod(token, &parsed_chars));
        if (parsed_chars != token.size()) {
            throw std::runtime_error(name + " contains a non-numeric value");
        }
    }
    if (values.size() != expected_count) {
        throw std::runtime_error(name + " must contain " + std::to_string(expected_count) + " comma-separated values");
    }
    return values;
}

Pelican::EngineLaunchCameraOverride parseCameraOverride(const std::string &value) {
    const auto values = parseCommaNumbers(value, 7, "--camera");
    if (values[6] <= 0.0) {
        throw std::runtime_error("--camera fov_deg must be positive");
    }
    return Pelican::EngineLaunchCameraOverride{
        .position =
            {static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])},
        .target =
            {static_cast<float>(values[3]), static_cast<float>(values[4]), static_cast<float>(values[5])},
        .fov_y = static_cast<float>(values[6]),
    };
}

std::filesystem::path resolveExistingCliFile(const std::string &value, const std::string &name) {
    if (value.empty()) {
        throw std::runtime_error(name + " must not be empty");
    }

    auto path = std::filesystem::path{value};
    if (path.is_relative()) {
        path = std::filesystem::current_path() / path;
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error(name + " file not found: " + path.string());
    }
    return std::filesystem::weakly_canonical(path);
}

std::filesystem::path resolveExistingProjectCliFile(const std::string &value, const std::string &name,
                                                    const std::filesystem::path &project_root,
                                                    bool allow_absolute_paths) {
    if (value.empty()) {
        throw std::runtime_error(name + " must not be empty");
    }

    auto path = std::filesystem::path{value};
    if (path.is_absolute() || path.has_root_name()) {
        if (!allow_absolute_paths) {
            throw std::runtime_error(name + " absolute paths require --allow-absolute-paths: " + value);
        }
    } else {
        path = project_root / path;
    }

    std::error_code ec;
    path = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        throw std::runtime_error(name + " failed to normalize path: " + path.string() + " (" + ec.message() + ")");
    }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error(name + " file not found: " + path.string());
    }
    return path;
}

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("failed to open project settings file: " + path.string());
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::filesystem::path weaklyCanonicalPath(const std::filesystem::path &path, const std::string &name) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        throw std::runtime_error(name + " failed to normalize path: " + path.string() + " (" + ec.message() + ")");
    }
    return canonical;
}

std::filesystem::path executableDirectory(char *argv0) {
    const auto exe_path = weaklyCanonicalPath(std::filesystem::absolute(std::filesystem::path{argv0}), "executable");
    return exe_path.parent_path();
}

std::optional<std::filesystem::path> findExampleProjectNearExecutable(const std::filesystem::path &exe_dir) {
    auto current = exe_dir;
    while (!current.empty()) {
        const auto project_file = current / "projects" / "example" / "project.json";
        if (std::filesystem::is_regular_file(project_file)) {
            return weaklyCanonicalPath(project_file.parent_path(), "implicit example project");
        }

        const auto parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return std::nullopt;
}

void configureImplicitProject(ParsedLaunchConfig &parsed, char *argv0) {
    parsed.project_root = executableDirectory(argv0);
    auto project_file = parsed.project_root / "project.json";
    if (!std::filesystem::is_regular_file(project_file)) {
        if (const auto example_project = findExampleProjectNearExecutable(parsed.project_root)) {
            parsed.project_root = *example_project;
            project_file = parsed.project_root / "project.json";
        }
    }
    if (std::filesystem::is_regular_file(project_file)) {
        parsed.project_json = readTextFile(project_file);
    }
}

void configureExplicitProject(ParsedLaunchConfig &parsed, const std::string &value) {
    if (value.empty()) {
        throw std::runtime_error("--project must not be empty");
    }

    auto path = std::filesystem::path{value};
    if (path.is_relative()) {
        path = std::filesystem::current_path() / path;
    }
    path = weaklyCanonicalPath(path, "--project");

    std::filesystem::path project_file;
    if (std::filesystem::is_directory(path)) {
        parsed.project_root = path;
        project_file = path / "project.json";
    } else if (std::filesystem::is_regular_file(path)) {
        parsed.project_root = path.parent_path();
        project_file = path;
    } else {
        throw std::runtime_error("--project must point to a directory or project.json file: " + path.string());
    }

    if (!std::filesystem::is_regular_file(project_file)) {
        throw std::runtime_error("--project project.json file not found: " + project_file.string());
    }

    parsed.project_json = readTextFile(project_file);
    parsed.project_explicit = true;
}

ParsedLaunchConfig parseLaunchConfig(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Player");
    program.add_argument("--headless").flag().help("run without a window");
    program.add_argument("--rpc").flag().help("run stdio JSON-RPC mode; requires --headless");
    program.add_argument("--frames").default_value(3).scan<'i', int>().help("headless frame count");
    program.add_argument("--size").default_value(std::string{"1280x720"}).metavar("WxH").help("headless render size");
    program.add_argument("--render-out").default_value(std::string{}).metavar("path").help("render output path");
    program.add_argument("--dump-frame-plan").flag().help("dump the resolved frame plan JSON to stderr");
    program.add_argument("--project")
        .default_value(std::string{})
        .metavar("dir|project.json")
        .help("load a Pelican project directory or project.json");
    program.add_argument("--allow-absolute-paths")
        .flag()
        .help("allow absolute paths in CLI-provided project content references");
    program.add_argument("--strict-assets")
        .flag()
        .help("treat assets manifest differences as startup errors");
    program.add_argument("--user-dir")
        .default_value(std::string{})
        .metavar("dir")
        .help("override the user:// root directory");
    program.add_argument("--ignore-engine-version")
        .flag()
        .help("warn instead of failing when project engine_min_version is newer");
    program.add_argument("--fps").default_value(60.0).scan<'g', double>().help("headless fixed-step frame rate");
    program.add_argument("--play-seq")
        .default_value(std::string{})
        .metavar("path.jsonl")
        .help("play a pelican.transform_seq JSONL file");
    program.add_argument("--play-vat")
        .default_value(std::string{})
        .metavar("path.glb")
        .help("play a GLB containing pelican.vat primitive extras");
    program.add_argument("--seq-mesh")
        .default_value(std::string{"builtin:sphere"})
        .metavar("builtin:sphere|path.glb")
        .help("mesh used for all transform_seq objects");
    program.add_argument("--seq-loop").flag().help("loop the transform_seq clip");
    program.add_argument("--camera")
        .default_value(std::string{})
        .metavar("px,py,pz,tx,ty,tz,fov_deg")
        .help("override camera position, target, and vertical FOV");

    try {
        program.parse_args(argc, argv);

        ParsedLaunchConfig parsed;
        auto &config = parsed.engine;
        config.headless = program.get<bool>("--headless");
        config.rpc = program.get<bool>("--rpc");
        if (config.rpc && !config.headless) {
            throw std::runtime_error("--rpc requires --headless in protocol v1");
        }
#if !PELICAN_WITH_RPC
        if (config.rpc) {
            Pelican::throwBuildFeatureDisabled("PELICAN_WITH_RPC", "--rpc is unavailable");
        }
#endif
        config.shader_hot_reload = !config.headless;
        config.allow_absolute_paths = program.get<bool>("--allow-absolute-paths");
        config.strict_assets = program.get<bool>("--strict-assets");
        parsed.ignore_engine_version = program.get<bool>("--ignore-engine-version");
        const auto user_dir = program.get<std::string>("--user-dir");
        if (!user_dir.empty()) {
            auto user_dir_path = std::filesystem::path{user_dir};
            if (user_dir_path.is_relative()) {
                user_dir_path = std::filesystem::current_path() / user_dir_path;
            }
            parsed.user_dir_override = weaklyCanonicalPath(user_dir_path, "--user-dir");
        }

        const auto project = program.get<std::string>("--project");
        if (project.empty()) {
            configureImplicitProject(parsed, argv[0]);
        } else {
            configureExplicitProject(parsed, project);
        }

        const int frames = program.get<int>("--frames");
        if (frames < 0) {
            throw std::runtime_error("--frames must be zero or greater");
        }
        config.headless_frames = static_cast<uint32_t>(frames);

        config.headless_extent = parseExtent(program.get<std::string>("--size"));

        const auto render_out = program.get<std::string>("--render-out");
        if (!render_out.empty()) {
            config.render_out = std::filesystem::path{render_out};
        }
        config.dump_frame_plan = program.get<bool>("--dump-frame-plan");

        config.fps = program.get<double>("--fps");
        if (config.fps <= 0.0) {
            throw std::runtime_error("--fps must be positive");
        }

        const auto play_seq = program.get<std::string>("--play-seq");
        if (!play_seq.empty()) {
#if !PELICAN_WITH_SEQPLAYER
            Pelican::throwBuildFeatureDisabled("PELICAN_WITH_SEQPLAYER", "--play-seq is unavailable");
#else
            config.play_seq = resolveExistingCliFile(play_seq, "--play-seq");

            const auto seq_mesh = program.get<std::string>("--seq-mesh");
            if (seq_mesh == "builtin:sphere") {
                config.seq_mesh = std::filesystem::path{seq_mesh};
            } else {
                config.seq_mesh = resolveExistingCliFile(seq_mesh, "--seq-mesh");
            }
            config.seq_loop = program.get<bool>("--seq-loop");
#endif
        }

        const auto play_vat = program.get<std::string>("--play-vat");
        if (!play_vat.empty()) {
#if !PELICAN_WITH_VAT
            Pelican::throwBuildFeatureDisabled("PELICAN_WITH_VAT", "--play-vat is unavailable");
#else
            config.play_vat = resolveExistingProjectCliFile(play_vat, "--play-vat", parsed.project_root,
                                                            config.allow_absolute_paths);
#endif
        }

        const auto camera = program.get<std::string>("--camera");
        if (!camera.empty()) {
            config.camera_override = parseCameraOverride(camera);
        }

        return parsed;
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        throw;
    }
}

} // namespace

int main(int argc, char *argv[]) {
    ParsedLaunchConfig parsed_launch_config;
    try {
        parsed_launch_config = parseLaunchConfig(argc, argv);
    } catch (const std::exception &) {
        return -1;
    }

    const auto &launch_config = parsed_launch_config.engine;
    Pelican::PelicanCore pl{"{}", launch_config.rpc};
    Pelican::FastModuleContainer::get<Pelican::EngineLaunchConfig>() = launch_config;
    auto &path_resolver = Pelican::FastModuleContainer::get<Pelican::PathResolver>();
    if (parsed_launch_config.project_json) {
        path_resolver.setup(parsed_launch_config.project_root, launch_config.allow_absolute_paths,
                            *parsed_launch_config.project_json, parsed_launch_config.user_dir_override);
    } else {
        path_resolver.setup(parsed_launch_config.project_root, launch_config.allow_absolute_paths);
    }
    const auto assets_summary = Pelican::verifyAssetsAtStartup(
        path_resolver.stores(), parsed_launch_config.project_root / ".pelican" / "assets-hash-cache.json",
        launch_config.strict_assets);
    if (!assets_summary.shouldContinueLoading()) {
        return 1;
    }
    auto &project_source = Pelican::FastModuleContainer::get<Pelican::ProjectSource>();
    if (parsed_launch_config.project_json) {
        project_source.setProjectData(*parsed_launch_config.project_json);
    }
    project_source.setIgnoreEngineVersion(parsed_launch_config.ignore_engine_version);

    if (!parsed_launch_config.project_explicit) {
        LOG_WARNING(Pelican::logger, "implicit project root = {} (pass --project to silence)",
                    parsed_launch_config.project_root.string());
    }
    if (launch_config.headless) {
        LOG_INFO(Pelican::logger, "headless mode enabled");
    }

    return pl.run() ? 0 : 1;
}
