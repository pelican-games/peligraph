#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/container.hpp"
#include "../src/core/ecs/componentinfo.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/shader/raygrouplayout.hpp"
#include "../src/core/userpublic/details/component/registerer.hpp"
#include "../src/core/userpublic/details/ecs/componentdeclare.hpp"
#include "scene_load_test_support.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

struct Wp379PolicyProbeComponent {};
DECLARE_COMPONENT(Wp379PolicyProbeComponent, 63);

namespace {

using Catch::Matchers::ContainsSubstring;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

class TemporaryProject final {
  public:
    std::filesystem::path root;

    TemporaryProject() {
        root = std::filesystem::temp_directory_path() /
               ("pelican_wp379_cpu_" +
                std::to_string(std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
        std::filesystem::create_directories(root);
    }

    ~TemporaryProject() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void write(std::string_view relative, std::string_view bytes) const {
        const auto path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        if (!output) {
            throw std::runtime_error("failed to create WP379 CPU fixture");
        }
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

void configureNoLayoutProject(const TemporaryProject &project) {
    // Intentional old-name occurrence: no-layout/unregistered negative.
    project.write(
        "scene.json",
        Json{{"schema", "pelican.scene"},
             {"version", 1},
             {"scenes",
              {{"main",
                {{"objects",
                  Json::array({{{"name", "LegacyName"},
                                {"components", Json::array(
                                                   {{{"name", "ray_instance"},
                                                     {"marker", 379}}})}}})}}}}}}
            .dump(2));
    project.write(
        "assets.json",
        Json{{"schema", "pelican.asset_data"},
             {"version", 1},
             {"models", Json::array()},
             {"textures", Json::array()}}
            .dump(2));
    project.write(
        "rendering.json",
        Json{{"rendering_passes",
              Json::array(
                  {{{"name", "main_render"},
                    {"passes",
                     Json::array(
                         {{{"name", "present"},
                           {"type", "fullscreen"},
                           {"output",
                            {{"color", Json::array({"swapchain"})},
                             {"depth", nullptr}}},
                           {"shader",
                            {{"vertex", "engine://fullscreen"},
                             {"fragment",
                              "engine://shader_lab_hello"}}}}})}}})}}
            .dump(2));

    GET_MODULE(PathResolver).setup(project.root, false);
    GET_MODULE(ProjectSource).setSourceByData(
        Json{{"basic_config",
              {{"window_size", {{"width", 32}, {"height", 32}}},
               {"scene_data_json", "scene.json"},
               {"asset_data_json", "assets.json"},
               {"default_scene_id", "main"},
               {"rendering_config_json", "rendering.json"},
               {"default_rendering_pass", "main_render"}}}}
            .dump());
    GET_MODULE(RayLayoutRuntimeState)
        .configure(RayLayoutSemanticMode::LegacyNoLayout);
    GET_MODULE(ECSPredefinedRegistration).reg();
}

template <class Register>
void requirePolicyError(
    Register &&registration,
    internal::ComponentRegistrationPolicyErrorCode expected_code,
    const OrderedJson &expected_payload) {
    try {
        (void)registration();
        FAIL("component registration policy accepted a rejected row");
    } catch (const internal::ComponentRegistrationPolicyError &error) {
        CHECK(error.code() == expected_code);
        CHECK(error.payload() == expected_payload);
        CHECK(std::string{error.what()} ==
              std::string{internal::componentRegistrationPolicyErrorCodeName(
                  expected_code)} +
                  expected_payload.dump());
    }
}

} // namespace

TEST_CASE("WP379 component namespace policy is table driven at the registration entry",
          "[WP379][component][namespace][policy][cpu][mutation]") {
    setupLogger();
    FastModuleContainer modules;
    auto &registerer = internal::getComponentRegisterer();

    constexpr std::array<std::string_view, 7> grandfathered{
        "eid",          "transform", "localtransform", "simplemodelview",
        "camera",       "animation", "sprite_view",
    };
    for (const auto name : grandfathered) {
        DYNAMIC_SECTION("engine grandfathered name " << name) {
            const auto token =
                registerer.registerEngineComponent<Wp379PolicyProbeComponent>(
                    std::string{name});
            REQUIRE(static_cast<bool>(token));
            internal::unregisterComponent(token);
        }
    }

    for (const auto name : {"foo", "ray_instance", "sprite"}) {
        DYNAMIC_SECTION("engine unprefixed rejection " << name) {
            requirePolicyError(
                [&] {
                    return registerer
                        .registerEngineComponent<Wp379PolicyProbeComponent>(
                            name);
                },
                internal::ComponentRegistrationPolicyErrorCode::
                    builtin_name_unprefixed,
                OrderedJson{{"name", name}, {"owner_kind", "engine"}});
        }
    }

    TestSupport::ScopedTestRegistrationOwner external_owner;
    for (const auto name :
         {"pelican_foo", "pelican_ray_instance", "pelican_future"}) {
        DYNAMIC_SECTION("external reserved rejection " << name) {
            requirePolicyError(
                [&] {
                    return registerer.registerComponent<
                        Wp379PolicyProbeComponent>(name);
                },
                internal::ComponentRegistrationPolicyErrorCode::
                    reserved_component_namespace,
                OrderedJson{{"name", name}, {"owner_kind", "external"}});
        }
    }
    for (const auto name : {"foo", "ray_instance", "sprite"}) {
        DYNAMIC_SECTION("external unprefixed acceptance " << name) {
            const auto token =
                registerer.registerComponent<Wp379PolicyProbeComponent>(name);
            REQUIRE(static_cast<bool>(token));
            internal::unregisterComponent(token);
        }
    }
}

TEST_CASE("WP379 policy payload ignores recycled registration owner identity",
          "[WP379][component][namespace][owner-generation][cpu]") {
    setupLogger();
    FastModuleContainer modules;
    auto &registerer = internal::getComponentRegisterer();
    OrderedJson first_payload;
    OrderedJson second_payload;

    const auto first = internal::allocateRegistrationOwner();
    {
        internal::ScopedRegistrationOwner scope{first};
        try {
            (void)registerer.registerComponent<Wp379PolicyProbeComponent>(
                "pelican_recycled");
            FAIL("reserved namespace unexpectedly accepted");
        } catch (const internal::ComponentRegistrationPolicyError &error) {
            first_payload = error.payload();
        }
    }
    internal::releaseRegistrationOwner(first);

    const auto second = internal::allocateRegistrationOwner();
    REQUIRE(internal::registrationOwnerIdentity(first) ==
            internal::registrationOwnerIdentity(second));
    REQUIRE(internal::registrationOwnerGeneration(first) !=
            internal::registrationOwnerGeneration(second));
    {
        internal::ScopedRegistrationOwner scope{second};
        try {
            (void)registerer.registerComponent<Wp379PolicyProbeComponent>(
                "pelican_recycled");
            FAIL("reserved namespace unexpectedly accepted");
        } catch (const internal::ComponentRegistrationPolicyError &error) {
            second_payload = error.payload();
        }
    }
    internal::releaseRegistrationOwner(second);

    CHECK(first_payload.dump() == second_payload.dump());
    CHECK(first_payload == OrderedJson{{"name", "pelican_recycled"},
                                       {"owner_kind", "external"}});
}

TEST_CASE("WP379 production engine registry has exactly seven unprefixed builtins",
          "[WP379][component][namespace][registry][cpu][mutation]") {
    setupLogger();
    FastModuleContainer modules;
    GET_MODULE(RayLayoutRuntimeState).configure(RayLayoutSemanticMode::LayoutV2);
    GET_MODULE(ECSPredefinedRegistration).reg();

    const auto names = internal::componentRegistrationNames(
        internal::engineRegistrationOwner);
    std::vector<std::string> unprefixed;
    std::ranges::copy_if(names, std::back_inserter(unprefixed),
                         [](const auto &name) {
                             return !name.starts_with("pelican_");
                         });
    const std::vector<std::string> expected{
        "animation", "camera", "eid", "localtransform", "simplemodelview",
        "sprite_view", "transform",
    };
    CHECK(unprefixed == expected);
    CHECK(std::ranges::find(names, "pelican_ray_instance") != names.end());
}

TEST_CASE("WP379 scope-less component registration cannot acquire engine ownership",
          "[WP379][component][namespace][owner][cpu]") {
    setupLogger();
    FastModuleContainer modules;
    CHECK_THROWS_WITH(
        internal::getComponentRegisterer()
            .registerComponent<Wp379PolicyProbeComponent>("eid"),
        ContainsSubstring("allocated external owner") &&
            ContainsSubstring("explicit engine registrar"));
}

TEST_CASE("WP379 no-layout SceneLoader treats the old name as an unknown component",
          "[WP379][component][namespace][scene-loader][cpu][negative]") {
    setupLogger();
    TemporaryProject project;
    FastModuleContainer modules;
    configureNoLayoutProject(project);

    try {
        test_support::loadScene(GET_MODULE(SceneLoader), "main");
        FAIL("old unregistered component name unexpectedly resolved");
    } catch (const ComponentUnknownError &error) {
        CHECK(std::string{error.code()} == "component_unknown@1");
        CHECK(error.payload() == OrderedJson{{"name", "ray_instance"}});
        CHECK(std::string{error.what()} ==
              "component_unknown@1{\"name\":\"ray_instance\"}");
    }
}

} // namespace Pelican
