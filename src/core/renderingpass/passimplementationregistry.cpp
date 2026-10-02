#include "passimplementationregistry.hpp"
#include "../registrationownerhost.hpp"

#include "providerregistrycommon.hpp"
#include "../../project/stablefingerprint.hpp"
#include "../../project/targetrenderplanning.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Pelican {
namespace {

using RenderPass::Status;

constexpr char authoredFullscreenImplementationIdV1[] =
    "pelican.render.authored_fullscreen@1";

using provider_registry_common::statusName;
using provider_registry_common::validByteRange;

bool validProvider(const RenderPass::ProviderV1 &provider) noexcept {
    return provider_registry_common::validProviderEnvelope(
        provider, &RenderPass::ProviderV1::resolve_fullscreen,
        RenderPass::descriptorVersionV1,
        RenderPass::providerVersionV1,
        RenderPass::builtinProviderCapabilitiesV1,
        RenderPass::maximumProviderNameBytesV1);
}

std::uint32_t abiSize(
    std::size_t size, std::string_view field,
    std::string_view pass) {
    return provider_registry_common::abiSizeChecked(
        size, field, "pass implementation", "pass ", pass);
}

Status builtinResolveFullscreen(
    void *, const RenderPass::ResolveFullscreenInputV1 *input,
    RenderPass::FullscreenImplementationV1 *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size < sizeof(RenderPass::ResolveFullscreenInputV1) ||
        input->version != RenderPass::descriptorVersionV1 ||
        input->reserved0 != 0 || input->reserved1 != 0 ||
        input->contract == nullptr ||
        input->authored_implementation == nullptr ||
        output->struct_size <
            sizeof(RenderPass::FullscreenImplementationV1) ||
        output->version != RenderPass::descriptorVersionV1 ||
        output->reserved0 != 0 || output->reserved1 != 0 ||
        output->reserved2 != 0 || output->reserved3 != 0 ||
        output->reserved4 != 0) {
        return Status::invalid_argument;
    }
    *output = *input->authored_implementation;
    return Status::ok;
}

RenderPass::PortDirectionV1 portDirection(
    LogicalPortDirection direction) {
    switch (direction) {
    case LogicalPortDirection::input:
        return RenderPass::PortDirectionV1::input;
    case LogicalPortDirection::output:
        return RenderPass::PortDirectionV1::output;
    case LogicalPortDirection::input_output:
        return RenderPass::PortDirectionV1::input_output;
    }
    throw std::runtime_error(
        "unknown logical port direction in pass contract");
}

RenderPass::AccessModeV1 accessMode(
    LogicalAccessMode access) {
    switch (access) {
    case LogicalAccessMode::read:
        return RenderPass::AccessModeV1::read;
    case LogicalAccessMode::write:
        return RenderPass::AccessModeV1::write;
    case LogicalAccessMode::read_write:
        return RenderPass::AccessModeV1::read_write;
    }
    throw std::runtime_error(
        "unknown logical access mode in pass contract");
}

RenderPass::AccessIntentV1 accessIntent(
    LogicalAccessIntent intent) {
    switch (intent) {
    case LogicalAccessIntent::automatic:
        return RenderPass::AccessIntentV1::automatic;
    case LogicalAccessIntent::sampled:
        return RenderPass::AccessIntentV1::sampled;
    case LogicalAccessIntent::attachment:
        return RenderPass::AccessIntentV1::attachment;
    case LogicalAccessIntent::storage:
        return RenderPass::AccessIntentV1::storage;
    case LogicalAccessIntent::transfer:
        return RenderPass::AccessIntentV1::transfer;
    case LogicalAccessIntent::host:
        return RenderPass::AccessIntentV1::host;
    }
    throw std::runtime_error(
        "unknown logical access intent in pass contract");
}

RenderPass::ReadFootprintV1 readFootprint(
    LogicalReadFootprintKind footprint) {
    switch (footprint) {
    case LogicalReadFootprintKind::none:
        return RenderPass::ReadFootprintV1::none;
    case LogicalReadFootprintKind::same_pixel:
        return RenderPass::ReadFootprintV1::same_pixel;
    case LogicalReadFootprintKind::neighborhood:
        return RenderPass::ReadFootprintV1::neighborhood;
    case LogicalReadFootprintKind::arbitrary:
        return RenderPass::ReadFootprintV1::arbitrary;
    case LogicalReadFootprintKind::temporal:
        return RenderPass::ReadFootprintV1::temporal;
    }
    throw std::runtime_error(
        "unknown logical read footprint in pass contract");
}

nlohmann::ordered_json relationToJson(
    const LogicalPortRelation &relation) {
    return {
        {"kind", logicalPortRelationKindName(relation.kind)},
        {"other_port", relation.other_port},
        {"scale_x",
         {{"numerator", relation.scale_x.numerator},
          {"denominator", relation.scale_x.denominator}}},
        {"scale_y",
         {{"numerator", relation.scale_y.numerator},
          {"denominator", relation.scale_y.denominator}}},
    };
}

using StableFingerprint = StableFingerprint64;

std::string copyProviderString(
    const char *data, std::uint32_t size,
    std::uint32_t maximum, std::string_view field,
    std::string_view provider, std::string_view pass) {
    if (!validByteRange(data, size, maximum)) {
        throw std::runtime_error(
            "pass implementation provider '" +
            std::string{provider} + "' returned invalid " +
            std::string{field} + " for pass '" +
            std::string{pass} + "'");
    }
    return std::string{data, size};
}

void validateOutputEnvelope(
    const RenderPass::FullscreenImplementationV1 &output,
    std::string_view provider, std::string_view pass) {
    if (output.struct_size <
            sizeof(RenderPass::FullscreenImplementationV1) ||
        output.version != RenderPass::descriptorVersionV1 ||
        output.reserved0 != 0 || output.reserved1 != 0 ||
        output.reserved2 != 0 || output.reserved3 != 0 ||
        output.reserved4 != 0) {
        throw std::runtime_error(
            "pass implementation provider '" +
            std::string{provider} +
            "' returned an invalid output envelope for pass '" +
            std::string{pass} + "'");
    }
}

int api_context_token = 0;

} // namespace

struct PassImplementationRegistryTraits {
    using Provider = RenderPass::ProviderV1;
    using Handle = RenderPass::ProviderHandleV1;
    using Status = RenderPass::Status;
    using Resolve = RenderPass::ResolveFullscreenV1Fn;

    static constexpr auto provider_resolve =
        &Provider::resolve_fullscreen;
    static constexpr std::string_view builtin_name =
        builtinFullscreenPassImplementationProvider;
    static constexpr std::uint64_t builtin_capability_bits =
        RenderPass::builtinProviderCapabilitiesV1;
    static constexpr Resolve builtin_resolve = builtinResolveFullscreen;
    static constexpr std::string_view builtin_registration_failure =
        "failed to register builtin fullscreen pass implementation "
        "provider";

    static Provider descriptor() noexcept {
        return RenderPass::descriptor<Provider>();
    }
    static bool valid(const Provider &provider) noexcept {
        return validProvider(provider);
    }
};

struct PassImplementationRegistryState
    : provider_registry_common::ProviderRegistry<
          PassImplementationRegistryTraits> {};

struct PassImplementationRegistrySnapshot::Impl {
    std::shared_lock<std::shared_mutex> lock;
    const PassImplementationRegistryState *registry = nullptr;

    Impl(std::shared_lock<std::shared_mutex> registry_lock,
         const PassImplementationRegistryState *value) noexcept
        : lock{std::move(registry_lock)}, registry{value} {}
};

PassImplementationContract makeFullscreenPassImplementationContract(
    const PassDefinition &pass,
    const LogicalGraphNode &logical_node) {
    if (!pass.isFullscreen()) {
        throw std::runtime_error(
            "fullscreen pass implementation contract requested for "
            "non-fullscreen pass: " +
            pass.name);
    }
    const auto is_fullscreen_logical_node =
        logical_node.kind == LogicalGraphNodeKind::render ||
        logical_node.kind ==
            LogicalGraphNodeKind::output_transform;
    if (pass.name.empty() || logical_node.name != pass.name ||
        !is_fullscreen_logical_node) {
        throw std::runtime_error(
            "fullscreen pass implementation contract has no compatible "
            "logical fullscreen node: " +
            pass.name);
    }

    PassImplementationContract result;
    result.id = RenderPass::fullscreenContractIdV1;
    result.pass_name = pass.name;
    switch (pass.fullscreenInfo().push_constants) {
    case FullscreenPushConstantData::eNone: break;
    case FullscreenPushConstantData::eCameraPosition:
        result.interface_flags |=
            RenderPass::interface_camera_position;
        break;
    case FullscreenPushConstantData::eProjectionView:
        result.interface_flags |=
            RenderPass::interface_projection_view;
        break;
    }
    if (pass.fullscreenInfo().uses_light_data) {
        result.interface_flags |=
            RenderPass::interface_light_data;
    }

    result.ports.reserve(logical_node.ports.size());
    for (const auto &port : logical_node.ports) {
        const LogicalResourceUse *selected_use = nullptr;
        for (const auto &use : logical_node.uses) {
            if (use.port != port.name) continue;
            if (selected_use != nullptr) {
                throw std::runtime_error(
                    "fullscreen pass logical port has multiple resource "
                    "uses: " +
                    pass.name + "/" + port.name);
            }
            selected_use = &use;
        }
        if (selected_use == nullptr) {
            throw std::runtime_error(
                "fullscreen pass logical port has no resource use: " +
                pass.name + "/" + port.name);
        }

        auto relations = nlohmann::ordered_json::array();
        for (const auto &relation : port.relations) {
            relations.push_back(relationToJson(relation));
        }
        result.ports.push_back(
            PassImplementationPortContract{
                .name = port.name,
                .type_pattern_json =
                    logicalTypePatternToJson(
                        port.accepted_type)
                        .dump(),
                .relations_json = relations.dump(),
                .direction = portDirection(port.direction),
                .access = accessMode(selected_use->access),
                .intent = accessIntent(selected_use->intent),
                .footprint =
                    readFootprint(
                        selected_use->footprint.kind),
                .footprint_radius =
                    selected_use->footprint.radius,
            });
    }

    StableFingerprint fingerprint;
    fingerprint.appendString(result.id);
    fingerprint.appendUnsigned(result.interface_flags);
    fingerprint.appendUnsigned(result.ports.size());
    for (const auto &port : result.ports) {
        fingerprint.appendString(port.name);
        fingerprint.appendString(port.type_pattern_json);
        fingerprint.appendString(port.relations_json);
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(port.direction));
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(port.access));
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(port.intent));
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(port.footprint));
        fingerprint.appendUnsigned(
            port.footprint_radius.has_value());
        fingerprint.appendUnsigned(
            port.footprint_radius.value_or(0));
    }
    result.fingerprint = fingerprint.value();
    return result;
}

PassImplementationRegistrySnapshot::
    PassImplementationRegistrySnapshot() noexcept = default;
PassImplementationRegistrySnapshot::
    ~PassImplementationRegistrySnapshot() = default;
PassImplementationRegistrySnapshot::
    PassImplementationRegistrySnapshot(
        PassImplementationRegistrySnapshot &&) noexcept = default;
PassImplementationRegistrySnapshot &
PassImplementationRegistrySnapshot::operator=(
    PassImplementationRegistrySnapshot &&) noexcept = default;

PassImplementationRegistrySnapshot::
    PassImplementationRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

ResolvedFullscreenPassImplementation
PassImplementationRegistrySnapshot::resolveFullscreen(
    const PassDefinition &pass,
    const LogicalGraphNode &logical_node) const {
    if (impl_ == nullptr || impl_->registry == nullptr) {
        throw std::runtime_error(
            "pass implementation registry snapshot is empty");
    }
    const auto &registry = *impl_->registry;
    const auto requested =
        pass.requested_implementation_provider
            .value_or(std::string{
                builtinFullscreenPassImplementationProvider});
    if (requested.empty()) {
        throw std::runtime_error(
            "pass implementation provider name must not be empty: " +
            pass.name);
    }

    const PassImplementationRegistryState::ProviderSlot *selected =
        registry.findActiveProvider(requested);
    if (selected == nullptr) {
        throw std::runtime_error(
            "pass implementation provider '" + requested +
            "' is not registered for the active owner while compiling pass '" +
            pass.name + "'");
    }

    const auto contract =
        makeFullscreenPassImplementationContract(
            pass, logical_node);
    std::vector<RenderPass::PortContractV1> abi_ports;
    abi_ports.reserve(contract.ports.size());
    for (const auto &port : contract.ports) {
        auto abi =
            RenderPass::descriptor<
                RenderPass::PortContractV1>();
        abi.name_utf8 = port.name.data();
        abi.name_size =
            abiSize(
                port.name.size(), "port name",
                pass.name);
        abi.type_pattern_json_utf8 =
            port.type_pattern_json.data();
        abi.type_pattern_json_size =
            abiSize(
                port.type_pattern_json.size(),
                "type-pattern JSON", pass.name);
        abi.relations_json_utf8 =
            port.relations_json.data();
        abi.relations_json_size =
            abiSize(
                port.relations_json.size(),
                "relation JSON", pass.name);
        abi.direction = port.direction;
        abi.access = port.access;
        abi.intent = port.intent;
        abi.footprint = port.footprint;
        abi.has_footprint_radius =
            static_cast<std::uint32_t>(
                port.footprint_radius.has_value());
        abi.footprint_radius =
            port.footprint_radius.value_or(0);
        abi_ports.push_back(abi);
    }

    auto abi_contract =
        RenderPass::descriptor<
            RenderPass::PassContractV1>();
    abi_contract.contract_id_utf8 = contract.id.data();
    abi_contract.contract_id_size =
        abiSize(
            contract.id.size(), "contract id",
            pass.name);
    abi_contract.pass_name_utf8 =
        contract.pass_name.data();
    abi_contract.pass_name_size =
        abiSize(
            contract.pass_name.size(), "pass name",
            pass.name);
    abi_contract.interface_flags =
        contract.interface_flags;
    abi_contract.ports = abi_ports.data();
    abi_contract.port_count =
        abiSize(
            abi_ports.size(), "port count",
            pass.name);
    abi_contract.fingerprint = contract.fingerprint;

    auto authored =
        RenderPass::descriptor<
            RenderPass::FullscreenImplementationV1>();
    authored.implementation_id_utf8 =
        authoredFullscreenImplementationIdV1;
    authored.implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(authoredFullscreenImplementationIdV1) - 1);
    authored.vertex_shader_utf8 =
        pass.fullscreenInfo().vert_shader.ref.data();
    if (!validByteRange(
            authored.vertex_shader_utf8,
            abiSize(
                pass.fullscreenInfo().vert_shader.ref.size(),
                "authored vertex shader reference",
                pass.name),
            RenderPass::maximumShaderReferenceBytesV1)) {
        throw std::runtime_error(
            "pass implementation authored vertex shader reference is "
            "invalid for pass '" +
            pass.name + "'");
    }
    authored.vertex_shader_size = static_cast<std::uint32_t>(
        pass.fullscreenInfo().vert_shader.ref.size());
    authored.fragment_shader_utf8 =
        pass.fullscreenInfo().frag_shader.ref.data();
    if (!validByteRange(
            authored.fragment_shader_utf8,
            abiSize(
                pass.fullscreenInfo().frag_shader.ref.size(),
                "authored fragment shader reference",
                pass.name),
            RenderPass::maximumShaderReferenceBytesV1)) {
        throw std::runtime_error(
            "pass implementation authored fragment shader reference is "
            "invalid for pass '" +
            pass.name + "'");
    }
    authored.fragment_shader_size = static_cast<std::uint32_t>(
        pass.fullscreenInfo().frag_shader.ref.size());

    auto input =
        RenderPass::descriptor<
            RenderPass::ResolveFullscreenInputV1>();
    input.contract = &abi_contract;
    input.authored_implementation = &authored;
    auto output =
        RenderPass::descriptor<
            RenderPass::FullscreenImplementationV1>();
    const auto status = selected->resolve(
        selected->context, &input, &output);
    if (status != Status::ok) {
        throw std::runtime_error(
            "pass implementation provider '" + requested +
            "' failed for pass '" + pass.name +
            "' with status " + statusName(status));
    }
    validateOutputEnvelope(output, requested, pass.name);
    auto implementation = copyProviderString(
        output.implementation_id_utf8,
        output.implementation_id_size,
        RenderPass::maximumImplementationIdBytesV1,
        "implementation id", requested, pass.name);
    try {
        (void)parseSemanticTypeId(implementation);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "pass implementation provider '" + requested +
            "' returned invalid implementation id '" +
            implementation + "' for pass '" + pass.name +
            "': " + error.what());
    }
    auto vertex = copyProviderString(
        output.vertex_shader_utf8,
        output.vertex_shader_size,
        RenderPass::maximumShaderReferenceBytesV1,
        "vertex shader reference", requested, pass.name);
    auto fragment = copyProviderString(
        output.fragment_shader_utf8,
        output.fragment_shader_size,
        RenderPass::maximumShaderReferenceBytesV1,
        "fragment shader reference", requested, pass.name);

    const auto slot_index = static_cast<std::uint64_t>(
        selected - registry.providers.data());
    return ResolvedFullscreenPassImplementation{
        .vertex_shader =
            makeShaderReference(
                std::move(vertex), ShaderStage::vertex),
        .fragment_shader =
            makeShaderReference(
                std::move(fragment),
                ShaderStage::fragment),
        .selection =
            PassImplementationSelection{
                .provider = requested,
                .implementation =
                    std::move(implementation),
                .contract = contract.id,
                .contract_fingerprint =
                    contract.fingerprint,
                .provider_owner = selected->owner,
                .provider_identity = slot_index + 1,
                .provider_generation =
                    selected->generation,
                .provider_version =
                    selected->provider_version,
                .provider_capability_bits =
                    selected->capability_bits,
                .explicitly_selected =
                    pass.requested_implementation_provider
                        .has_value(),
            },
    };
}

PassImplementationRegistry::PassImplementationRegistry()
    : impl_{
          std::make_unique<
              PassImplementationRegistryState>()} {
    impl_->registerBuiltinProvider();
}

PassImplementationRegistry::~PassImplementationRegistry() = default;

RenderPass::Status
PassImplementationRegistry::registerProvider(
    const RenderPass::ProviderV1 &provider,
    internal::RegistrationOwner owner,
    RenderPass::ProviderHandleV1 &out_handle) noexcept {
    return impl_->registerProvider(provider, owner, out_handle);
}

RenderPass::Status
PassImplementationRegistry::unregisterProvider(
    RenderPass::ProviderHandleV1 handle,
    internal::RegistrationOwner owner) noexcept {
    return impl_->unregisterProvider(handle, owner);
}

PassImplementationRegistrySnapshot
PassImplementationRegistry::snapshot() const {
    std::shared_lock lock{impl_->mutex};
    return PassImplementationRegistrySnapshot{
        std::make_unique<
            PassImplementationRegistrySnapshot::Impl>(
            std::move(lock), impl_.get())};
}

std::vector<RenderProviderListing>
PassImplementationRegistry::providerListing() const {
    return impl_->listing();
}

void PassImplementationRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    impl_->activateOwner(owner);
}

void PassImplementationRegistry::releaseOwner(
    internal::RegistrationOwner owner) noexcept {
    impl_->releaseOwner(owner);
}

void resolveRenderingPassImplementations(
    RenderingPassDefinition &definition,
    const VulkanTargetPlan &target_plan,
    const PassImplementationRegistrySnapshot &providers) {
    if (target_plan.graph != definition.name ||
        target_plan.lowering_graph.name != definition.name) {
        throw std::runtime_error(
            "pass implementation target plan does not match rendering pass: " +
            definition.name);
    }
    std::map<std::string, const LogicalGraphNode *,
             std::less<>>
        nodes;
    for (const auto &node :
         target_plan.lowering_graph.nodes) {
        if (!nodes.emplace(
                 node.logical.name,
                 &node.logical)
                 .second) {
            throw std::runtime_error(
                "pass implementation target plan has duplicate logical node: " +
                node.logical.name);
        }
    }

    struct PendingResolution {
        std::size_t pass_index = 0;
        ResolvedFullscreenPassImplementation
            implementation;
    };
    std::vector<PendingResolution> pending;
    pending.reserve(definition.passes.size());
    for (std::size_t pass_index = 0;
         pass_index < definition.passes.size();
         ++pass_index) {
        const auto &pass =
            definition.passes[pass_index];
        if (!pass.isFullscreen()) {
            if (pass.requested_implementation_provider) {
                throw std::runtime_error(
                    "pass implementation providers currently support "
                    "fullscreen passes only: " +
                    pass.name);
            }
            continue;
        }
        const auto found = nodes.find(pass.name);
        if (found == nodes.end()) {
            throw std::runtime_error(
                "fullscreen pass implementation has no target-plan "
                "logical node: " +
                pass.name);
        }
        pending.push_back(PendingResolution{
            .pass_index = pass_index,
            .implementation =
                providers.resolveFullscreen(
                    pass, *found->second),
        });
    }
    for (auto &resolution : pending) {
        auto &pass =
            definition.passes[
                resolution.pass_index];
        auto &resolved =
            resolution.implementation;
        pass.fullscreenInfo().vert_shader =
            std::move(resolved.vertex_shader);
        pass.fullscreenInfo().frag_shader =
            std::move(resolved.fragment_shader);
        pass.implementation_selection =
            std::move(resolved.selection);
    }
}

PassImplementationRegistry &
passImplementationRegistry() {
    static auto *value =
        new PassImplementationRegistry;
    return *value;
}

namespace render_pass_internal {

void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        passImplementationRegistry()
            .activateOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        passImplementationRegistry()
            .releaseOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

} // namespace render_pass_internal

namespace {

Status registerProviderApi(
    void *context,
    const RenderPass::ProviderV1 *provider,
    RenderPass::ProviderHandleV1 *out_handle) noexcept {
    if (context != &api_context_token ||
        provider == nullptr || out_handle == nullptr) {
        return Status::invalid_argument;
    }
    const auto owner =
        internal::currentRegistrationOwner();
    if (owner == internal::engineRegistrationOwner) {
        return Status::wrong_owner;
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        return Status::stale_owner;
    }
    try {
        return passImplementationRegistry()
            .registerProvider(
                *provider, owner, *out_handle);
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

Status unregisterProviderApi(
    void *context,
    RenderPass::ProviderHandleV1 handle) noexcept {
    if (context != &api_context_token) {
        return Status::invalid_argument;
    }
    const auto owner =
        internal::currentRegistrationOwner();
    if (owner == internal::engineRegistrationOwner) {
        return Status::wrong_owner;
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        return Status::stale_owner;
    }
    try {
        return passImplementationRegistry()
            .unregisterProvider(handle, owner);
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace

namespace RenderPass {

Status getApiV1(
    std::uint32_t client_abi_version,
    ApiV1 *out_api) noexcept {
    if (out_api == nullptr ||
        out_api->struct_size < sizeof(ApiV1)) {
        return Status::invalid_argument;
    }
    if (out_api->version != descriptorVersionV1) {
        return Status::unsupported_version;
    }
    if (out_api->reserved0 != 0 ||
        out_api->reserved1 != 0) {
        return Status::reserved_not_zero;
    }
    if (client_abi_version != abiVersionV1) {
        return Status::unsupported_version;
    }
    try {
        (void)passImplementationRegistry();
        auto produced = descriptor<ApiV1>();
        produced.capability_bits =
            api_provider_registration;
        produced.context = &api_context_token;
        produced.register_provider =
            registerProviderApi;
        produced.unregister_provider =
            unregisterProviderApi;
        *out_api = produced;
        return Status::ok;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace RenderPass

} // namespace Pelican
