#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/subgraphreplacementregistry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace Pelican {
namespace {

nlohmann::json taggedRegionConfig(
    std::optional<std::string_view> provider =
        std::nullopt) {
    auto request = nlohmann::json{
        {"region", "region.post.fixture"},
    };
    if (provider) {
        request["provider"] = *provider;
    }
    return nlohmann::json{
        {"render_targets",
         nlohmann::json::array(
             {{{"name", "scene_in"},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"usage",
                nlohmann::json::array(
                    {"COLOR_ATTACHMENT", "SAMPLED"})}},
              {{"name", "scratch"},
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
                nlohmann::json::array({request})},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "pre"},
                      {"type", "fullscreen"},
                      {"before", "tone"},
                      {"output",
                       {{"color", "scene_in"},
                        {"depth", nullptr}}},
                      {"shader",
                       {{"vertex",
                         "engine://fullscreen"},
                        {"fragment",
                         "engine://scene_present"}}}},
                     {{"name", "tone"},
                      {"type", "fullscreen"},
                      {"regions",
                       nlohmann::json::array(
                           {"region.post.fixture",
                            "region.post"})},
                      {"input",
                       nlohmann::json::array(
                           {"scene_in"})},
                      {"after", "pre"},
                      {"before", "present"},
                      {"output",
                       {{"color", "scene_out"},
                        {"depth", nullptr}}},
                      {"shader",
                       {{"vertex",
                         "engine://fullscreen"},
                        {"fragment",
                         "engine://scene_present"}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"input",
                       nlohmann::json::array(
                           {"scene_out"})},
                      {"after", "tone"},
                      {"output",
                       {{"color", "swapchain"},
                        {"depth", nullptr}}},
                      {"shader",
                       {{"vertex",
                         "engine://fullscreen"},
                        {"fragment",
                         "engine://scene_present"}}}}})}}})},
    };
}

enum class ProviderBehavior {
    expand,
    change_boundary,
    malformed_json,
    unavailable,
};

struct ProviderState {
    ProviderBehavior behavior =
        ProviderBehavior::expand;
    std::uint32_t calls = 0;
    std::uint32_t port_count = 0;
    std::uint64_t fingerprint = 0;
    std::string graph;
    std::string region;
    std::string implementation =
        "fixture.render.post_subgraph@1";
    std::string replacement =
        R"json([
          {
            "name": "tone_a",
            "type": "fullscreen",
            "input": ["scene_in"],
            "output": {"color": "scratch", "depth": null},
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "engine://scene_present"
            }
          },
          {
            "name": "tone_b",
            "type": "fullscreen",
            "input": ["scratch"],
            "after": "tone_a",
            "output": {"color": "scene_out", "depth": null},
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "engine://scene_present"
            }
          }
        ])json";
};

RenderSubgraph::Status resolveRegion(
    void *context,
    const RenderSubgraph::ResolveRegionInputV1 *input,
    RenderSubgraph::RegionReplacementV1 *output) noexcept {
    if (context == nullptr || input == nullptr ||
        output == nullptr || input->contract == nullptr) {
        return RenderSubgraph::Status::invalid_argument;
    }
    auto &state = *static_cast<ProviderState *>(context);
    ++state.calls;
    const auto &contract = *input->contract;
    if (input->struct_size <
            sizeof(
                RenderSubgraph::ResolveRegionInputV1) ||
        input->version !=
            RenderSubgraph::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        contract.struct_size <
            sizeof(RenderSubgraph::RegionContractV1) ||
        contract.version !=
            RenderSubgraph::descriptorVersionV1 ||
        contract.reserved0 != 0 ||
        contract.reserved1 != 0 ||
        contract.reserved2 != 0 ||
        contract.reserved3 != 0 ||
        contract.reserved4 != 0 ||
        contract.reserved5 != 0 ||
        output->struct_size <
            sizeof(
                RenderSubgraph::RegionReplacementV1) ||
        output->version !=
            RenderSubgraph::descriptorVersionV1) {
        return RenderSubgraph::Status::invalid_argument;
    }
    state.port_count = contract.boundary_port_count;
    state.fingerprint = contract.fingerprint;
    state.graph.assign(
        contract.graph_name_utf8,
        contract.graph_name_size);
    state.region.assign(
        contract.region_tag_utf8,
        contract.region_tag_size);

    if (state.behavior ==
        ProviderBehavior::unavailable) {
        return RenderSubgraph::Status::unavailable;
    }
    *output =
        RenderSubgraph::descriptor<
            RenderSubgraph::RegionReplacementV1>();
    output->implementation_id_utf8 =
        state.implementation.data();
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            state.implementation.size());
    if (state.behavior ==
        ProviderBehavior::malformed_json) {
        static constexpr char malformed[] = "[";
        output->subgraph_json_utf8 = malformed;
        output->subgraph_json_size =
            sizeof(malformed) - 1;
        return RenderSubgraph::Status::ok;
    }
    if (state.behavior ==
        ProviderBehavior::change_boundary) {
        static constexpr char changed[] =
            R"json([{
              "name": "tone_changed",
              "type": "fullscreen",
              "input": ["scene_in"],
              "output": {"color": "scratch", "depth": null},
              "shader": {
                "vertex": "engine://fullscreen",
                "fragment": "engine://scene_present"
              }
            }])json";
        output->subgraph_json_utf8 = changed;
        output->subgraph_json_size =
            sizeof(changed) - 1;
        return RenderSubgraph::Status::ok;
    }
    output->subgraph_json_utf8 =
        state.replacement.data();
    output->subgraph_json_size =
        static_cast<std::uint32_t>(
            state.replacement.size());
    return RenderSubgraph::Status::ok;
}

RenderSubgraph::ProviderV1 provider(
    std::string_view name, ProviderState &state) {
    auto result =
        RenderSubgraph::descriptor<
            RenderSubgraph::ProviderV1>();
    result.capability_bits =
        RenderSubgraph::builtinProviderCapabilitiesV1;
    result.name_utf8 = name.data();
    result.name_size =
        static_cast<std::uint32_t>(name.size());
    result.context = &state;
    result.resolve_region = resolveRegion;
    return result;
}

template <class Function>
void requireThrowsContaining(
    Function &&function, std::string_view text) {
    try {
        function();
        FAIL("expected an exception");
    } catch (const std::exception &error) {
        REQUIRE(
            std::string_view{error.what()}.find(text) !=
            std::string_view::npos);
    }
}

const nlohmann::json &graphObject(
    const nlohmann::json &config) {
    return config.at("rendering_passes").at(0);
}

} // namespace

TEST_CASE(
    "WP201 builtin provider preserves an authored tagged region",
    "[render-subgraph][provider][wp201]") {
    SubgraphReplacementRegistry registry;
    const auto authored = taggedRegionConfig();
    ResolvedTaggedSubgraphConfig resolved;
    {
        const auto providers = registry.snapshot();
        resolved =
            resolveTaggedSubgraphReplacements(
                authored, providers);
    }

    const auto authored_graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            authored);
    const auto resolved_identity_graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            resolved.config);
    REQUIRE(
        framePlanToJson(
            planFrameGraph(
                authored_graphs.front())) ==
        framePlanToJson(
            planFrameGraph(
                resolved_identity_graphs.front())));
    REQUIRE(resolved.graphs.size() == 1);
    REQUIRE(
        resolved.graphs.front().selections.size() ==
        1);
    const auto &selection =
        resolved.graphs.front().selections.front();
    REQUIRE(
        selection.provider ==
        std::string{
            builtinTaggedSubgraphReplacementProvider});
    REQUIRE(
        selection.implementation ==
        "pelican.render.authored_subgraph@1");
    REQUIRE(
        selection.contract ==
        RenderSubgraph::regionContractIdV1);
    REQUIRE(selection.contract_fingerprint != 0);
    REQUIRE(
        selection.source_nodes ==
        std::vector<std::string>{"tone"});
    REQUIRE(
        selection.replacement_nodes ==
        std::vector<std::string>{"tone"});
    REQUIRE_FALSE(selection.explicitly_selected);

    auto frame_graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            resolved.config);
    applyResolvedTaggedSubgraphSelections(
        frame_graphs, resolved.graphs);
    const auto render_targets =
        parseRenderTargetDefinitionsFromJson(
            resolved.config);
    const auto logical =
        compileRenderingLogicalGraphs(
            frame_graphs, render_targets);
    REQUIRE(
        logical.front().subgraph_replacements ==
        resolved.graphs.front().selections);

    const auto target =
        compileRenderingTargetPlans(
            frame_graphs, render_targets,
            SampleCountPolicy{}, vk::Format::eB8G8R8A8Srgb,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<std::uint32_t>{1};
                    },
            });
    REQUIRE(target.plans.size() == 1);
    REQUIRE(
        target.plans.front()
            ->subgraph_replacements ==
        resolved.graphs.front().selections);
}

TEST_CASE(
    "WP201 custom provider expands a tagged region under the same typed boundary",
    "[render-subgraph][provider][contract][wp201]") {
    SubgraphReplacementRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.subgraph";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderSubgraph::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderSubgraph::Status::ok);
    registry.activateOwner(owner);

    ResolvedTaggedSubgraphConfig resolved;
    {
        const auto providers = registry.snapshot();
        resolved =
            resolveTaggedSubgraphReplacements(
                taggedRegionConfig(name),
                providers);
    }
    const auto &passes =
        graphObject(resolved.config).at("passes");
    REQUIRE(passes.size() == 4);
    REQUIRE(passes.at(0).at("name") == "pre");
    REQUIRE(passes.at(1).at("name") == "tone_a");
    REQUIRE(passes.at(2).at("name") == "tone_b");
    REQUIRE(passes.at(3).at("name") == "present");
    REQUIRE(
        passes.at(1).at("regions") ==
        nlohmann::json::array(
            {"region.post.fixture", "region.post"}));
    REQUIRE(
        passes.at(2).at("regions") ==
        passes.at(1).at("regions"));
    REQUIRE(
        passes.at(3).at("after") ==
        nlohmann::json::array(
            {"tone_a", "tone_b"}));

    REQUIRE(state.calls == 1);
    REQUIRE(state.port_count == 2);
    REQUIRE(state.fingerprint != 0);
    REQUIRE(state.graph == "main");
    REQUIRE(state.region == "region.post.fixture");
    const auto &selection =
        resolved.graphs.front().selections.front();
    REQUIRE(
        selection.provider ==
        std::string{name});
    REQUIRE(selection.provider_owner == owner);
    REQUIRE(
        selection.provider_identity ==
        handle.identity);
    REQUIRE(
        selection.provider_generation ==
        handle.generation);
    REQUIRE(selection.explicitly_selected);
    REQUIRE(
        selection.source_nodes ==
        std::vector<std::string>{"tone"});
    REQUIRE(
        selection.replacement_nodes ==
        std::vector<std::string>{
            "tone_a", "tone_b"});

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP201 boundary contract retains an externally materialized terminal output",
    "[render-subgraph][contract][external][wp201]") {
    auto config = taggedRegionConfig();
    auto &passes =
        config["rendering_passes"][0]["passes"];
    passes.at(1).erase("regions");
    passes.at(2)["regions"] =
        nlohmann::json::array(
            {"region.post.fixture"});

    const auto frame_graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    const auto render_targets =
        parseRenderTargetDefinitionsFromJson(config);
    const auto logical_graphs =
        compileRenderingLogicalGraphs(
            frame_graphs, render_targets);
    REQUIRE(logical_graphs.size() == 1);

    const auto contract =
        makeTaggedRegionContract(
            logical_graphs.front(),
            "region.post.fixture");
    REQUIRE(contract.boundary_ports.size() == 2);
    REQUIRE(
        contract.boundary_ports.at(0).resource ==
        "scene_out");
    REQUIRE(
        contract.boundary_ports.at(0).direction ==
        RenderSubgraph::BoundaryDirectionV1::input);
    REQUIRE(
        contract.boundary_ports.at(1).resource ==
        "swapchain");
    REQUIRE(
        contract.boundary_ports.at(1).direction ==
        RenderSubgraph::BoundaryDirectionV1::output);
    REQUIRE(
        contract.boundary_ports.at(1).materialization ==
        RenderSubgraph::MaterializationV1::external);
}

TEST_CASE(
    "WP201 invalid replacement is rejected without mutating authored configuration",
    "[render-subgraph][provider][transaction][wp201]") {
    SubgraphReplacementRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.invalid_subgraph";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderSubgraph::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderSubgraph::Status::ok);
    registry.activateOwner(owner);
    const auto authored = taggedRegionConfig(name);
    const auto original = authored;

    state.behavior =
        ProviderBehavior::change_boundary;
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveTaggedSubgraphReplacements(
                    authored, providers);
            },
            "changed tagged region boundary contract");
    }
    REQUIRE(authored == original);

    state.behavior =
        ProviderBehavior::malformed_json;
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveTaggedSubgraphReplacements(
                    authored, providers);
            },
            "returned malformed JSON");
    }
    REQUIRE(authored == original);

    state.behavior =
        ProviderBehavior::unavailable;
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveTaggedSubgraphReplacements(
                    authored, providers);
            },
            "status unavailable");
    }
    REQUIRE(authored == original);

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP201 v1 rejects non-contiguous tagged pass regions",
    "[render-subgraph][topology][wp201]") {
    auto config = taggedRegionConfig();
    auto &passes =
        config["rendering_passes"][0]["passes"];
    auto second = passes.at(1);
    second["name"] = "tone_second";
    second["input"] =
        nlohmann::json::array({"scratch"});
    second["output"]["color"] = "scene_out";
    passes.at(1)["output"]["color"] = "scratch";
    config["render_targets"].push_back({
        {"name", "unrelated_out"},
        {"extent_scale", 1.0},
        {"format", "R16G16B16A16_SFLOAT"},
        {"usage",
         nlohmann::json::array(
             {"COLOR_ATTACHMENT", "SAMPLED"})},
    });
    passes.insert(
        passes.begin() + 2,
        nlohmann::json{
            {"name", "unrelated"},
            {"type", "fullscreen"},
             {"input",
             nlohmann::json::array({"scratch"})},
            {"output",
             {{"color", "unrelated_out"},
              {"depth", nullptr}}},
            {"shader",
             {{"vertex", "engine://fullscreen"},
              {"fragment",
               "engine://scene_present"}}},
        });
    passes.insert(
        passes.begin() + 3, std::move(second));

    SubgraphReplacementRegistry registry;
    const auto providers = registry.snapshot();
    requireThrowsContaining(
        [&] {
            (void)resolveTaggedSubgraphReplacements(
                config, providers);
        },
        "requires contiguous authored passes");
}

TEST_CASE(
    "WP201 registry lifecycle rejects stale handles and snapshots lease providers",
    "[render-subgraph][provider][abi][lifetime][wp201]") {
    using namespace std::chrono_literals;

    SubgraphReplacementRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.lifecycle";
    const auto owner =
        internal::allocateRegistrationOwner();
    const auto other =
        internal::allocateRegistrationOwner();
    const auto declaration =
        provider(name, state);
    RenderSubgraph::ProviderHandleV1 first{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, first) ==
        RenderSubgraph::Status::ok);
    RenderSubgraph::ProviderHandleV1 duplicate{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, duplicate) ==
        RenderSubgraph::Status::duplicate_provider);
    REQUIRE(
        registry.unregisterProvider(first, other) ==
        RenderSubgraph::Status::wrong_owner);
    registry.activateOwner(owner);

    std::optional<
        SubgraphReplacementRegistrySnapshot>
        compile_snapshot;
    compile_snapshot.emplace(registry.snapshot());
    std::binary_semaphore release_started{0};
    std::binary_semaphore release_finished{0};
    std::thread release_thread{[&] {
        release_started.release();
        registry.releaseOwner(owner);
        release_finished.release();
    }};
    release_started.acquire();
    const bool released_while_snapshot_leased =
        release_finished.try_acquire_for(150ms);
    compile_snapshot.reset();
    release_thread.join();

    REQUIRE_FALSE(released_while_snapshot_leased);
    REQUIRE(
        registry.unregisterProvider(first, owner) ==
        RenderSubgraph::Status::stale_provider);
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveTaggedSubgraphReplacements(
                    taggedRegionConfig(name),
                    providers);
            },
            "not registered for the active owner");
    }
    internal::releaseRegistrationOwner(owner);
    registry.releaseOwner(other);
    internal::releaseRegistrationOwner(other);

    auto api =
        RenderSubgraph::descriptor<
            RenderSubgraph::ApiV1>();
    REQUIRE(
        RenderSubgraph::getApiV1(
            RenderSubgraph::abiVersionV1,
            &api) ==
        RenderSubgraph::Status::ok);
    REQUIRE(
        (api.capability_bits &
         RenderSubgraph::api_provider_registration) !=
        0);
    RenderSubgraph::ProviderHandleV1 api_handle{};
    REQUIRE(
        api.register_provider(
            api.context, &declaration,
            &api_handle) ==
        RenderSubgraph::Status::wrong_owner);
}

} // namespace Pelican
