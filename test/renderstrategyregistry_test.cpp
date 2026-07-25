#include "../src/core/renderingpass/renderstrategyregistry.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/project/renderpipeline.hpp"

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

nlohmann::json authoredRendererConfig(
    std::optional<std::string_view> provider =
        std::nullopt) {
    auto strategy = nlohmann::json{
        {"name", "fixture.whole_renderer"},
        {"parameters", {{"quality", "fixture"}}},
    };
    if (provider) {
        strategy["provider"] = *provider;
    }
    return nlohmann::json{
        {"resolver_version", 2},
        {"seed_marker", "authored"},
        {"render_strategy", strategy},
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

nlohmann::json generatedRendererConfig() {
    return nlohmann::json{
        {"resolver_version", 2},
        {"generated_marker", "strategy"},
        {"render_targets",
         nlohmann::json::array(
             {{{"name", "strategy_scene"},
               {"extent_scale", 1.0},
               {"format", "R16G16B16A16_SFLOAT"},
               {"usage",
                nlohmann::json::array(
                    {"COLOR_ATTACHMENT", "SAMPLED"})}}})},
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "strategy_main"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "strategy_source"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "strategy_scene"},
                        {"depth", nullptr}}}},
                     {{"name", "strategy_present"},
                      {"type", "fullscreen"},
                      {"input",
                       nlohmann::json::array(
                           {"strategy_scene"})},
                      {"after", "strategy_source"},
                      {"output",
                       {{"color", "swapchain"},
                        {"depth", nullptr}}}}})}}})},
    };
}

enum class ProviderBehavior {
    replace,
    malformed_json,
    non_object,
    reintroduce_control,
    reintroduce_pipeline_control,
    invalid_compiler_input,
    unavailable,
};

struct ProviderState {
    ProviderBehavior behavior =
        ProviderBehavior::replace;
    std::uint32_t calls = 0;
    std::uint64_t facade_capability_bits = 0;
    std::uint64_t seed_fingerprint = 0;
    RenderStrategy::GraphVariantV1 variant =
        RenderStrategy::GraphVariantV1::flat;
    RenderStrategy::HistoryPolicyV1 history =
        RenderStrategy::HistoryPolicyV1::preserve;
    RenderStrategy::ViewFamilyV1 view_family =
        RenderStrategy::ViewFamilyV1::caller_defined;
    RenderStrategy::ViewExecutionV1 view_execution =
        RenderStrategy::ViewExecutionV1::caller_defined;
    RenderStrategy::TerminalV1 terminal =
        RenderStrategy::TerminalV1::presentation;
    RenderStrategy::MirrorOutputV1 mirror_output =
        RenderStrategy::MirrorOutputV1::none;
    std::uint32_t view_count = 0;
    bool runtime_shader_compiler_enabled = false;
    std::string strategy;
    std::string parameters;
    std::string seed;
    std::string implementation =
        "fixture.render.strategy@1";
    std::string output;
};

bool equalsRange(
    const char *data, std::uint32_t size,
    std::string_view expected) {
    return data != nullptr &&
           std::string_view{data, size} == expected;
}

RenderStrategy::Status resolveStrategy(
    void *context,
    const RenderStrategy::ResolveRenderStrategyInputV1
        *input,
    RenderStrategy::RenderStrategyOutputV1
        *output) noexcept {
    if (context == nullptr || input == nullptr ||
        output == nullptr || input->contract == nullptr) {
        return RenderStrategy::Status::invalid_argument;
    }
    auto &state =
        *static_cast<ProviderState *>(context);
    ++state.calls;
    const auto &contract = *input->contract;
    const auto &policy =
        contract.graph_variant_policy;
    if (input->struct_size <
            sizeof(RenderStrategy::
                       ResolveRenderStrategyInputV1) ||
        input->version !=
            RenderStrategy::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->reserved3 != 0 ||
        input->reserved4 != 0 ||
        contract.struct_size <
            sizeof(RenderStrategy::
                       RendererFacadeContractV1) ||
        contract.version !=
            RenderStrategy::descriptorVersionV1 ||
        contract.reserved0 != 0 ||
        contract.reserved1 != 0 ||
        contract.reserved2 != 0 ||
        contract.reserved3 != 0 ||
        contract.reserved4 != 0 ||
        contract.reserved5 != 0 ||
        !equalsRange(
            contract.contract_id_utf8,
            contract.contract_id_size,
            RenderStrategy::rendererFacadeContractIdV1) ||
        !equalsRange(
            contract.output_contract_id_utf8,
            contract.output_contract_id_size,
            RenderStrategy::authoredConfigContractIdV1) ||
        contract.capability_bits !=
            RenderStrategy::builtinFacadeCapabilitiesV1 ||
        policy.struct_size <
            sizeof(RenderStrategy::GraphVariantPolicyV1) ||
        policy.version !=
            RenderStrategy::descriptorVersionV1 ||
        policy.reserved0 != 0 ||
        policy.reserved1 != 0 ||
        policy.reserved2 != 0 ||
        policy.reserved3 != 0 ||
        policy.reserved4 != 0 ||
        contract.runtime_shader_compiler_enabled > 1 ||
        input->strategy_name_utf8 == nullptr ||
        input->strategy_name_size == 0 ||
        input->parameters_json_utf8 == nullptr ||
        input->parameters_json_size == 0 ||
        input->seed_config_json_utf8 == nullptr ||
        input->seed_config_json_size == 0 ||
        input->seed_config_fingerprint == 0 ||
        output->struct_size <
            sizeof(RenderStrategy::RenderStrategyOutputV1) ||
        output->version !=
            RenderStrategy::descriptorVersionV1) {
        return RenderStrategy::Status::invalid_argument;
    }

    state.facade_capability_bits =
        contract.capability_bits;
    state.seed_fingerprint =
        input->seed_config_fingerprint;
    state.variant = policy.variant;
    state.history = policy.history;
    state.view_family = policy.view_family;
    state.view_execution = policy.view_execution;
    state.terminal = policy.terminal;
    state.mirror_output = policy.mirror_output;
    state.view_count = policy.view_count;
    state.runtime_shader_compiler_enabled =
        contract.runtime_shader_compiler_enabled != 0;
    state.strategy.assign(
        input->strategy_name_utf8,
        input->strategy_name_size);
    state.parameters.assign(
        input->parameters_json_utf8,
        input->parameters_json_size);
    state.seed.assign(
        input->seed_config_json_utf8,
        input->seed_config_json_size);

    if (state.behavior ==
        ProviderBehavior::unavailable) {
        return RenderStrategy::Status::unavailable;
    }
    if (state.behavior ==
        ProviderBehavior::malformed_json) {
        state.output = "{";
    } else if (
        state.behavior == ProviderBehavior::non_object) {
        state.output = "[]";
    } else if (
        state.behavior ==
        ProviderBehavior::reintroduce_control) {
        auto generated = generatedRendererConfig();
        generated["render_strategy"] =
            nlohmann::json::object();
        state.output =
            nlohmann::ordered_json(generated).dump();
    } else if (
        state.behavior ==
        ProviderBehavior::
            reintroduce_pipeline_control) {
        auto generated = generatedRendererConfig();
        generated["pipeline"] = {
            {"preset", "fixture://recursive"}};
        state.output =
            nlohmann::ordered_json(generated).dump();
    } else if (
        state.behavior ==
        ProviderBehavior::invalid_compiler_input) {
        state.output =
            R"json({"render_targets":"invalid"})json";
    } else {
        state.output =
            nlohmann::ordered_json(
                generatedRendererConfig())
                .dump();
    }

    *output =
        RenderStrategy::descriptor<
            RenderStrategy::RenderStrategyOutputV1>();
    output->implementation_id_utf8 =
        state.implementation.data();
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            state.implementation.size());
    output->config_json_utf8 = state.output.data();
    output->config_json_size =
        static_cast<std::uint32_t>(
            state.output.size());
    return RenderStrategy::Status::ok;
}

RenderStrategy::ProviderV1 provider(
    std::string_view name, ProviderState &state) {
    auto result =
        RenderStrategy::descriptor<
            RenderStrategy::ProviderV1>();
    result.capability_bits =
        RenderStrategy::builtinProviderCapabilitiesV1;
    result.name_utf8 = name.data();
    result.name_size =
        static_cast<std::uint32_t>(name.size());
    result.context = &state;
    result.resolve_render_strategy =
        resolveStrategy;
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
    "WP202b builtin render strategy preserves authored config through the typed facade",
    "[render-strategy][provider][wp202b]") {
    RenderStrategyRegistry registry;
    const auto authored = authoredRendererConfig();
    const auto policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::flat});
    ResolvedRenderStrategyConfig resolved;
    {
        const auto providers = registry.snapshot();
        resolved = resolveRenderStrategy(
            authored, policy, true, providers);
    }

    auto expected = authored;
    expected.erase("render_strategy");
    REQUIRE(resolved.config == expected);
    REQUIRE(resolved.selection.has_value());
    REQUIRE(
        resolved.selection->provider ==
        std::string{
            builtinAuthoredRenderStrategyProvider});
    REQUIRE(
        resolved.selection->implementation ==
        "pelican.render.strategy.authored_config@1");
    REQUIRE(
        resolved.selection->graph_variant == "flat");
    REQUIRE_FALSE(
        resolved.selection->explicitly_selected);
    REQUIRE(
        resolved.selection->input_config_fingerprint ==
        resolved.selection->output_config_fingerprint);

    auto controlled = authored;
    controlled["target_planning"] = {
        {"profile", {{"kind", "optimized"}}},
    };
    controlled["vulkan_plan_pins"] = {
        {"flat", nlohmann::json::array()},
    };
    ResolvedRenderStrategyConfig with_controls;
    {
        const auto providers = registry.snapshot();
        with_controls = resolveRenderStrategy(
            controlled, policy, true, providers);
    }
    auto expected_with_controls = controlled;
    expected_with_controls.erase("render_strategy");
    REQUIRE(with_controls.config ==
            expected_with_controls);
    REQUIRE(
        with_controls.selection->input_config_fingerprint ==
        resolved.selection->input_config_fingerprint);
    REQUIRE(
        with_controls.selection->output_config_fingerprint ==
        resolved.selection->output_config_fingerprint);
    const auto logical_fingerprint =
        [](const ResolvedRenderStrategyConfig &strategy) {
            auto graphs =
                parseFrameGraphDefinitionsFromConfigJson(
                    strategy.config);
            applyResolvedRenderStrategySelection(
                graphs, strategy.selection);
            const auto targets =
                parseRenderTargetDefinitionsFromJson(
                    strategy.config);
            const auto logical =
                compileRenderingLogicalGraphs(
                    graphs, targets);
            REQUIRE(logical.size() == 1);
            return vulkanTargetPlanLogicalGraphFingerprint(
                logical.front());
        };
    REQUIRE(
        logical_fingerprint(with_controls) ==
        logical_fingerprint(resolved));

    auto no_strategy = expected;
    {
        const auto providers = registry.snapshot();
        const auto bypass = resolveRenderStrategy(
            no_strategy, policy, true, providers);
        REQUIRE(bypass.config == no_strategy);
        REQUIRE_FALSE(bypass.selection.has_value());
    }
}

TEST_CASE(
    "WP202b strategy can replace the whole renderer seed and observes the XR facade",
    "[render-strategy][provider][xr][wp202b]") {
    RenderStrategyRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.render_strategy";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderStrategy::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderStrategy::Status::ok);
    registry.activateOwner(owner);
    const auto policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{
                RenderPipelineGraphVariant::xr});

    ResolvedRenderStrategyConfig resolved;
    {
        const auto providers = registry.snapshot();
        resolved = resolveRenderStrategy(
            authoredRendererConfig(name),
            policy, true, providers);
    }
    REQUIRE(state.calls == 1);
    REQUIRE(
        state.facade_capability_bits ==
        RenderStrategy::builtinFacadeCapabilitiesV1);
    REQUIRE(state.seed_fingerprint != 0);
    REQUIRE(
        state.variant ==
        RenderStrategy::GraphVariantV1::xr);
    REQUIRE(
        state.history ==
        RenderStrategy::HistoryPolicyV1::forbid);
    REQUIRE(
        state.view_family ==
        RenderStrategy::ViewFamilyV1::stereo);
    REQUIRE(
        state.view_execution ==
        RenderStrategy::ViewExecutionV1::sequential);
    REQUIRE(
        state.terminal ==
        RenderStrategy::TerminalV1::external_view);
    REQUIRE(
        state.mirror_output ==
        RenderStrategy::MirrorOutputV1::left_eye);
    REQUIRE(state.view_count == 2);
    REQUIRE(state.runtime_shader_compiler_enabled);
    REQUIRE(
        state.strategy == "fixture.whole_renderer");
    REQUIRE(
        nlohmann::json::parse(state.parameters)
            .at("quality") == "fixture");
    REQUIRE_FALSE(
        nlohmann::json::parse(state.seed)
            .contains("render_strategy"));

    REQUIRE(
        resolved.config.at("generated_marker") ==
        "strategy");
    REQUIRE_FALSE(resolved.config.contains("seed_marker"));
    REQUIRE(resolved.selection.has_value());
    REQUIRE(
        resolved.selection->provider ==
        std::string{name});
    REQUIRE(
        resolved.selection->provider_owner == owner);
    REQUIRE(
        resolved.selection->provider_identity ==
        handle.identity);
    REQUIRE(
        resolved.selection->provider_generation ==
        handle.generation);
    REQUIRE(
        resolved.selection->graph_variant == "xr");
    REQUIRE(
        resolved.selection->input_config_fingerprint !=
        resolved.selection->output_config_fingerprint);
    REQUIRE(resolved.selection->explicitly_selected);

    auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            resolved.config);
    applyResolvedRenderStrategySelection(
        graphs, resolved.selection);
    const auto targets =
        parseRenderTargetDefinitionsFromJson(
            resolved.config);
    const auto logical =
        compileRenderingLogicalGraphs(graphs, targets);
    REQUIRE(logical.size() == 1);
    REQUIRE(
        logical.front().render_strategy ==
        resolved.selection);

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP202b strategy executes after preset expansion and keeps preset provenance",
    "[render-strategy][preset][pipeline][wp202b]") {
    RenderStrategyRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.preset_render_strategy";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderStrategy::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderStrategy::Status::ok);
    registry.activateOwner(owner);

    auto preset_config = authoredRendererConfig(name);
    const auto preset_document = nlohmann::json{
        {"schema", "pelican.render_pipeline"},
        {"version", 1},
        {"name", "fixture_strategy"},
        {"config", preset_config},
    };
    const auto authored = nlohmann::json{
        {"pipeline",
         {{"preset", "fixture://strategy"}}},
    };

    {
        const auto providers = registry.snapshot();
        std::optional<RenderStrategySelection> selection;
        const auto resolved = resolveRenderPipeline(
            RenderPipelineRequest{authored, "strategy preset"},
            RenderEnvironmentCapabilities{
                .runtime_shader_compiler_enabled = true,
            },
            RenderPipelineResolveDependencies{
                .load_pipeline_json =
                    [&](std::string_view reference) {
                        REQUIRE(
                            std::string{reference} ==
                            "fixture://strategy");
                        return preset_document.dump();
                    },
                .resolve_render_strategy =
                    [&](const nlohmann::json &config,
                        const CompiledGraphVariantPolicy
                            &policy) {
                        auto generated =
                            resolveRenderStrategy(
                                config, policy, true,
                                providers);
                        selection = generated.selection;
                        return generated.config;
                    },
            });
        REQUIRE(selection.has_value());
        REQUIRE(resolved.pipeline_preset.has_value());
        REQUIRE(
            resolved.pipeline_preset->reference ==
            "fixture://strategy");
        REQUIRE_FALSE(
            resolved.normalized_config.contains(
                "render_strategy"));
        REQUIRE(
            resolved.normalized_config.at(
                "generated_marker") == "strategy");
        const auto strategy_seed =
            nlohmann::json::parse(state.seed);
        REQUIRE(
            strategy_seed.at("seed_marker") ==
            "authored");
        REQUIRE_FALSE(
            strategy_seed.contains("pipeline"));

        auto override_config = authored;
        override_config["render_strategy"] = {
            {"name", "fixture.override"}};
        requireThrowsContaining(
            [&] {
                (void)resolveRenderPipeline(
                    RenderPipelineRequest{
                        override_config,
                        "strategy preset override"},
                    RenderEnvironmentCapabilities{
                        .runtime_shader_compiler_enabled =
                            true,
                    },
                    RenderPipelineResolveDependencies{
                        .load_pipeline_json =
                            [&](std::string_view) {
                                return preset_document.dump();
                            },
                    });
            },
            "cannot override preset render_strategy");
    }

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP202b rejects malformed strategy candidates without mutating the seed",
    "[render-strategy][transaction][wp202b]") {
    RenderStrategyRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.invalid_render_strategy";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderStrategy::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderStrategy::Status::ok);
    registry.activateOwner(owner);
    const auto authored =
        authoredRendererConfig(name);
    const auto original = authored;
    const auto policy =
        compileGraphVariantPolicy(
            GraphVariantPolicyRequest{});

    const auto check =
        [&](ProviderBehavior behavior,
            std::string_view expected) {
            state.behavior = behavior;
            const auto providers = registry.snapshot();
            requireThrowsContaining(
                [&] {
                    (void)resolveRenderStrategy(
                        authored, policy, true,
                        providers);
                },
                expected);
            REQUIRE(authored == original);
        };
    check(
        ProviderBehavior::malformed_json,
        "returned malformed JSON");
    check(
        ProviderBehavior::non_object,
        "returned a non-object config");
    check(
        ProviderBehavior::reintroduce_control,
        "reintroduced reserved render_strategy");
    check(
        ProviderBehavior::reintroduce_pipeline_control,
        "reintroduced reserved pipeline preset");
    check(
        ProviderBehavior::unavailable,
        "status unavailable");

    state.behavior =
        ProviderBehavior::invalid_compiler_input;
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveRenderPipeline(
                    RenderPipelineRequest{
                        authored, "invalid strategy"},
                    RenderEnvironmentCapabilities{
                        .runtime_shader_compiler_enabled =
                            true,
                    },
                    RenderPipelineResolveDependencies{
                        .resolve_render_strategy =
                            [&](const nlohmann::json
                                    &config,
                                const CompiledGraphVariantPolicy
                                    &compiled_policy) {
                                return resolveRenderStrategy(
                                           config,
                                           compiled_policy,
                                           true, providers)
                                    .config;
                            },
                    });
            },
            "render_targets");
    }
    REQUIRE(authored == original);

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP202b render strategy registry leases provider generations",
    "[render-strategy][abi][lifetime][wp202b]") {
    using namespace std::chrono_literals;

    RenderStrategyRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.lifecycle";
    const auto owner =
        internal::allocateRegistrationOwner();
    const auto other =
        internal::allocateRegistrationOwner();
    const auto declaration =
        provider(name, state);
    RenderStrategy::ProviderHandleV1 first{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, first) ==
        RenderStrategy::Status::ok);
    RenderStrategy::ProviderHandleV1 duplicate{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, duplicate) ==
        RenderStrategy::Status::duplicate_provider);
    REQUIRE(
        registry.unregisterProvider(first, other) ==
        RenderStrategy::Status::wrong_owner);
    registry.activateOwner(owner);

    std::optional<RenderStrategyRegistrySnapshot>
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
        RenderStrategy::Status::stale_provider);
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                (void)resolveRenderStrategy(
                    authoredRendererConfig(name),
                    compileGraphVariantPolicy(
                        GraphVariantPolicyRequest{}),
                    true, providers);
            },
            "not registered for the active owner");
    }
    internal::releaseRegistrationOwner(owner);
    registry.releaseOwner(other);
    internal::releaseRegistrationOwner(other);

    auto api =
        RenderStrategy::descriptor<
            RenderStrategy::ApiV1>();
    REQUIRE(
        RenderStrategy::getApiV1(
            RenderStrategy::abiVersionV1,
            &api) == RenderStrategy::Status::ok);
    REQUIRE(
        (api.capability_bits &
         RenderStrategy::api_provider_registration) !=
        0);
    RenderStrategy::ProviderHandleV1 api_handle{};
    REQUIRE(
        api.register_provider(
            api.context, &declaration,
            &api_handle) ==
        RenderStrategy::Status::wrong_owner);
}

} // namespace Pelican
