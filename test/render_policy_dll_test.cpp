#include "../src/core/gamelogic/gamelogicreload.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderingpass/passimplementationregistry.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"
#include "../src/core/renderer/drawqueuebuilder.hpp"
#include "../src/core/renderer/renderpolicyregistry.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <Windows.h>

#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Pelican {
namespace {

using namespace std::chrono_literals;

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

std::filesystem::path requiredFixture(const char *name) {
    char *value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr ||
        value_size <= 1) {
        std::free(value);
        throw std::runtime_error(
            std::string{"missing fixture environment variable: "} + name);
    }
    const auto result = std::filesystem::absolute(value).lexically_normal();
    std::free(value);
    return result;
}

class Sandbox {
    std::filesystem::path root_path;

  public:
    Sandbox() {
        root_path = std::filesystem::temp_directory_path() /
                    ("pelican_render_policy_dll_" +
                     std::to_string(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::remove_all(root_path, error);
        error.clear();
        std::filesystem::create_directories(root_path, error);
        if (error) {
            throw std::runtime_error(
                "cannot create render policy DLL sandbox");
        }
    }

    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(root_path, error);
    }

    const std::filesystem::path &root() const noexcept { return root_path; }
};

void replaceFixture(const std::filesystem::path &source,
                    const std::filesystem::path &destination) {
    std::error_code error;
    std::filesystem::copy_file(source, destination,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
        throw std::runtime_error("cannot replace render policy fixture: " +
                                 error.message());
    }
}

struct FixtureControls {
    using StatusFn = std::uint32_t (*)();
    using VoidFn = void (*)();
    using BoolFn = bool (*)();

    StatusFn registration_status = nullptr;
    StatusFn pass_registration_status = nullptr;
    StatusFn subgraph_registration_status = nullptr;
    VoidFn arm_next_sort = nullptr;
    BoolFn sort_entered = nullptr;
    VoidFn resume_sort = nullptr;
};

template <class Function>
Function fixtureFunction(HMODULE module, const char *name) {
    const auto address = GetProcAddress(module, name);
    if (address == nullptr) {
        throw std::runtime_error(
            std::string{"render policy fixture export is missing: "} + name);
    }
    return reinterpret_cast<Function>(address);
}

FixtureControls fixtureControls(const GameLogicReloadStatus &status) {
    auto module = GetModuleHandleW(status.loaded_copy.c_str());
    if (module == nullptr) {
        module = GetModuleHandleW(status.loaded_copy.filename().c_str());
    }
    if (module == nullptr) {
        throw std::runtime_error(
            "cannot locate loaded render policy shadow DLL");
    }
    return FixtureControls{
        fixtureFunction<FixtureControls::StatusFn>(
            module, "pelican_render_policy_fixture_registration_status"),
        fixtureFunction<FixtureControls::StatusFn>(
            module, "pelican_render_pass_fixture_registration_status"),
        fixtureFunction<FixtureControls::StatusFn>(
            module, "pelican_render_subgraph_fixture_registration_status"),
        fixtureFunction<FixtureControls::VoidFn>(
            module, "pelican_render_policy_fixture_arm_next_sort"),
        fixtureFunction<FixtureControls::BoolFn>(
            module, "pelican_render_policy_fixture_sort_entered"),
        fixtureFunction<FixtureControls::VoidFn>(
            module, "pelican_render_policy_fixture_resume_sort"),
    };
}

bool waitUntilEntered(const FixtureControls &controls) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (controls.sort_entered()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return controls.sort_entered();
}

DrawItemSnapshot item(std::uint64_t ordinal, std::uint32_t instance) {
    return DrawItemSnapshot{
        .stable_identity =
            DrawStableIdentity{
                .instance = ModelInstanceId{instance, 1, 9},
                .primitive_index = instance,
            },
        .declaration_ordinal = ordinal,
        .indexed =
            DrawIndexedArguments{
                .index_count = 3,
                .instance_count = 1,
                .first_index = instance * 3,
                .first_instance = instance,
            },
        .pipeline_material_key =
            DrawPipelineMaterialKey{
                .material = GlobalMaterialId{1},
                .source_material_index = 0,
            },
        .view_mask = DrawViewMask::both,
    };
}

std::vector<std::uint32_t> buildOrder() {
    const std::vector input{item(0, 10), item(1, 20), item(2, 30)};
    const auto provider = renderPolicyRegistry().resolveDrawSortProvider(
        "fixture.draw_sort");
    const auto queue = DrawQueueBuilder::build(
        DrawQueueBuildRequest{
            .items = input,
            .max_draw_indirect_count = 64,
        },
        provider);
    std::vector<std::uint32_t> result;
    for (const auto &ordered : queue.orderedItems()) {
        result.push_back(ordered.stable_identity.instance.index);
    }
    return result;
}

std::string resolveFullscreenFragment() {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto scene = sceneLinearHdrV1(types);
    LogicalGraphNode node;
    node.name = "fixture_composite";
    node.kind = LogicalGraphNodeKind::render;
    node.ports = {
        LogicalPortContract{
            "in.scene", LogicalPortDirection::input,
            exactLogicalTypePattern(types, scene), {}},
        LogicalPortContract{
            "out.scene", LogicalPortDirection::output,
            exactLogicalTypePattern(types, scene), {}},
    };
    node.uses = {
        makeLogicalReadUse(
            "in.scene", LogicalValueId{"scene_in", 0},
            LogicalReadFootprint{
                LogicalReadFootprintKind::same_pixel,
                std::nullopt},
            LogicalAccessIntent::sampled),
        makeLogicalWriteUse(
            "out.scene", LogicalValueId{"scene_out", 1},
            LogicalAccessIntent::attachment),
    };

    PassDefinition pass;
    pass.name = node.name;
    pass.pass_info = FullscreenPassInfo{
        .vert_shader =
            makeShaderReference(
                "engine://fullscreen",
                ShaderStage::vertex),
        .frag_shader =
            makeShaderReference(
                "engine://scene_present",
                ShaderStage::fragment),
    };
    pass.requested_implementation_provider =
        "fixture.fullscreen";
    const auto providers =
        passImplementationRegistry().snapshot();
    return providers.resolveFullscreen(pass, node)
        .fragment_shader.ref;
}

std::pair<std::string, std::string>
resolveSubgraphImplementation() {
    const auto config = nlohmann::json{
        {"render_targets",
         nlohmann::json::array(
             {{{"name", "scene_in"},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"usage",
                nlohmann::json::array(
                    {"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", "scene_out"},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"usage",
                nlohmann::json::array(
                    {"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "main"},
               {"region_replacements",
                nlohmann::json::array(
                    {{{"region",
                       "region.post.fixture"},
                      {"provider",
                       "fixture.subgraph"}}})},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "pre"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "scene_in"},
                        {"depth", nullptr}}}},
                     {{"name", "tone"},
                      {"type", "fullscreen"},
                      {"regions",
                       nlohmann::json::array(
                           {"region.post.fixture"})},
                      {"input",
                       nlohmann::json::array(
                           {"scene_in"})},
                      {"output",
                       {{"color", "scene_out"},
                        {"depth", nullptr}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"input",
                       nlohmann::json::array(
                           {"scene_out"})},
                      {"output",
                       {{"color", "swapchain"},
                        {"depth", nullptr}}}}})}}})},
    };
    const auto providers =
        subgraphReplacementRegistry().snapshot();
    const auto resolved =
        resolveTaggedSubgraphReplacements(
            config, providers);
    const auto &selection =
        resolved.graphs.front().selections.front();
    const auto replacement_name =
        resolved.config.at("rendering_passes")
            .at(0)
            .at("passes")
            .at(1)
            .at("name")
            .get<std::string>();
    return {
        selection.implementation,
        replacement_name,
    };
}

} // namespace

TEST_CASE("public render providers survive reload rollback and in-flight unload",
          "[render-policy-dll]") {
    ensureLogger();
    const auto fixture_v1 = requiredFixture("PELICAN_RENDER_POLICY_FIXTURE_V1");
    const auto fixture_v2 = requiredFixture("PELICAN_RENDER_POLICY_FIXTURE_V2");
    const auto fixture_bad_abi =
        requiredFixture("PELICAN_RENDER_POLICY_FIXTURE_BAD_ABI");

    Sandbox sandbox;
    const auto live_dll = sandbox.root() / "render_policy_live.dll";
    replaceFixture(fixture_v1, live_dll);

    GameLogicReloader reloader;
    REQUIRE(reloader.initialize(live_dll));
    REQUIRE(reloader.status().generation == 1);
    auto controls = fixtureControls(reloader.status());
    REQUIRE(static_cast<RenderPolicy::Status>(
                controls.registration_status()) == RenderPolicy::Status::ok);
    REQUIRE(static_cast<RenderPass::Status>(
                controls.pass_registration_status()) ==
            RenderPass::Status::ok);
    REQUIRE(static_cast<RenderSubgraph::Status>(
                controls.subgraph_registration_status()) ==
            RenderSubgraph::Status::ok);
    REQUIRE(buildOrder() == std::vector<std::uint32_t>{30, 20, 10});
    REQUIRE(resolveFullscreenFragment() ==
            "shaders/fixture_composite_v1");
    const auto subgraph_v1 =
        resolveSubgraphImplementation();
    REQUIRE(
        subgraph_v1.first ==
        "fixture.render.subgraph_v1@1");
    REQUIRE(
        subgraph_v1.second ==
        "fixture_tone_v1");

    replaceFixture(fixture_v2, live_dll);
    controls.arm_next_sort();
    std::vector<std::uint32_t> in_flight_order;
    std::exception_ptr build_error;
    std::thread build_thread{[&] {
        try {
            in_flight_order = buildOrder();
        } catch (...) {
            build_error = std::current_exception();
        }
    }};

    const bool callback_entered = waitUntilEntered(controls);
    if (!callback_entered) {
        controls.resume_sort();
        build_thread.join();
        reloader.shutdown();
        REQUIRE(callback_entered);
    }

    std::binary_semaphore reload_started{0};
    std::binary_semaphore reload_finished{0};
    bool reload_result = false;
    std::exception_ptr reload_error;
    std::thread reload_thread{[&] {
        reload_started.release();
        try {
            reload_result = reloader.reloadNow([] {}, [] {});
        } catch (...) {
            reload_error = std::current_exception();
        }
        reload_finished.release();
    }};
    reload_started.acquire();
    const bool unloaded_while_callback_blocked =
        reload_finished.try_acquire_for(150ms);

    controls.resume_sort();
    build_thread.join();
    reload_thread.join();

    REQUIRE_FALSE(unloaded_while_callback_blocked);
    REQUIRE(build_error == nullptr);
    REQUIRE(reload_error == nullptr);
    REQUIRE(reload_result);
    REQUIRE(in_flight_order == std::vector<std::uint32_t>{30, 20, 10});
    REQUIRE(reloader.status().generation == 2);
    controls = fixtureControls(reloader.status());
    REQUIRE(static_cast<RenderPolicy::Status>(
                controls.registration_status()) == RenderPolicy::Status::ok);
    REQUIRE(static_cast<RenderPass::Status>(
                controls.pass_registration_status()) ==
            RenderPass::Status::ok);
    REQUIRE(static_cast<RenderSubgraph::Status>(
                controls.subgraph_registration_status()) ==
            RenderSubgraph::Status::ok);
    REQUIRE(buildOrder() == std::vector<std::uint32_t>{10, 20, 30});
    REQUIRE(resolveFullscreenFragment() ==
            "shaders/fixture_composite_v2");
    auto subgraph_v2 =
        resolveSubgraphImplementation();
    REQUIRE(
        subgraph_v2.first ==
        "fixture.render.subgraph_v2@1");
    REQUIRE(
        subgraph_v2.second ==
        "fixture_tone_v2");

    replaceFixture(fixture_bad_abi, live_dll);
    REQUIRE_FALSE(reloader.reloadNow([] {}, [] {}));
    REQUIRE(reloader.status().generation == 2);
    REQUIRE(buildOrder() == std::vector<std::uint32_t>{10, 20, 30});
    REQUIRE(resolveFullscreenFragment() ==
            "shaders/fixture_composite_v2");
    subgraph_v2 =
        resolveSubgraphImplementation();
    REQUIRE(
        subgraph_v2.first ==
        "fixture.render.subgraph_v2@1");
    REQUIRE(
        subgraph_v2.second ==
        "fixture_tone_v2");

    replaceFixture(fixture_v1, live_dll);
    int rebuild_calls = 0;
    REQUIRE_FALSE(reloader.reloadNow([] {}, [&] {
        if (++rebuild_calls == 1) {
            throw std::runtime_error(
                "forced render policy rebuild failure");
        }
    }));
    REQUIRE(rebuild_calls == 2);
    REQUIRE(reloader.status().generation == 2);
    REQUIRE(buildOrder() == std::vector<std::uint32_t>{10, 20, 30});
    REQUIRE(resolveFullscreenFragment() ==
            "shaders/fixture_composite_v2");
    subgraph_v2 =
        resolveSubgraphImplementation();
    REQUIRE(
        subgraph_v2.first ==
        "fixture.render.subgraph_v2@1");
    REQUIRE(
        subgraph_v2.second ==
        "fixture_tone_v2");

    reloader.shutdown();
    REQUIRE_THROWS_WITH(
        renderPolicyRegistry().resolveDrawSortProvider("fixture.draw_sort"),
        "draw sort provider 'fixture.draw_sort' is not registered for the active owner");
    REQUIRE_THROWS_WITH(
        resolveFullscreenFragment(),
        Catch::Matchers::ContainsSubstring(
            "pass implementation provider 'fixture.fullscreen' is not registered"));
    REQUIRE_THROWS_WITH(
        resolveSubgraphImplementation(),
        Catch::Matchers::ContainsSubstring(
            "subgraph replacement provider 'fixture.subgraph' is not registered"));
    const auto builtin = renderPolicyRegistry().resolveDrawSortProvider(
        builtinStateBatchedDrawSortProvider);
    REQUIRE(builtin.info().owner == internal::engineRegistrationOwner);
}

} // namespace Pelican
