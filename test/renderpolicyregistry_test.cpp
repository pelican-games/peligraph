#include "../src/core/renderer/drawqueuebuilder.hpp"
#include "../src/core/renderer/renderpolicyregistry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace Pelican {
namespace {

DrawItemSnapshot item(std::uint64_t ordinal, std::uint32_t instance) {
    return DrawItemSnapshot{
        .stable_identity =
            DrawStableIdentity{
                .instance = ModelInstanceId{instance, 1, 4},
                .mesh_index = instance % 3,
                .primitive_index = instance + 10,
                .node_index = instance + 20,
            },
        .declaration_ordinal = ordinal,
        .indexed =
            DrawIndexedArguments{
                .index_count = 3,
                .instance_count = 1,
                .first_index = instance * 3,
                .vertex_offset = static_cast<std::int32_t>(instance),
                .first_instance = instance,
            },
        .pipeline_material_key =
            DrawPipelineMaterialKey{
                .material = GlobalMaterialId{2},
                .source_material_index = 7,
            },
        .world_bounds = DrawWorldBounds{
            .minimum = {-1.0F, -2.0F, -3.0F},
            .maximum = {1.0F, 2.0F, 3.0F},
        },
        .view_mask = DrawViewMask::both,
    };
}

enum class ProviderBehavior {
    reverse_declaration,
    equal_keys,
    short_output,
    error,
    unknown_status,
};

struct ProviderState {
    ProviderBehavior behavior = ProviderBehavior::reverse_declaration;
    std::uint32_t calls = 0;
    RenderPolicy::DrawSortPhaseV1 last_phase =
        RenderPolicy::DrawSortPhaseV1::opaque;
    RenderPolicy::DrawSortLogicalViewV1 last_view =
        RenderPolicy::DrawSortLogicalViewV1::first_person;
};

RenderPolicy::Status customSort(
    void *context, const RenderPolicy::DrawSortInputV1 *input,
    RenderPolicy::DrawSortKeyV1 *output, std::uint32_t capacity,
    std::uint32_t *out_count) noexcept {
    if (context == nullptr || input == nullptr || out_count == nullptr ||
        input->struct_size < sizeof(RenderPolicy::DrawSortInputV1) ||
        input->version != RenderPolicy::descriptorVersionV1 ||
        input->reserved0 != 0 || input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        (input->item_count != 0 && input->items == nullptr) ||
        (capacity != 0 && output == nullptr)) {
        return RenderPolicy::Status::invalid_argument;
    }
    auto &state = *static_cast<ProviderState *>(context);
    ++state.calls;
    state.last_phase = input->target_phase;
    state.last_view = input->logical_view;
    if (state.behavior == ProviderBehavior::error) {
        return RenderPolicy::Status::provider_error;
    }
    if (state.behavior == ProviderBehavior::unknown_status) {
        return static_cast<RenderPolicy::Status>(999);
    }
    *out_count = input->item_count;
    if (state.behavior == ProviderBehavior::short_output && *out_count != 0) {
        --*out_count;
    }
    if (capacity < input->item_count) {
        return RenderPolicy::Status::buffer_too_small;
    }
    for (std::uint32_t index = 0; index < input->item_count; ++index) {
        const auto key =
            state.behavior == ProviderBehavior::equal_keys
                ? 0
                : std::numeric_limits<std::uint64_t>::max() -
                      input->items[index].declaration_ordinal;
        output[index] = {.primary = key};
    }
    return RenderPolicy::Status::ok;
}

RenderPolicy::ProviderV1 provider(std::string_view name,
                                  ProviderState &state) {
    auto result = RenderPolicy::descriptor<RenderPolicy::ProviderV1>();
    result.capability_bits = RenderPolicy::builtinProviderCapabilitiesV1;
    result.name_utf8 = name.data();
    result.name_size = static_cast<std::uint32_t>(name.size());
    result.context = &state;
    result.sort_items = customSort;
    return result;
}

template <class Function>
void requireThrowsContaining(Function &&function, std::string_view text) {
    try {
        function();
        FAIL("expected an exception");
    } catch (const std::exception &error) {
        REQUIRE(std::string{error.what()}.find(text) != std::string::npos);
    }
}

} // namespace

TEST_CASE("builtin draw sort provider is versioned and adds a stable identity tie break",
          "[renderer][render-policy][wp183]") {
    RenderPolicyRegistry registry;
    const auto provider_lease =
        registry.resolveDrawSortProvider(builtinStateBatchedDrawSortProvider);
    REQUIRE(provider_lease.info().name ==
            std::string{builtinStateBatchedDrawSortProvider});
    REQUIRE(provider_lease.info().owner == internal::engineRegistrationOwner);
    REQUIRE(provider_lease.info().provider_version ==
            RenderPolicy::providerVersionV1);
    REQUIRE(provider_lease.info().capability_bits ==
            RenderPolicy::builtinProviderCapabilitiesV1);
    REQUIRE(provider_lease.info().handle.identity != 0);
    REQUIRE(provider_lease.info().handle.generation != 0);

    const std::vector input{item(0, 9), item(1, 3), item(2, 6)};
    const auto original = input;
    const DrawQueueBuildRequest request{
        .items = input,
        .max_draw_indirect_count = 64,
    };
    const auto first = DrawQueueBuilder::build(request, provider_lease);
    const auto second = DrawQueueBuilder::build(request, provider_lease);

    REQUIRE(input == original);
    REQUIRE(first.orderedItems() == second.orderedItems());
    REQUIRE(first.orderedItems().at(0).stable_identity.instance.index == 3);
    REQUIRE(first.orderedItems().at(1).stable_identity.instance.index == 6);
    REQUIRE(first.orderedItems().at(2).stable_identity.instance.index == 9);
    REQUIRE(first.provider() == provider_lease.info());
}

TEST_CASE("custom draw sort keys are contained and malformed callback output is named",
          "[renderer][render-policy][provider][wp183]") {
    RenderPolicyRegistry registry;
    ProviderState state;
    constexpr std::string_view name = "fixture.reverse";
    const auto owner = internal::allocateRegistrationOwner();
    RenderPolicy::ProviderHandleV1 handle{};
    const auto declaration = provider(name, state);
    REQUIRE(registry.registerDrawSortProvider(declaration, owner, handle) ==
            RenderPolicy::Status::ok);

    requireThrowsContaining(
        [&] { (void)registry.resolveDrawSortProvider(name); }, name);
    registry.activateOwner(owner);

    const std::vector input{item(0, 4), item(1, 5), item(2, 6)};
    const auto original = input;
    {
        const auto lease = registry.resolveDrawSortProvider(name);
        const DrawQueueBuildRequest request{
            .items = input,
            .max_draw_indirect_count = 64,
        };
        const auto reverse = DrawQueueBuilder::build(request, lease);
        REQUIRE(input == original);
        REQUIRE(reverse.orderedItems().at(0).declaration_ordinal == 2);
        REQUIRE(reverse.orderedItems().at(1).declaration_ordinal == 1);
        REQUIRE(reverse.orderedItems().at(2).declaration_ordinal == 0);
        REQUIRE(state.last_phase == RenderPolicy::DrawSortPhaseV1::mixed);
        REQUIRE(state.last_view ==
                RenderPolicy::DrawSortLogicalViewV1::shared);

        state.behavior = ProviderBehavior::short_output;
        requireThrowsContaining(
            [&] { (void)DrawQueueBuilder::build(request, lease); },
            "fixture.reverse' returned 2 keys for 3");

        state.behavior = ProviderBehavior::error;
        requireThrowsContaining(
            [&] { (void)DrawQueueBuilder::build(request, lease); },
            "fixture.reverse' failed with status provider_error");

        state.behavior = ProviderBehavior::unknown_status;
        requireThrowsContaining(
            [&] { (void)DrawQueueBuilder::build(request, lease); },
            "fixture.reverse' failed with status unknown_status");
    }
    REQUIRE(state.calls == 4);

    REQUIRE(registry.unregisterDrawSortProvider(handle, owner) ==
            RenderPolicy::Status::ok);
    registry.releaseOwner(owner);
    internal::releaseRegistrationOwner(owner);
}

TEST_CASE("render policy registry rejects malformed descriptors and stale generations",
          "[renderer][render-policy][lifetime][wp183]") {
    RenderPolicyRegistry registry;
    ProviderState state;
    constexpr std::string_view name = "fixture.lifecycle";
    const auto first_owner = internal::allocateRegistrationOwner();
    const auto other_owner = internal::allocateRegistrationOwner();
    auto declaration = provider(name, state);
    RenderPolicy::ProviderHandleV1 first_handle{};
    REQUIRE(registry.registerDrawSortProvider(declaration, first_owner,
                                              first_handle) ==
            RenderPolicy::Status::ok);

    RenderPolicy::ProviderHandleV1 duplicate{};
    REQUIRE(registry.registerDrawSortProvider(declaration, first_owner,
                                              duplicate) ==
            RenderPolicy::Status::duplicate_provider);
    REQUIRE(registry.unregisterDrawSortProvider(first_handle, other_owner) ==
            RenderPolicy::Status::wrong_owner);
    REQUIRE(registry.unregisterDrawSortProvider(first_handle, first_owner) ==
            RenderPolicy::Status::ok);

    RenderPolicy::ProviderHandleV1 second_handle{};
    REQUIRE(registry.registerDrawSortProvider(declaration, first_owner,
                                              second_handle) ==
            RenderPolicy::Status::ok);
    REQUIRE(second_handle.identity == first_handle.identity);
    REQUIRE(second_handle.generation != first_handle.generation);
    REQUIRE(registry.unregisterDrawSortProvider(first_handle, first_owner) ==
            RenderPolicy::Status::stale_provider);

    auto malformed = declaration;
    malformed.reserved2 = 1;
    REQUIRE(registry.registerDrawSortProvider(malformed, other_owner,
                                              duplicate) ==
            RenderPolicy::Status::invalid_argument);
    malformed = declaration;
    malformed.provider_version = RenderPolicy::providerVersionV1 + 1;
    REQUIRE(registry.registerDrawSortProvider(malformed, other_owner,
                                              duplicate) ==
            RenderPolicy::Status::invalid_argument);
    malformed = declaration;
    malformed.capability_bits |= 1ULL << 63U;
    REQUIRE(registry.registerDrawSortProvider(malformed, other_owner,
                                              duplicate) ==
            RenderPolicy::Status::invalid_argument);
    malformed = declaration;
    malformed.sort_items = nullptr;
    REQUIRE(registry.registerDrawSortProvider(malformed, other_owner,
                                              duplicate) ==
            RenderPolicy::Status::invalid_argument);

    registry.releaseOwner(first_owner);
    REQUIRE(registry.registerDrawSortProvider(declaration, first_owner,
                                              duplicate) ==
            RenderPolicy::Status::stale_owner);
    internal::releaseRegistrationOwner(first_owner);
    REQUIRE(registry.registerDrawSortProvider(declaration, first_owner,
                                              duplicate) ==
            RenderPolicy::Status::stale_owner);
    requireThrowsContaining(
        [&] { (void)registry.resolveDrawSortProvider("missing.provider"); },
        "missing.provider");

    registry.releaseOwner(other_owner);
    internal::releaseRegistrationOwner(other_owner);
}

TEST_CASE("public render policy API binds registrations to the loading owner",
          "[renderer][render-policy][abi][wp183]") {
    auto api = RenderPolicy::descriptor<RenderPolicy::ApiV1>();
    REQUIRE(RenderPolicy::getApiV1(RenderPolicy::abiVersionV1, &api) ==
            RenderPolicy::Status::ok);
    REQUIRE((api.capability_bits & RenderPolicy::api_provider_registration) !=
            0);

    ProviderState state;
    auto declaration = provider("fixture.public-api", state);
    RenderPolicy::ProviderHandleV1 handle{};
    REQUIRE(api.register_provider(api.context, &declaration, &handle) ==
            RenderPolicy::Status::wrong_owner);

    const auto owner = internal::allocateRegistrationOwner();
    {
        internal::ScopedRegistrationOwner owner_scope{owner};
        REQUIRE(api.register_provider(api.context, &declaration, &handle) ==
                RenderPolicy::Status::ok);
    }
    render_policy_internal::activateProviderOwner(owner);
    {
        const auto lease = renderPolicyRegistry().resolveDrawSortProvider(
            "fixture.public-api");
        REQUIRE(lease.info().owner == owner);
        REQUIRE(lease.info().handle == handle);
    }

    render_policy_internal::releaseProviderOwner(owner);
    internal::releaseRegistrationOwner(owner);
    {
        internal::ScopedRegistrationOwner stale_scope{owner};
        REQUIRE(api.unregister_provider(api.context, handle) ==
                RenderPolicy::Status::stale_owner);
    }

    auto malformed_api = RenderPolicy::descriptor<RenderPolicy::ApiV1>();
    malformed_api.reserved0 = 1;
    REQUIRE(RenderPolicy::getApiV1(RenderPolicy::abiVersionV1,
                                   &malformed_api) ==
            RenderPolicy::Status::reserved_not_zero);
    malformed_api = RenderPolicy::descriptor<RenderPolicy::ApiV1>();
    REQUIRE(RenderPolicy::getApiV1(RenderPolicy::abiVersionV1 + 1,
                                   &malformed_api) ==
            RenderPolicy::Status::unsupported_version);
}

} // namespace Pelican
