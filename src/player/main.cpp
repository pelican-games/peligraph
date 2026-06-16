#include <components/predefined.hpp>
#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/coredist.hpp>
#include <gameobjects.hpp>
#include <argparse/argparse.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <pelican_core.hpp>

#include "../core/container.hpp"
#include "../core/launchconfig.hpp"
#include "../core/log.hpp"

namespace {

struct ParsedLaunchConfig {
    Pelican::EngineLaunchConfig engine;
    std::string project_settings{"{}"};
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

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("failed to open project settings file: " + path.string());
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

ParsedLaunchConfig parseLaunchConfig(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Player");
    program.add_argument("--headless").flag().help("run without a window");
    program.add_argument("--frames").default_value(3).scan<'i', int>().help("headless frame count");
    program.add_argument("--size").default_value(std::string{"1280x720"}).metavar("WxH").help("headless render size");
    program.add_argument("--render-out").default_value(std::string{}).metavar("path").help("render output path");
    program.add_argument("--project-settings")
        .default_value(std::string{})
        .metavar("settings.json")
        .help("load project settings JSON passed to PelicanCore");
    program.add_argument("--fps").default_value(60.0).scan<'g', double>().help("headless fixed-step frame rate");
    program.add_argument("--play-seq")
        .default_value(std::string{})
        .metavar("path.jsonl")
        .help("play a pelican.transform_seq JSONL file");
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
        config.shader_hot_reload = !config.headless;

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

        const auto project_settings = program.get<std::string>("--project-settings");
        if (!project_settings.empty()) {
            parsed.project_settings = readTextFile(resolveExistingCliFile(project_settings, "--project-settings"));
        }

        config.fps = program.get<double>("--fps");
        if (config.fps <= 0.0) {
            throw std::runtime_error("--fps must be positive");
        }

        const auto play_seq = program.get<std::string>("--play-seq");
        if (!play_seq.empty()) {
            config.play_seq = resolveExistingCliFile(play_seq, "--play-seq");

            const auto seq_mesh = program.get<std::string>("--seq-mesh");
            if (seq_mesh == "builtin:sphere") {
                config.seq_mesh = std::filesystem::path{seq_mesh};
            } else {
                config.seq_mesh = resolveExistingCliFile(seq_mesh, "--seq-mesh");
            }
            config.seq_loop = program.get<bool>("--seq-loop");
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

struct MyCharComponent {
    float x;

    template <class TArchive> void ref(TArchive &ar) { ar.prop("x", x); }
};

DECLARE_COMPONENT(MyCharComponent, 32);

class MyCharSystem {
  public:
    using QueryComponents = std::tuple<MyCharComponent *, Pelican::LocalTransformComponent *>;
    int timer = 0;

    void process(QueryComponents components, size_t count) {
        auto m = std::get<MyCharComponent *>(components);
        auto t = std::get<Pelican::LocalTransformComponent *>(components);

        for (int i = 0; i < count; i++) {
            t[i].pos = Pelican::vec3{m[i].x, 0, 0};
            t[i].scale = Pelican::vec3{0.1, 0.1, 0.1};
            t[i].rotation = Pelican::quat{0, 0, 0, 1};

            m[i].x += 0.05;
        }

        // timer++;
        // if (timer == 10)
        //     Pelican::GameObjects::add()
        //         .addComponent<Pelican::TransformComponent>()
        //         .addComponent<Pelican::LocalTransformComponent>(t[0])
        //         .addComponent<Pelican::SimpleModelViewComponent>()
        //         .finish();
    }
};

int main(int argc, char *argv[]) {
    ParsedLaunchConfig parsed_launch_config;
    try {
        parsed_launch_config = parseLaunchConfig(argc, argv);
    } catch (const std::exception &) {
        return -1;
    }

    const auto &launch_config = parsed_launch_config.engine;
    Pelican::PelicanCore pl{parsed_launch_config.project_settings};
    Pelican::FastModuleContainer::get<Pelican::EngineLaunchConfig>() = launch_config;
    if (launch_config.headless) {
        LOG_INFO(Pelican::logger, "headless mode enabled");
    }

    auto &cr = Pelican::internal::getComponentRegisterer();
    auto &ecs = Pelican::internal::getEcsCore();

    cr.registerComponent<MyCharComponent>("mychar");

    MyCharSystem sys;
    ecs.registerSystem<MyCharSystem, MyCharComponent, Pelican::LocalTransformComponent>(sys, {}, true);

    pl.run();
    return 0;
}
