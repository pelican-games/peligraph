#include "../src/core/renderingpass/passimplementationregistry.hpp"
#include "../src/project/targetrenderplanning.hpp"

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

LogicalGraphNode fullscreenNode(
    std::string name = "composite",
    LogicalGraphNodeKind kind =
        LogicalGraphNodeKind::render) {
    const auto types = makeBuiltinLogicalTypeRegistry();
    const auto scene = sceneLinearHdrV1(types);
    LogicalGraphNode node;
    node.name = std::move(name);
    node.kind = kind;
    node.ports = {
        LogicalPortContract{
            "in.scene", LogicalPortDirection::input,
            exactLogicalTypePattern(types, scene), {}},
        LogicalPortContract{
            "out.scene", LogicalPortDirection::output,
            exactLogicalTypePattern(types, scene),
            {LogicalPortRelation{
                LogicalPortRelationKind::same_extent,
                "in.scene"}}},
    };
    node.uses = {
        makeLogicalReadUse(
            "in.scene", LogicalValueId{"scene_in", 0},
            LogicalReadFootprint{
                LogicalReadFootprintKind::same_pixel,
                std::nullopt},
            LogicalAccessIntent::sampled),
        makeLogicalWriteUse(
            "out.scene",
            LogicalValueId{"scene_out", 1},
            LogicalAccessIntent::attachment),
    };
    return node;
}

PassDefinition fullscreenPass(
    std::string name = "composite") {
    PassDefinition pass;
    pass.name = std::move(name);
    pass.pass_info = FullscreenPassInfo{
        .vert_shader =
            makeShaderReference(
                "engine://fullscreen",
                ShaderStage::vertex),
        .frag_shader =
            makeShaderReference(
                "engine://scene_present",
                ShaderStage::fragment),
        .push_constants =
            FullscreenPushConstantData::eProjectionView,
        .uses_light_data = true,
    };
    return pass;
}

VulkanTargetPlan targetPlan(
    const LogicalGraphNode &node) {
    VulkanTargetPlan plan;
    plan.graph = "main";
    plan.lowering_graph.name = "main";
    plan.lowering_graph.nodes.push_back(
        TargetLoweringNode{.logical = node});
    return plan;
}

enum class ProviderBehavior {
    replace,
    unavailable,
    unavailable_after_first,
    invalid_output,
};

struct ProviderState {
    ProviderBehavior behavior =
        ProviderBehavior::replace;
    std::uint32_t calls = 0;
    std::uint32_t port_count = 0;
    std::uint32_t interface_flags = 0;
    std::uint64_t fingerprint = 0;
    RenderPass::PortDirectionV1 first_direction =
        RenderPass::PortDirectionV1::input;
    RenderPass::AccessModeV1 first_access =
        RenderPass::AccessModeV1::read;
    RenderPass::AccessIntentV1 first_intent =
        RenderPass::AccessIntentV1::automatic;
    RenderPass::ReadFootprintV1 first_footprint =
        RenderPass::ReadFootprintV1::none;
    std::uint32_t first_has_footprint_radius = 0;
    std::uint32_t first_footprint_radius = 0;
    std::string first_type_pattern_json;
    std::string first_relations_json;
    std::string implementation =
        "fixture.render.composite@1";
    std::string vertex =
        "shaders/fixture_fullscreen";
    std::string fragment =
        "shaders/fixture_composite";
};

RenderPass::Status resolveFullscreen(
    void *context,
    const RenderPass::ResolveFullscreenInputV1 *input,
    RenderPass::FullscreenImplementationV1 *output) noexcept {
    if (context == nullptr || input == nullptr ||
        output == nullptr ||
        input->struct_size <
            sizeof(RenderPass::ResolveFullscreenInputV1) ||
        input->version !=
            RenderPass::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->contract == nullptr ||
        input->authored_implementation == nullptr) {
        return RenderPass::Status::invalid_argument;
    }
    auto &state =
        *static_cast<ProviderState *>(context);
    ++state.calls;
    state.port_count = input->contract->port_count;
    state.interface_flags =
        input->contract->interface_flags;
    state.fingerprint =
        input->contract->fingerprint;
    if (input->contract->port_count != 0) {
        const auto &first =
            input->contract->ports[0];
        state.first_direction = first.direction;
        state.first_access = first.access;
        state.first_intent = first.intent;
        state.first_footprint = first.footprint;
        state.first_has_footprint_radius =
            first.has_footprint_radius;
        state.first_footprint_radius =
            first.footprint_radius;
        state.first_type_pattern_json.assign(
            first.type_pattern_json_utf8,
            first.type_pattern_json_size);
        state.first_relations_json.assign(
            first.relations_json_utf8,
            first.relations_json_size);
    }
    if (state.behavior ==
            ProviderBehavior::unavailable ||
        (state.behavior ==
             ProviderBehavior::unavailable_after_first &&
         state.calls > 1)) {
        return RenderPass::Status::unavailable;
    }
    *output =
        RenderPass::descriptor<
            RenderPass::FullscreenImplementationV1>();
    if (state.behavior ==
        ProviderBehavior::invalid_output) {
        output->reserved4 = 1;
        return RenderPass::Status::ok;
    }
    output->implementation_id_utf8 =
        state.implementation.data();
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            state.implementation.size());
    output->vertex_shader_utf8 =
        state.vertex.data();
    output->vertex_shader_size =
        static_cast<std::uint32_t>(
            state.vertex.size());
    output->fragment_shader_utf8 =
        state.fragment.data();
    output->fragment_shader_size =
        static_cast<std::uint32_t>(
            state.fragment.size());
    return RenderPass::Status::ok;
}

RenderPass::ProviderV1 provider(
    std::string_view name, ProviderState &state) {
    auto result =
        RenderPass::descriptor<
            RenderPass::ProviderV1>();
    result.capability_bits =
        RenderPass::builtinProviderCapabilitiesV1;
    result.name_utf8 = name.data();
    result.name_size =
        static_cast<std::uint32_t>(name.size());
    result.context = &state;
    result.resolve_fullscreen =
        resolveFullscreen;
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
    "WP200 builtin fullscreen implementation uses the typed provider path",
    "[render-pass][provider][wp200]") {
    PassImplementationRegistry registry;
    auto definition =
        RenderingPassDefinition{
            "main", {fullscreenPass()}};
    const auto node = fullscreenNode();
    const auto plan = targetPlan(node);
    {
        const auto providers = registry.snapshot();
        resolveRenderingPassImplementations(
            definition, plan, providers);
    }

    const auto &pass = definition.passes.front();
    REQUIRE(
        pass.fullscreenInfo().vert_shader.ref ==
        "engine://fullscreen");
    REQUIRE(
        pass.fullscreenInfo().frag_shader.ref ==
        "engine://scene_present");
    REQUIRE(pass.implementation_selection.has_value());
    const auto &selection =
        *pass.implementation_selection;
    REQUIRE(
        selection.provider ==
        std::string{
            builtinFullscreenPassImplementationProvider});
    REQUIRE(
        selection.implementation ==
        "pelican.render.authored_fullscreen@1");
    REQUIRE(
        selection.contract ==
        RenderPass::fullscreenContractIdV1);
    REQUIRE(selection.contract_fingerprint != 0);
    REQUIRE(
        selection.provider_owner ==
        internal::engineRegistrationOwner);
    REQUIRE_FALSE(selection.explicitly_selected);

    auto renamed_pass = fullscreenPass("renamed");
    auto renamed_node = fullscreenNode("renamed");
    REQUIRE(
        makeFullscreenPassImplementationContract(
            renamed_pass, renamed_node)
            .fingerprint ==
        selection.contract_fingerprint);

    auto output_transform_pass =
        fullscreenPass("output_transform");
    auto output_transform_node = fullscreenNode(
        "output_transform",
        LogicalGraphNodeKind::output_transform);
    REQUIRE_NOTHROW(
        makeFullscreenPassImplementationContract(
            output_transform_pass,
            output_transform_node));
    auto compute_node = fullscreenNode(
        "composite",
        LogicalGraphNodeKind::compute);
    requireThrowsContaining(
        [&] {
            (void)makeFullscreenPassImplementationContract(
                fullscreenPass(), compute_node);
        },
        "no compatible logical fullscreen node");
}

TEST_CASE(
    "WP200 game owner replaces only implementation data and keeps typed contract",
    "[render-pass][provider][contract][wp200]") {
    PassImplementationRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.fullscreen";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderPass::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderPass::Status::ok);

    auto definition =
        RenderingPassDefinition{
            "main", {fullscreenPass()}};
    definition.passes.front()
        .requested_implementation_provider =
        std::string{name};
    auto node = fullscreenNode();
    node.uses.front().footprint =
        LogicalReadFootprint{
            LogicalReadFootprintKind::neighborhood,
            3};
    const auto plan = targetPlan(node);
    {
        const auto inactive = registry.snapshot();
        requireThrowsContaining(
            [&] {
                resolveRenderingPassImplementations(
                    definition, plan, inactive);
            },
            "not registered for the active owner");
    }

    registry.activateOwner(owner);
    {
        const auto providers = registry.snapshot();
        resolveRenderingPassImplementations(
            definition, plan, providers);
    }
    const auto &pass = definition.passes.front();
    REQUIRE(
        pass.fullscreenInfo().vert_shader.ref ==
        state.vertex);
    REQUIRE(
        pass.fullscreenInfo().frag_shader.ref ==
        state.fragment);
    REQUIRE(
        pass.fullscreenInfo().push_constants ==
        FullscreenPushConstantData::eProjectionView);
    REQUIRE(pass.fullscreenInfo().uses_light_data);
    REQUIRE(
        pass.implementation_selection->provider ==
        std::string{name});
    REQUIRE(
        pass.implementation_selection->provider_owner ==
        owner);
    REQUIRE(
        pass.implementation_selection
            ->provider_identity ==
        handle.identity);
    REQUIRE(
        pass.implementation_selection
            ->provider_generation ==
        handle.generation);
    REQUIRE(
        pass.implementation_selection
            ->explicitly_selected);
    REQUIRE(state.calls == 1);
    REQUIRE(state.port_count == 2);
    REQUIRE(
        state.interface_flags ==
        (RenderPass::interface_projection_view |
         RenderPass::interface_light_data));
    REQUIRE(
        state.fingerprint ==
        pass.implementation_selection
            ->contract_fingerprint);
    REQUIRE(
        state.first_footprint ==
        RenderPass::ReadFootprintV1::neighborhood);
    REQUIRE(
        state.first_direction ==
        RenderPass::PortDirectionV1::input);
    REQUIRE(
        state.first_access ==
        RenderPass::AccessModeV1::read);
    REQUIRE(
        state.first_intent ==
        RenderPass::AccessIntentV1::sampled);
    REQUIRE(state.first_has_footprint_radius == 1);
    REQUIRE(state.first_footprint_radius == 3);
    REQUIRE(
        state.first_type_pattern_json.find(
            "pelican.render.color_signal@1") !=
        std::string::npos);
    REQUIRE(state.first_relations_json == "[]");

    auto wider_node = node;
    wider_node.uses.front().footprint.radius = 4;
    REQUIRE(
        makeFullscreenPassImplementationContract(
            pass, wider_node)
            .fingerprint !=
        state.fingerprint);

    state.behavior = ProviderBehavior::unavailable;
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                resolveRenderingPassImplementations(
                    definition, plan, providers);
            },
            "status unavailable");
    }
    state.behavior =
        ProviderBehavior::invalid_output;
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                resolveRenderingPassImplementations(
                    definition, plan, providers);
            },
            "invalid output envelope");
    }
    state.behavior = ProviderBehavior::replace;
    state.implementation = "not-versioned";
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                resolveRenderingPassImplementations(
                    definition, plan, providers);
            },
            "invalid implementation id");
    }
    state.implementation =
        "fixture.render.composite@1";
    state.fragment.clear();
    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                resolveRenderingPassImplementations(
                    definition, plan, providers);
            },
            "invalid fragment shader reference");
    }

    REQUIRE(
        registry.unregisterProvider(handle, owner) ==
        RenderPass::Status::ok);
    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP200 pass provider registry rejects stale handles and public API binds owners",
    "[render-pass][provider][abi][lifetime][wp200]") {
    PassImplementationRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.lifecycle";
    const auto owner =
        internal::allocateRegistrationOwner();
    const auto other =
        internal::allocateRegistrationOwner();
    auto declaration = provider(name, state);
    RenderPass::ProviderHandleV1 first{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, first) ==
        RenderPass::Status::ok);
    RenderPass::ProviderHandleV1 duplicate{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, duplicate) ==
        RenderPass::Status::duplicate_provider);
    REQUIRE(
        registry.unregisterProvider(first, other) ==
        RenderPass::Status::wrong_owner);
    REQUIRE(
        registry.unregisterProvider(first, owner) ==
        RenderPass::Status::ok);

    RenderPass::ProviderHandleV1 second{};
    REQUIRE(
        registry.registerProvider(
            declaration, owner, second) ==
        RenderPass::Status::ok);
    REQUIRE(second.identity == first.identity);
    REQUIRE(second.generation != first.generation);
    REQUIRE(
        registry.unregisterProvider(first, owner) ==
        RenderPass::Status::stale_provider);

    auto malformed = declaration;
    malformed.resolve_fullscreen = nullptr;
    REQUIRE(
        registry.registerProvider(
            malformed, other, duplicate) ==
        RenderPass::Status::invalid_argument);
    malformed = declaration;
    malformed.capability_bits = 0;
    REQUIRE(
        registry.registerProvider(
            malformed, other, duplicate) ==
        RenderPass::Status::invalid_argument);
    malformed = declaration;
    malformed.provider_version =
        RenderPass::providerVersionV1 + 1;
    REQUIRE(
        registry.registerProvider(
            malformed, other, duplicate) ==
        RenderPass::Status::invalid_argument);
    registry.releaseOwner(owner);
    REQUIRE(
        registry.registerProvider(
            declaration, owner, duplicate) ==
        RenderPass::Status::stale_owner);
    internal::releaseRegistrationOwner(owner);
    registry.releaseOwner(other);
    internal::releaseRegistrationOwner(other);

    auto api =
        RenderPass::descriptor<RenderPass::ApiV1>();
    REQUIRE(
        RenderPass::getApiV1(
            RenderPass::abiVersionV1, &api) ==
        RenderPass::Status::ok);
    REQUIRE(
        (api.capability_bits &
         RenderPass::api_provider_registration) != 0);
    RenderPass::ProviderHandleV1 api_handle{};
    REQUIRE(
        api.register_provider(
            api.context, &declaration,
            &api_handle) ==
        RenderPass::Status::wrong_owner);
}

TEST_CASE(
    "WP200 pass implementation resolution is failure atomic",
    "[render-pass][provider][transaction][wp200]") {
    PassImplementationRegistry registry;
    ProviderState state;
    state.behavior =
        ProviderBehavior::unavailable_after_first;
    constexpr std::string_view name =
        "fixture.atomic";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderPass::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderPass::Status::ok);
    registry.activateOwner(owner);

    auto first = fullscreenPass("first");
    first.requested_implementation_provider =
        std::string{name};
    auto second = fullscreenPass("second");
    second.requested_implementation_provider =
        std::string{name};
    RenderingPassDefinition definition{
        "main", {first, second}};
    auto plan = targetPlan(fullscreenNode("first"));
    plan.lowering_graph.nodes.push_back(
        TargetLoweringNode{
            .logical = fullscreenNode("second")});

    {
        const auto providers = registry.snapshot();
        requireThrowsContaining(
            [&] {
                resolveRenderingPassImplementations(
                    definition, plan, providers);
            },
            "status unavailable");
    }
    REQUIRE(state.calls == 2);
    for (const auto &pass : definition.passes) {
        REQUIRE(
            pass.fullscreenInfo().vert_shader.ref ==
            "engine://fullscreen");
        REQUIRE(
            pass.fullscreenInfo().frag_shader.ref ==
            "engine://scene_present");
        REQUIRE_FALSE(
            pass.implementation_selection.has_value());
    }

    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE(
    "WP200 pass provider snapshot leases block owner release",
    "[render-pass][provider][lifetime][wp200]") {
    using namespace std::chrono_literals;

    PassImplementationRegistry registry;
    ProviderState state;
    constexpr std::string_view name =
        "fixture.snapshot_lease";
    const auto owner =
        internal::allocateRegistrationOwner();
    RenderPass::ProviderHandleV1 handle{};
    REQUIRE(
        registry.registerProvider(
            provider(name, state), owner, handle) ==
        RenderPass::Status::ok);
    registry.activateOwner(owner);

    std::optional<PassImplementationRegistrySnapshot>
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
    {
        const auto providers = registry.snapshot();
        auto pass = fullscreenPass();
        pass.requested_implementation_provider =
            std::string{name};
        requireThrowsContaining(
            [&] {
                (void)providers.resolveFullscreen(
                    pass, fullscreenNode());
            },
            "not registered for the active owner");
    }
    internal::releaseRegistrationOwner(owner);
}

} // namespace Pelican
