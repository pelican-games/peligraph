#include "../src/core/renderingpass/graphtransformregistry.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"

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

nlohmann::json graphTransformConfig(
    std::optional<std::string_view> provider =
        std::nullopt) {
    auto request = nlohmann::json{
        {"name", "fixture.global_transform"},
        {"parameters",
         {{"quality", "fixture"}}},
    };
    if (provider) {
        request["provider"] = *provider;
    }
    return nlohmann::json{
        {"resolver_version", 2},
        {"graph_transforms",
         nlohmann::json::array({request})},
        {"render_targets",
         nlohmann::json::array(
             {{{"name", "scene"},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"usage",
                nlohmann::json::array(
                    {"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "main"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "scene_source"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "scene"},
                        {"depth", nullptr}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"input",
                       nlohmann::json::array(
                           {"scene"})},
                      {"after", "scene_source"},
                      {"output",
                       {{"color", "swapchain"},
                        {"depth", nullptr}}}}})}}})},
    };
}

enum class ProviderBehavior {
    expand,
    change_boundary,
    change_control,
    add_history,
    append_after_terminal,
    malformed_json,
    reintroduce_control,
    unavailable,
};

struct ProviderState {
    ProviderBehavior behavior =
        ProviderBehavior::expand;
    std::uint32_t calls = 0;
    std::uint32_t graph_count = 0;
    std::uint32_t port_count = 0;
    std::uint64_t boundary_fingerprint = 0;
    std::uint64_t graph_fingerprint = 0;
    std::uint32_t transform_index = 0;
    std::string transform;
    std::string parameters;
    std::string implementation =
        "fixture.render.graph_transform@1";
    std::string transformed_config;
};

RenderGraphTransform::Status resolveTransform(
    void *context,
    const RenderGraphTransform::
        ResolveGraphTransformInputV1 *input,
    RenderGraphTransform::GraphTransformOutputV1
        *output) noexcept {
    if (context == nullptr || input == nullptr ||
        output == nullptr || input->contract == nullptr) {
        return RenderGraphTransform::Status::
            invalid_argument;
    }
    auto &state =
        *static_cast<ProviderState *>(context);
    ++state.calls;
    const auto &contract = *input->contract;
    if (input->struct_size <
            sizeof(RenderGraphTransform::
                       ResolveGraphTransformInputV1) ||
        input->version !=
            RenderGraphTransform::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->reserved3 != 0 ||
        input->reserved4 != 0 ||
        contract.struct_size <
            sizeof(RenderGraphTransform::
                       GraphSetContractV1) ||
        contract.version !=
            RenderGraphTransform::descriptorVersionV1 ||
        contract.reserved0 != 0 ||
        contract.reserved1 != 0 ||
        contract.reserved2 != 0 ||
        contract.contract_id_utf8 == nullptr ||
        contract.contract_id_size !=
            sizeof(RenderGraphTransform::
                       graphSetContractIdV1) -
                1 ||
        contract.graph_count == 0 ||
        (contract.boundary_port_count != 0 &&
         contract.boundary_ports == nullptr) ||
        input->transform_name_utf8 == nullptr ||
        input->transform_name_size == 0 ||
        input->parameters_json_utf8 == nullptr ||
        input->parameters_json_size == 0 ||
        input->config_json_utf8 == nullptr ||
        input->config_json_size == 0 ||
        input->logical_graphs_json_utf8 == nullptr ||
        input->logical_graphs_json_size == 0 ||
        output->struct_size <
            sizeof(RenderGraphTransform::
                       GraphTransformOutputV1) ||
        output->version !=
            RenderGraphTransform::descriptorVersionV1) {
        return RenderGraphTransform::Status::
            invalid_argument;
    }
    for (std::uint32_t index = 0;
         index < contract.boundary_port_count;
         ++index) {
        const auto &port =
            contract.boundary_ports[index];
        if (port.struct_size <
                sizeof(RenderGraphTransform::
                           BoundaryPortV1) ||
            port.version !=
                RenderGraphTransform::
                    descriptorVersionV1 ||
            port.reserved0 != 0 ||
            port.reserved1 != 0 ||
            port.reserved2 != 0 ||
            port.reserved3 != 0 ||
            port.reserved4 != 0 ||
            port.reserved5 != 0 ||
            port.graph_name_utf8 == nullptr ||
            port.graph_name_size == 0 ||
            port.resource_utf8 == nullptr ||
            port.resource_size == 0 ||
            port.type_json_utf8 == nullptr ||
            port.type_json_size == 0) {
            return RenderGraphTransform::Status::
                invalid_argument;
        }
    }
    state.graph_count = contract.graph_count;
    state.port_count =
        contract.boundary_port_count;
    state.boundary_fingerprint =
        contract.boundary_fingerprint;
    state.graph_fingerprint =
        input->logical_graphs_fingerprint;
    state.transform_index =
        input->transform_index;
    state.transform.assign(
        input->transform_name_utf8,
        input->transform_name_size);
    state.parameters.assign(
        input->parameters_json_utf8,
        input->parameters_json_size);

    if (state.behavior ==
        ProviderBehavior::unavailable) {
        return RenderGraphTransform::Status::unavailable;
    }
    if (state.behavior ==
        ProviderBehavior::malformed_json) {
        state.transformed_config = "{";
    } else {
        try {
            auto config = nlohmann::json::parse(
                input->config_json_utf8,
                input->config_json_utf8 +
                    input->config_json_size);
            if (state.behavior ==
                ProviderBehavior::expand) {
                config["render_targets"].push_back({
                    {"name", "transform_scratch"},
                    {"extent_scale", 1.0},
                    {"format",
                     "R16G16B16A16_SFLOAT"},
                    {"usage",
                     nlohmann::json::array(
                         {"COLOR_ATTACHMENT",
                          "SAMPLED"})},
                });
                auto &passes =
                    config["rendering_passes"][0]
                          ["passes"];
                passes.at(1)["input"] =
                    nlohmann::json::array(
                        {"transform_scratch"});
                passes.at(1)["after"] =
                    "fixture_transform_pass";
                passes.insert(
                    passes.begin() + 1,
                    nlohmann::json{
                        {"name",
                         "fixture_transform_pass"},
                        {"type", "fullscreen"},
                        {"input",
                         nlohmann::json::array(
                             {"scene"})},
                        {"after", "scene_source"},
                        {"output",
                         {{"color",
                           "transform_scratch"},
                          {"depth", nullptr}}},
                    });
            } else if (
                state.behavior ==
                ProviderBehavior::change_boundary) {
                auto &present =
                    config["rendering_passes"][0]
                          ["passes"][1];
                present["output"]["color"] =
                    "scene";
                // Keep the candidate shape-valid so the protected-boundary
                // rejection remains the tested failure rather than current
                // frame feedback.
                present.erase("input");
            } else if (
                state.behavior ==
                ProviderBehavior::change_control) {
                config["draw_sort"] = {
                    {"opaque",
                     {{"provider",
                       "fixture.changed"}}},
                };
            } else if (
                state.behavior ==
                ProviderBehavior::add_history) {
                config["render_targets"].push_back({
                    {"name", "transform_history"},
                    {"extent_scale", 1.0},
                    {"format",
                     "R16G16B16A16_SFLOAT"},
                    {"history", true},
                    {"usage",
                     nlohmann::json::array(
                         {"COLOR_ATTACHMENT",
                          "SAMPLED"})},
                });
                config["rendering_passes"][0]
                      ["passes"][1]["input"]
                          .push_back(
                              "transform_history@history");
            } else if (
                state.behavior ==
                ProviderBehavior::
                    append_after_terminal) {
                config["rendering_passes"][0]
                      ["passes"]
                          .push_back({
                              {"name", "after_terminal"},
                              {"type", "fullscreen"},
                              {"output",
                               {{"color", "swapchain"},
                                {"depth", nullptr}}},
                          });
            } else if (
                state.behavior ==
                ProviderBehavior::
                    reintroduce_control) {
                config["graph_transforms"] =
                    nlohmann::json::array();
            }
            state.transformed_config =
                nlohmann::ordered_json(config).dump();
        } catch (...) {
            return RenderGraphTransform::Status::
                provider_error;
        }
    }

    *output =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::
                GraphTransformOutputV1>();
    output->implementation_id_utf8 =
        state.implementation.data();
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            state.implementation.size());
    output->config_json_utf8 =
        state.transformed_config.data();
    output->config_json_size =
        static_cast<std::uint32_t>(
            state.transformed_config.size());
    return RenderGraphTransform::Status::ok;
}

RenderGraphTransform::ProviderV1 provider(
    std::string_view name, ProviderState &state) {
    auto result =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::ProviderV1>();
    result.capability_bits =
        RenderGraphTransform::
            builtinProviderCapabilitiesV1;
    result.name_utf8 = name.data();
    result.name_size =
        static_cast<std::uint32_t>(name.size());
    result.context = &state;
    result.resolve_graph_transform =
        resolveTransform;
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

} // namespace

TEST_CASE(
    "WP202a builtin graph transform is a typed identity",
    "[render-graph-transform][provider][wp202a]") {
    GraphTransformRegistry registry;
    const auto authored =
        graphTransformConfig();
    ResolvedLogicalGraphTransformConfig resolved;
    {
        const auto providers = registry.snapshot();
        resolved = resolveLogicalGraphTransforms(
            authored, providers);
    }

    auto expected = authored;
    expected.erase("graph_transforms");
    REQUIRE(resolved.config == expected);
    REQUIRE(resolved.selections.size() == 1);
    const auto &selection =
        resolved.selections.front();
    REQUIRE(
        selection.provider ==
        std::string{
            builtinLogicalGraphTransformProvider});
    REQUIRE(
        selection.implementation ==
        "pelican.render.graph_transform.identity@1");
    REQUIRE_FALSE(selection.explicitly_selected);
    REQUIRE(
        selection.input_graph_fingerprint ==
        selection.output_graph_fingerprint);
    REQUIRE(selection.boundary_fingerprint != 0);

    auto frame_graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            resolved.config);
    applyResolvedLogicalGraphTransformSelections(
        frame_graphs, resolved.selections);
    const auto render_targets =
        parseRenderTargetDefinitionsFromJson(
            resolved.config);
    const auto logical_graphs =
        compileRenderingLogicalGraphs(
            frame_graphs, render_targets);
    REQUIRE(logical_graphs.size() == 1);
    REQUIRE(
        logical_graphs.front().graph_transforms ==
        resolved.selections);
}

TEST_CASE(
    "WP202a provider can expand internal graph structure while preserving its boundary",
    "[render-graph-transform][provider][boundary][wp202a]") {
    GraphTransformRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.graph_transform";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderGraphTransform::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderGraphTransform::Status::ok);
    registry.activateOwner(owner);

    ResolvedLogicalGraphTransformConfig resolved;
    {
        const auto providers = registry.snapshot();
        resolved = resolveLogicalGraphTransforms(
            graphTransformConfig(name),
            providers);
    }
    REQUIRE(state.calls == 1);
    REQUIRE(state.graph_count == 1);
    REQUIRE(state.port_count != 0);
    REQUIRE(state.boundary_fingerprint != 0);
    REQUIRE(state.graph_fingerprint != 0);
    REQUIRE(state.transform_index == 0);
    REQUIRE(
        state.transform ==
        "fixture.global_transform");
    REQUIRE(
        nlohmann::json::parse(state.parameters)
            .at("quality") == "fixture");
    REQUIRE(
        resolved.config.at("render_targets").size() ==
        2);
    REQUIRE(
        resolved.config.at("rendering_passes")
            .at(0)
            .at("passes")
            .size() == 3);
    REQUIRE(resolved.selections.size() == 1);
    REQUIRE(
        resolved.selections.front().provider ==
        std::string{name});
    REQUIRE(
        resolved.selections.front()
            .input_graph_fingerprint !=
        resolved.selections.front()
            .output_graph_fingerprint);
    REQUIRE(
        resolved.selections.front()
            .provider_owner == owner);
    REQUIRE(
        resolved.selections.front()
            .provider_identity ==
        handle.identity);
    REQUIRE(
        resolved.selections.front()
            .provider_generation ==
        handle.generation);
    REQUIRE(
        resolved.selections.front()
            .explicitly_selected);

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP202a graph transforms form an ordered finite chain and survive preset expansion",
    "[render-graph-transform][chain][preset][wp202a]") {
    auto preset_config = graphTransformConfig();
    preset_config.erase("graph_transforms");
    const auto preset_document = nlohmann::json{
        {"schema", "pelican.render_pipeline"},
        {"version", 1},
        {"name", "fixture"},
        {"config", preset_config},
    };
    const auto authored = nlohmann::json{
        {"pipeline",
         {{"preset", "fixture://pipeline"}}},
        {"graph_transforms",
         nlohmann::json::array(
             {{{"name", "fixture.first"}},
              {{"name", "fixture.second"}}})},
    };
    const auto expanded =
        resolveRenderPipelinePreset(
            authored,
            [&](std::string_view reference) {
                REQUIRE(
                    std::string{reference} ==
                    "fixture://pipeline");
                return preset_document.dump();
            });
    REQUIRE(
        expanded.config.at("graph_transforms")
            .size() == 2);

    GraphTransformRegistry registry;
    const auto providers = registry.snapshot();
    const auto resolved =
        resolveLogicalGraphTransforms(
            expanded.config, providers);
    REQUIRE(resolved.selections.size() == 2);
    REQUIRE(
        resolved.selections.at(0).transform_index ==
        0);
    REQUIRE(
        resolved.selections.at(1).transform_index ==
        1);
    REQUIRE(
        resolved.selections.at(0)
            .output_graph_fingerprint ==
        resolved.selections.at(1)
            .input_graph_fingerprint);
    REQUIRE_FALSE(
        resolved.config.contains(
            "graph_transforms"));

    auto duplicate = expanded.config;
    duplicate["graph_transforms"][1]["name"] =
        "fixture.first";
    requireThrowsContaining(
        [&] {
            (void)resolveLogicalGraphTransforms(
                duplicate, providers);
        },
        "names must be unique");
}

TEST_CASE(
    "WP202a rejects invalid candidates without mutating authored config",
    "[render-graph-transform][transaction][wp202a]") {
    GraphTransformRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.invalid_graph_transform";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderGraphTransform::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderGraphTransform::Status::ok);
    registry.activateOwner(owner);
    const auto authored =
        graphTransformConfig(name);
    const auto original = authored;

    const auto check =
        [&](ProviderBehavior behavior,
            std::string_view expected) {
            state.behavior = behavior;
            const auto providers =
                registry.snapshot();
            requireThrowsContaining(
                [&] {
                    (void)resolveLogicalGraphTransforms(
                        authored, providers);
                },
                expected);
            REQUIRE(authored == original);
        };
    check(
        ProviderBehavior::change_boundary,
        "protected boundary");
    check(
        ProviderBehavior::change_control,
        "policy or control fields");
    check(
        ProviderBehavior::add_history,
        "introduced a new external or history");
    check(
        ProviderBehavior::malformed_json,
        "returned malformed JSON");
    check(
        ProviderBehavior::reintroduce_control,
        "reintroduced reserved graph_transforms");
    check(
        ProviderBehavior::unavailable,
        "status unavailable");

    auto terminal = authored;
    terminal["rendering_passes"][0]["passes"][1]
            ["type"] = "output_transform";
    state.behavior =
        ProviderBehavior::append_after_terminal;
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveLogicalGraphTransforms(
                    terminal, providers);
            },
            "non-terminal or duplicate output_transform");
    }

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP202a registry lifecycle leases provider generations",
    "[render-graph-transform][abi][lifetime][wp202a]") {
    using namespace std::chrono_literals;

    GraphTransformRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.lifecycle";
    const auto owner =
        internal::allocateRegistrationOwner();
    const auto other =
        internal::allocateRegistrationOwner();
    const auto declaration =
        provider(name, state);
    RenderGraphTransform::ProviderHandleV1 first{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, first) ==
        RenderGraphTransform::Status::ok);
    RenderGraphTransform::ProviderHandleV1 duplicate{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, duplicate) ==
        RenderGraphTransform::Status::
            duplicate_provider);
    REQUIRE(
        registry.unregisterProvider(first, other) ==
        RenderGraphTransform::Status::wrong_owner);
    registry.activateOwner(owner);

    std::optional<GraphTransformRegistrySnapshot>
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
        RenderGraphTransform::Status::
            stale_provider);
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveLogicalGraphTransforms(
                    graphTransformConfig(name),
                    providers);
            },
            "not registered for the active owner");
    }
    internal::releaseRegistrationOwner(owner);
    registry.releaseOwner(other);
    internal::releaseRegistrationOwner(other);

    auto api =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::ApiV1>();
    REQUIRE(
        RenderGraphTransform::getApiV1(
            RenderGraphTransform::abiVersionV1,
            &api) ==
        RenderGraphTransform::Status::ok);
    REQUIRE(
        (api.capability_bits &
         RenderGraphTransform::
             api_provider_registration) != 0);
    RenderGraphTransform::ProviderHandleV1
        api_handle{};
    REQUIRE(
        api.register_provider(
            api.context, &declaration,
            &api_handle) ==
        RenderGraphTransform::Status::wrong_owner);
}

} // namespace Pelican
