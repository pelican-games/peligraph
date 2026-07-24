#include "passimplementationregistry.hpp"

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

std::uint32_t nextGeneration(std::uint32_t generation) noexcept {
    if (++generation == 0) ++generation;
    return generation;
}

bool validHandle(RenderPass::ProviderHandleV1 handle) noexcept {
    return handle.identity != 0 && handle.generation != 0 &&
           handle.reserved == 0;
}

bool validByteRange(const char *data, std::uint32_t size,
                    std::uint32_t maximum) noexcept {
    return data != nullptr && size != 0 && size <= maximum &&
           std::find(data, data + size, '\0') == data + size;
}

bool validProvider(const RenderPass::ProviderV1 &provider) noexcept {
    if (provider.struct_size < sizeof(RenderPass::ProviderV1) ||
        provider.version != RenderPass::descriptorVersionV1 ||
        provider.reserved0 != 0 || provider.reserved1 != 0 ||
        provider.reserved2 != 0 ||
        provider.provider_version != RenderPass::providerVersionV1 ||
        provider.minimum_engine_provider_version >
            RenderPass::providerVersionV1 ||
        provider.capability_bits !=
            RenderPass::builtinProviderCapabilitiesV1 ||
        provider.resolve_fullscreen == nullptr) {
        return false;
    }
    return validByteRange(
        provider.name_utf8, provider.name_size,
        RenderPass::maximumProviderNameBytesV1);
}

std::uint32_t abiSize(
    std::size_t size, std::string_view field,
    std::string_view pass) {
    if (size >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "pass implementation " +
            std::string{field} +
            " exceeds the v1 ABI size limit for pass '" +
            std::string{pass} + "'");
    }
    return static_cast<std::uint32_t>(size);
}

const char *statusName(Status status) noexcept {
    switch (status) {
    case Status::ok: return "ok";
    case Status::invalid_argument: return "invalid_argument";
    case Status::unsupported_version: return "unsupported_version";
    case Status::reserved_not_zero: return "reserved_not_zero";
    case Status::duplicate_provider: return "duplicate_provider";
    case Status::stale_provider: return "stale_provider";
    case Status::wrong_owner: return "wrong_owner";
    case Status::stale_owner: return "stale_owner";
    case Status::provider_error: return "provider_error";
    case Status::out_of_memory: return "out_of_memory";
    case Status::unavailable: return "unavailable";
    }
    return "unknown_status";
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

class StableFingerprint {
    std::uint64_t value_ = 14695981039346656037ULL;

  public:
    void appendByte(std::uint8_t byte) noexcept {
        value_ ^= byte;
        value_ *= 1099511628211ULL;
    }

    void appendUnsigned(std::uint64_t value) noexcept {
        for (std::uint32_t shift = 0; shift < 64; shift += 8) {
            appendByte(static_cast<std::uint8_t>(value >> shift));
        }
    }

    void appendString(std::string_view value) noexcept {
        appendUnsigned(value.size());
        for (const auto byte : value) {
            appendByte(static_cast<std::uint8_t>(
                static_cast<unsigned char>(byte)));
        }
    }

    std::uint64_t value() const noexcept {
        return value_;
    }
};

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

struct PassImplementationRegistryState {
    struct ProviderSlot {
        std::uint32_t generation = 0;
        bool active = false;
        internal::RegistrationOwner owner =
            internal::engineRegistrationOwner;
        std::uint32_t provider_version = 0;
        std::uint64_t capability_bits = 0;
        std::string name;
        void *context = nullptr;
        RenderPass::ResolveFullscreenV1Fn resolve_fullscreen = nullptr;
    };

    mutable std::shared_mutex mutex;
    std::vector<ProviderSlot> providers;
    std::vector<std::uint32_t> free_slots;
    std::unordered_map<std::uint32_t, std::uint32_t>
        retired_owner_generations;
    internal::RegistrationOwner active_game_owner =
        internal::engineRegistrationOwner;

    bool ownerIsRetired(
        internal::RegistrationOwner owner) const noexcept {
        const auto found = retired_owner_generations.find(
            internal::registrationOwnerIdentity(owner));
        return found != retired_owner_generations.end() &&
               found->second ==
                   internal::registrationOwnerGeneration(owner);
    }

    void retireOwner(internal::RegistrationOwner owner) {
        retired_owner_generations.insert_or_assign(
            internal::registrationOwnerIdentity(owner),
            internal::registrationOwnerGeneration(owner));
    }
};

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

    const auto find_for_owner =
        [&](internal::RegistrationOwner owner)
        -> const PassImplementationRegistryState::ProviderSlot * {
        const auto found = std::find_if(
            registry.providers.begin(),
            registry.providers.end(),
            [&](const auto &slot) {
                return slot.active && slot.owner == owner &&
                       slot.name == requested;
            });
        return found == registry.providers.end()
                   ? nullptr
                   : &*found;
    };
    const PassImplementationRegistryState::ProviderSlot *selected =
        nullptr;
    if (registry.active_game_owner !=
        internal::engineRegistrationOwner) {
        selected = find_for_owner(
            registry.active_game_owner);
    }
    if (selected == nullptr) {
        selected = find_for_owner(
            internal::engineRegistrationOwner);
    }
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
    const auto status = selected->resolve_fullscreen(
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
    auto provider =
        RenderPass::descriptor<RenderPass::ProviderV1>();
    provider.capability_bits =
        RenderPass::builtinProviderCapabilitiesV1;
    provider.name_utf8 =
        builtinFullscreenPassImplementationProvider.data();
    provider.name_size = static_cast<std::uint32_t>(
        builtinFullscreenPassImplementationProvider.size());
    provider.resolve_fullscreen =
        builtinResolveFullscreen;
    RenderPass::ProviderHandleV1 handle{};
    if (registerProvider(
            provider,
            internal::engineRegistrationOwner,
            handle) != Status::ok) {
        throw std::logic_error(
            "failed to register builtin fullscreen pass implementation "
            "provider");
    }
}

PassImplementationRegistry::~PassImplementationRegistry() = default;

RenderPass::Status
PassImplementationRegistry::registerProvider(
    const RenderPass::ProviderV1 &provider,
    internal::RegistrationOwner owner,
    RenderPass::ProviderHandleV1 &out_handle) noexcept {
    out_handle = {};
    if (!validProvider(provider)) {
        return Status::invalid_argument;
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        return Status::stale_owner;
    }
    try {
        std::unique_lock lock{impl_->mutex};
        if (impl_->ownerIsRetired(owner)) {
            return Status::stale_owner;
        }
        const std::string_view name{
            provider.name_utf8, provider.name_size};
        if (std::any_of(
                impl_->providers.begin(),
                impl_->providers.end(),
                [&](const auto &registered) {
                    return registered.active &&
                           registered.owner == owner &&
                           registered.name == name;
                })) {
            return Status::duplicate_provider;
        }

        std::uint32_t slot_index = 0;
        if (!impl_->free_slots.empty()) {
            slot_index = impl_->free_slots.back();
            impl_->free_slots.pop_back();
        } else {
            if (impl_->providers.size() >=
                std::numeric_limits<std::uint32_t>::max()) {
                return Status::out_of_memory;
            }
            impl_->providers.emplace_back();
            slot_index = static_cast<std::uint32_t>(
                impl_->providers.size() - 1);
        }

        auto &slot = impl_->providers[slot_index];
        slot.generation =
            nextGeneration(slot.generation);
        slot.active = true;
        slot.owner = owner;
        slot.provider_version =
            provider.provider_version;
        slot.capability_bits =
            provider.capability_bits;
        slot.name.assign(
            provider.name_utf8, provider.name_size);
        slot.context = provider.context;
        slot.resolve_fullscreen =
            provider.resolve_fullscreen;
        out_handle = RenderPass::ProviderHandleV1{
            .identity =
                static_cast<std::uint64_t>(slot_index) + 1,
            .generation = slot.generation,
        };
        return Status::ok;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

RenderPass::Status
PassImplementationRegistry::unregisterProvider(
    RenderPass::ProviderHandleV1 handle,
    internal::RegistrationOwner owner) noexcept {
    if (!validHandle(handle)) {
        return Status::invalid_argument;
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        return Status::stale_owner;
    }
    std::unique_lock lock{impl_->mutex};
    if (handle.identity > impl_->providers.size()) {
        return Status::stale_provider;
    }
    const auto index = static_cast<std::uint32_t>(
        handle.identity - 1);
    auto &slot = impl_->providers[index];
    if (!slot.active ||
        slot.generation != handle.generation) {
        return Status::stale_provider;
    }
    if (slot.owner != owner) {
        return Status::wrong_owner;
    }
    slot.active = false;
    slot.owner = internal::engineRegistrationOwner;
    slot.provider_version = 0;
    slot.capability_bits = 0;
    slot.name.clear();
    slot.context = nullptr;
    slot.resolve_fullscreen = nullptr;
    impl_->free_slots.push_back(index);
    return Status::ok;
}

PassImplementationRegistrySnapshot
PassImplementationRegistry::snapshot() const {
    std::shared_lock lock{impl_->mutex};
    return PassImplementationRegistrySnapshot{
        std::make_unique<
            PassImplementationRegistrySnapshot::Impl>(
            std::move(lock), impl_.get())};
}

void PassImplementationRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    std::unique_lock lock{impl_->mutex};
    impl_->active_game_owner =
        !impl_->ownerIsRetired(owner) &&
                internal::isRegistrationOwnerCurrent(owner)
            ? owner
            : internal::engineRegistrationOwner;
}

void PassImplementationRegistry::releaseOwner(
    internal::RegistrationOwner owner) noexcept {
    if (owner == internal::engineRegistrationOwner) {
        return;
    }
    std::unique_lock lock{impl_->mutex};
    impl_->retireOwner(owner);
    if (impl_->active_game_owner == owner) {
        impl_->active_game_owner =
            internal::engineRegistrationOwner;
    }
    for (std::uint32_t index = 0;
         index < impl_->providers.size(); ++index) {
        auto &slot = impl_->providers[index];
        if (!slot.active || slot.owner != owner) {
            continue;
        }
        slot.active = false;
        slot.owner =
            internal::engineRegistrationOwner;
        slot.provider_version = 0;
        slot.capability_bits = 0;
        slot.name.clear();
        slot.context = nullptr;
        slot.resolve_fullscreen = nullptr;
        impl_->free_slots.push_back(index);
    }
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
