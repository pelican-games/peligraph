#include <components/predefined.hpp>
#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>
#include <details/ecs/coredist.hpp>
#include <gameobjects.hpp>
#include <argparse/argparse.hpp>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <pelican_core.hpp>

#include "../core/container.hpp"
#include "../core/launchconfig.hpp"
#include "../core/log.hpp"

namespace {

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

Pelican::EngineLaunchConfig parseLaunchConfig(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Player");
    program.add_argument("--headless").flag().help("run without a window");
    program.add_argument("--frames").default_value(3).scan<'i', int>().help("headless frame count");
    program.add_argument("--size").default_value(std::string{"1280x720"}).metavar("WxH").help("headless render size");
    program.add_argument("--render-out").default_value(std::string{}).metavar("path").help("render output path");
    program.add_argument("--fps").default_value(60.0).scan<'g', double>().help("headless fixed-step frame rate");

    try {
        program.parse_args(argc, argv);

        Pelican::EngineLaunchConfig config;
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

        config.fps = program.get<double>("--fps");
        if (config.fps <= 0.0) {
            throw std::runtime_error("--fps must be positive");
        }

        return config;
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
    Pelican::EngineLaunchConfig launch_config;
    try {
        launch_config = parseLaunchConfig(argc, argv);
    } catch (const std::exception &) {
        return -1;
    }

    Pelican::PelicanCore pl;
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
