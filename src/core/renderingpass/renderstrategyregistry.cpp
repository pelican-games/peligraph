#include "renderstrategyregistry.hpp"

#include "../../project/logicalrendertype.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

using RenderStrategy::Status;

constexpr char authoredImplementationIdV1[] =
    "pelican.render.strategy.authored_config@1";

std::uint32_t nextGeneration(
    std::uint32_t generation) noexcept {
    if (++generation == 0) ++generation;
    return generation;
}

bool validHandle(
    RenderStrategy::ProviderHandleV1 handle) noexcept {
    return handle.identity != 0 &&
           handle.generation != 0 &&
           handle.reserved == 0;
}

bool validByteRange(
    const char *data, std::uint32_t size,
    std::uint32_t maximum) noexcept {
    return data != nullptr && size != 0 &&
           size <= maximum &&
           std::find(data, data + size, '\0') ==
               data + size;
}

bool validProvider(
    const RenderStrategy::ProviderV1
        &provider) noexcept {
    if (provider.struct_size <
            sizeof(RenderStrategy::ProviderV1) ||
        provider.version !=
            RenderStrategy::descriptorVersionV1 ||
        provider.reserved0 != 0 ||
        provider.reserved1 != 0 ||
        provider.reserved2 != 0 ||
        provider.provider_version !=
            RenderStrategy::providerVersionV1 ||
        provider.minimum_engine_provider_version >
            RenderStrategy::providerVersionV1 ||
        provider.capability_bits !=
            RenderStrategy::builtinProviderCapabilitiesV1 ||
        provider.resolve_render_strategy == nullptr) {
        return false;
    }
    return validByteRange(
        provider.name_utf8, provider.name_size,
        RenderStrategy::maximumProviderNameBytesV1);
}

std::uint32_t abiSize(
    std::size_t size, std::string_view field,
    std::string_view strategy) {
    if (size >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "render strategy " + std::string{field} +
            " exceeds the v1 ABI size limit for '" +
            std::string{strategy} + "'");
    }
    return static_cast<std::uint32_t>(size);
}

const char *statusName(Status status) noexcept {
    switch (status) {
    case Status::ok: return "ok";
    case Status::invalid_argument:
        return "invalid_argument";
    case Status::unsupported_version:
        return "unsupported_version";
    case Status::reserved_not_zero:
        return "reserved_not_zero";
    case Status::duplicate_provider:
        return "duplicate_provider";
    case Status::stale_provider:
        return "stale_provider";
    case Status::wrong_owner: return "wrong_owner";
    case Status::stale_owner: return "stale_owner";
    case Status::provider_error:
        return "provider_error";
    case Status::out_of_memory:
        return "out_of_memory";
    case Status::unavailable: return "unavailable";
    }
    return "unknown_status";
}

bool validGraphVariantPolicyEnvelope(
    const RenderStrategy::GraphVariantPolicyV1
        &policy) noexcept {
    return policy.struct_size >=
               sizeof(RenderStrategy::GraphVariantPolicyV1) &&
           policy.version ==
               RenderStrategy::descriptorVersionV1 &&
           policy.reserved0 == 0 &&
           policy.reserved1 == 0 &&
           policy.reserved2 == 0 &&
           policy.reserved3 == 0 &&
           policy.reserved4 == 0;
}

bool validFacadeContract(
    const RenderStrategy::RendererFacadeContractV1
        &contract) noexcept {
    return contract.struct_size >=
               sizeof(
                   RenderStrategy::RendererFacadeContractV1) &&
           contract.version ==
               RenderStrategy::descriptorVersionV1 &&
           contract.reserved0 == 0 &&
           contract.reserved1 == 0 &&
           contract.reserved2 == 0 &&
           contract.reserved3 == 0 &&
           contract.reserved4 == 0 &&
           contract.reserved5 == 0 &&
           contract.capability_bits ==
               RenderStrategy::builtinFacadeCapabilitiesV1 &&
           contract.runtime_shader_compiler_enabled <= 1 &&
           validByteRange(
               contract.contract_id_utf8,
               contract.contract_id_size,
               sizeof(
                   RenderStrategy::rendererFacadeContractIdV1) -
                   1) &&
           contract.contract_id_size ==
               sizeof(
                   RenderStrategy::rendererFacadeContractIdV1) -
                   1 &&
           std::string_view{
               contract.contract_id_utf8,
               contract.contract_id_size} ==
               RenderStrategy::rendererFacadeContractIdV1 &&
           validByteRange(
               contract.output_contract_id_utf8,
               contract.output_contract_id_size,
               sizeof(
                   RenderStrategy::authoredConfigContractIdV1) -
                   1) &&
           contract.output_contract_id_size ==
               sizeof(
                   RenderStrategy::authoredConfigContractIdV1) -
                   1 &&
           std::string_view{
               contract.output_contract_id_utf8,
               contract.output_contract_id_size} ==
               RenderStrategy::authoredConfigContractIdV1 &&
           validGraphVariantPolicyEnvelope(
               contract.graph_variant_policy);
}

Status builtinResolveRenderStrategy(
    void *,
    const RenderStrategy::ResolveRenderStrategyInputV1
        *input,
    RenderStrategy::RenderStrategyOutputV1
        *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size <
            sizeof(RenderStrategy::
                       ResolveRenderStrategyInputV1) ||
        input->version !=
            RenderStrategy::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->reserved3 != 0 ||
        input->reserved4 != 0 ||
        input->contract == nullptr ||
        !validFacadeContract(*input->contract) ||
        !validByteRange(
            input->strategy_name_utf8,
            input->strategy_name_size,
            RenderStrategy::maximumStrategyNameBytesV1) ||
        !validByteRange(
            input->parameters_json_utf8,
            input->parameters_json_size,
            RenderStrategy::maximumParametersJsonBytesV1) ||
        !validByteRange(
            input->seed_config_json_utf8,
            input->seed_config_json_size,
            RenderStrategy::maximumConfigJsonBytesV1) ||
        input->seed_config_fingerprint == 0 ||
        output->struct_size <
            sizeof(RenderStrategy::RenderStrategyOutputV1) ||
        output->version !=
            RenderStrategy::descriptorVersionV1 ||
        output->reserved0 != 0 ||
        output->reserved1 != 0 ||
        output->reserved2 != 0 ||
        output->reserved3 != 0) {
        return Status::invalid_argument;
    }
    *output =
        RenderStrategy::descriptor<
            RenderStrategy::RenderStrategyOutputV1>();
    output->implementation_id_utf8 =
        authoredImplementationIdV1;
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(authoredImplementationIdV1) - 1);
    output->config_json_utf8 =
        input->seed_config_json_utf8;
    output->config_json_size =
        input->seed_config_json_size;
    return Status::ok;
}

class StableFingerprint {
    std::uint64_t value_ =
        14695981039346656037ULL;

  public:
    void appendByte(std::uint8_t byte) noexcept {
        value_ ^= byte;
        value_ *= 1099511628211ULL;
    }

    void appendUnsigned(
        std::uint64_t value) noexcept {
        for (std::uint32_t shift = 0;
             shift < 64; shift += 8) {
            appendByte(
                static_cast<std::uint8_t>(
                    value >> shift));
        }
    }

    void appendString(
        std::string_view value) noexcept {
        appendUnsigned(value.size());
        for (const auto byte : value) {
            appendByte(
                static_cast<std::uint8_t>(
                    static_cast<unsigned char>(byte)));
        }
    }

    std::uint64_t value() const noexcept {
        return value_;
    }
};

std::string canonicalJson(
    const nlohmann::json &value) {
    return nlohmann::ordered_json(value).dump();
}

std::uint64_t configFingerprint(
    std::string_view canonical_json) noexcept {
    StableFingerprint fingerprint;
    fingerprint.appendString(canonical_json);
    return fingerprint.value();
}

RenderStrategy::GraphVariantV1 graphVariant(
    RenderPipelineGraphVariant value) {
    switch (value) {
    case RenderPipelineGraphVariant::flat:
        return RenderStrategy::GraphVariantV1::flat;
    case RenderPipelineGraphVariant::preview:
        return RenderStrategy::GraphVariantV1::preview;
    case RenderPipelineGraphVariant::xr:
        return RenderStrategy::GraphVariantV1::xr;
    }
    throw std::runtime_error(
        "unknown graph variant in render strategy contract");
}

RenderStrategy::HistoryPolicyV1 historyPolicy(
    GraphVariantHistoryPolicy value) {
    switch (value) {
    case GraphVariantHistoryPolicy::preserve:
        return RenderStrategy::HistoryPolicyV1::preserve;
    case GraphVariantHistoryPolicy::forbid:
        return RenderStrategy::HistoryPolicyV1::forbid;
    }
    throw std::runtime_error(
        "unknown history policy in render strategy contract");
}

RenderStrategy::ProjectionJitterPolicyV1
projectionJitterPolicy(
    GraphVariantProjectionJitterPolicy value) {
    switch (value) {
    case GraphVariantProjectionJitterPolicy::preserve:
        return RenderStrategy::ProjectionJitterPolicyV1::
            preserve;
    case GraphVariantProjectionJitterPolicy::forbid:
        return RenderStrategy::ProjectionJitterPolicyV1::
            forbid;
    }
    throw std::runtime_error(
        "unknown projection jitter policy in render strategy "
        "contract");
}

RenderStrategy::ViewFamilyV1 viewFamily(
    GraphVariantViewFamily value) {
    switch (value) {
    case GraphVariantViewFamily::caller_defined:
        return RenderStrategy::ViewFamilyV1::caller_defined;
    case GraphVariantViewFamily::mono:
        return RenderStrategy::ViewFamilyV1::mono;
    case GraphVariantViewFamily::stereo:
        return RenderStrategy::ViewFamilyV1::stereo;
    }
    throw std::runtime_error(
        "unknown view family in render strategy contract");
}

RenderStrategy::ViewExecutionV1 viewExecution(
    GraphVariantViewExecution value) {
    switch (value) {
    case GraphVariantViewExecution::caller_defined:
        return RenderStrategy::ViewExecutionV1::
            caller_defined;
    case GraphVariantViewExecution::single_view:
        return RenderStrategy::ViewExecutionV1::single_view;
    case GraphVariantViewExecution::sequential:
        return RenderStrategy::ViewExecutionV1::sequential;
    }
    throw std::runtime_error(
        "unknown view execution in render strategy contract");
}

RenderStrategy::ResourceLayoutV1 resourceLayout(
    GraphVariantResourceLayout value) {
    switch (value) {
    case GraphVariantResourceLayout::shared_2d:
        return RenderStrategy::ResourceLayoutV1::shared_2d;
    case GraphVariantResourceLayout::sequential_2d:
        return RenderStrategy::ResourceLayoutV1::sequential_2d;
    }
    throw std::runtime_error(
        "unknown resource layout in render strategy contract");
}

RenderStrategy::TerminalV1 terminal(
    GraphVariantTerminal value) {
    switch (value) {
    case GraphVariantTerminal::presentation:
        return RenderStrategy::TerminalV1::presentation;
    case GraphVariantTerminal::request_local_capture:
        return RenderStrategy::TerminalV1::
            request_local_capture;
    case GraphVariantTerminal::external_view:
        return RenderStrategy::TerminalV1::external_view;
    }
    throw std::runtime_error(
        "unknown terminal in render strategy contract");
}

RenderStrategy::MirrorOutputV1 mirrorOutput(
    GraphVariantMirrorOutput value) {
    switch (value) {
    case GraphVariantMirrorOutput::none:
        return RenderStrategy::MirrorOutputV1::none;
    case GraphVariantMirrorOutput::left_eye:
        return RenderStrategy::MirrorOutputV1::left_eye;
    }
    throw std::runtime_error(
        "unknown mirror output in render strategy contract");
}

RenderStrategy::GraphVariantPolicyV1
makeGraphVariantPolicy(
    const CompiledGraphVariantPolicy &policy) {
    auto result =
        RenderStrategy::descriptor<
            RenderStrategy::GraphVariantPolicyV1>();
    result.variant = graphVariant(policy.variant);
    result.history = historyPolicy(policy.history);
    result.projection_jitter =
        projectionJitterPolicy(policy.projection_jitter);
    result.view_family = viewFamily(policy.view_family);
    result.view_execution =
        viewExecution(policy.view_execution);
    result.resource_layout =
        resourceLayout(policy.resource_layout);
    result.terminal = terminal(policy.terminal);
    result.mirror_output =
        mirrorOutput(policy.mirror_output);
    result.view_count = policy.view_count;
    return result;
}

std::string copyProviderString(
    const char *data, std::uint32_t size,
    std::uint32_t maximum, std::string_view field,
    std::string_view provider,
    std::string_view strategy) {
    if (!validByteRange(data, size, maximum)) {
        throw std::runtime_error(
            "render strategy provider '" +
            std::string{provider} +
            "' returned invalid " + std::string{field} +
            " for '" + std::string{strategy} + "'");
    }
    return std::string{data, size};
}

void validateOutputEnvelope(
    const RenderStrategy::RenderStrategyOutputV1
        &output,
    std::string_view provider,
    std::string_view strategy) {
    if (output.struct_size <
            sizeof(RenderStrategy::RenderStrategyOutputV1) ||
        output.version !=
            RenderStrategy::descriptorVersionV1 ||
        output.reserved0 != 0 ||
        output.reserved1 != 0 ||
        output.reserved2 != 0 ||
        output.reserved3 != 0) {
        throw std::runtime_error(
            "render strategy provider '" +
            std::string{provider} +
            "' returned an invalid output envelope for '" +
            std::string{strategy} + "'");
    }
}

struct StrategyRequest {
    std::string name;
    std::optional<std::string> provider;
    nlohmann::json parameters =
        nlohmann::json::object();
};

std::optional<StrategyRequest> parseStrategyRequest(
    const nlohmann::json &config) {
    if (!config.contains("render_strategy")) {
        return std::nullopt;
    }
    const auto &request = config.at("render_strategy");
    if (!request.is_object()) {
        throw std::runtime_error(
            "render_strategy must be an object");
    }
    for (auto field = request.begin();
         field != request.end(); ++field) {
        if (field.key() != "name" &&
            field.key() != "provider" &&
            field.key() != "parameters") {
            throw std::runtime_error(
                "render_strategy has unknown key '" +
                field.key() + "'");
        }
    }
    if (!request.contains("name") ||
        !request.at("name").is_string()) {
        throw std::runtime_error(
            "render_strategy requires a name string");
    }
    auto name =
        request.at("name").get<std::string>();
    if (name.empty() ||
        name.find('\0') != std::string::npos ||
        name.size() >
            RenderStrategy::maximumStrategyNameBytesV1) {
        throw std::runtime_error(
            "render strategy name is empty or too long: " +
            name);
    }

    std::optional<std::string> provider;
    if (request.contains("provider")) {
        if (!request.at("provider").is_string()) {
            throw std::runtime_error(
                "render strategy provider must be a string: " +
                name);
        }
        provider =
            request.at("provider").get<std::string>();
        if (provider->empty() ||
            provider->find('\0') !=
                std::string::npos ||
            provider->size() >
                RenderStrategy::maximumProviderNameBytesV1) {
            throw std::runtime_error(
                "render strategy provider is empty or too "
                "long: " +
                name);
        }
    }

    auto parameters = nlohmann::json::object();
    if (request.contains("parameters")) {
        parameters = request.at("parameters");
        if (!parameters.is_object()) {
            throw std::runtime_error(
                "render strategy parameters must be an "
                "object: " +
                name);
        }
    }
    if (canonicalJson(parameters).size() >
        RenderStrategy::maximumParametersJsonBytesV1) {
        throw std::runtime_error(
            "render strategy parameters are too large: " +
            name);
    }
    return StrategyRequest{
        .name = std::move(name),
        .provider = std::move(provider),
        .parameters = std::move(parameters),
    };
}

int api_context_token = 0;

} // namespace

struct RenderStrategyRegistryState {
    struct ProviderSlot {
        std::uint32_t generation = 0;
        bool active = false;
        internal::RegistrationOwner owner =
            internal::engineRegistrationOwner;
        std::uint32_t provider_version = 0;
        std::uint64_t capability_bits = 0;
        std::string name;
        void *context = nullptr;
        RenderStrategy::ResolveRenderStrategyV1Fn
            resolve_render_strategy = nullptr;
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
        const auto found =
            retired_owner_generations.find(
                internal::registrationOwnerIdentity(owner));
        return found !=
                   retired_owner_generations.end() &&
               found->second ==
                   internal::registrationOwnerGeneration(owner);
    }

    void retireOwner(
        internal::RegistrationOwner owner) {
        retired_owner_generations.insert_or_assign(
            internal::registrationOwnerIdentity(owner),
            internal::registrationOwnerGeneration(owner));
    }
};

struct RenderStrategyRegistrySnapshot::Impl {
    std::shared_lock<std::shared_mutex> lock;
    const RenderStrategyRegistryState *registry = nullptr;

    Impl(
        std::shared_lock<std::shared_mutex>
            registry_lock,
        const RenderStrategyRegistryState
            *value) noexcept
        : lock{std::move(registry_lock)},
          registry{value} {}
};

RenderStrategyRegistrySnapshot::
    RenderStrategyRegistrySnapshot() noexcept = default;
RenderStrategyRegistrySnapshot::
    ~RenderStrategyRegistrySnapshot() = default;
RenderStrategyRegistrySnapshot::
    RenderStrategyRegistrySnapshot(
        RenderStrategyRegistrySnapshot &&) noexcept =
        default;
RenderStrategyRegistrySnapshot &
RenderStrategyRegistrySnapshot::operator=(
    RenderStrategyRegistrySnapshot &&) noexcept = default;

RenderStrategyRegistrySnapshot::
    RenderStrategyRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

ResolvedRenderStrategy
RenderStrategyRegistrySnapshot::resolveStrategy(
    std::string_view strategy_name,
    std::string_view parameters_json,
    std::string_view seed_config_json,
    std::uint64_t seed_config_fingerprint,
    const CompiledGraphVariantPolicy
        &graph_variant_policy,
    bool runtime_shader_compiler_enabled,
    const std::optional<std::string>
        &requested_provider) const {
    if (impl_ == nullptr ||
        impl_->registry == nullptr) {
        throw std::runtime_error(
            "render strategy registry snapshot is empty");
    }
    if (strategy_name.empty() ||
        strategy_name.find('\0') !=
            std::string_view::npos ||
        strategy_name.size() >
            RenderStrategy::maximumStrategyNameBytesV1 ||
        seed_config_fingerprint == 0) {
        throw std::runtime_error(
            "render strategy request envelope is invalid");
    }
    const auto require_range =
        [&](std::string_view value,
            std::uint32_t maximum,
            std::string_view field) {
            if (value.empty() ||
                value.size() > maximum) {
                throw std::runtime_error(
                    "render strategy " +
                    std::string{field} +
                    " is empty or too large for '" +
                    std::string{strategy_name} + "'");
            }
        };
    require_range(
        parameters_json,
        RenderStrategy::maximumParametersJsonBytesV1,
        "parameters JSON");
    require_range(
        seed_config_json,
        RenderStrategy::maximumConfigJsonBytesV1,
        "seed config JSON");

    const auto &registry = *impl_->registry;
    const auto requested =
        requested_provider.value_or(
            std::string{
                builtinAuthoredRenderStrategyProvider});
    if (requested.empty() ||
        requested.find('\0') != std::string::npos ||
        requested.size() >
            RenderStrategy::maximumProviderNameBytesV1) {
        throw std::runtime_error(
            "render strategy provider name is invalid for '" +
            std::string{strategy_name} + "'");
    }
    const auto find_for_owner =
        [&](internal::RegistrationOwner owner)
        -> const RenderStrategyRegistryState::
            ProviderSlot * {
        const auto found = std::find_if(
            registry.providers.begin(),
            registry.providers.end(),
            [&](const auto &slot) {
                return slot.active &&
                       slot.owner == owner &&
                       slot.name == requested;
            });
        return found == registry.providers.end()
                   ? nullptr
                   : &*found;
    };
    const RenderStrategyRegistryState::ProviderSlot
        *selected = nullptr;
    if (registry.active_game_owner !=
        internal::engineRegistrationOwner) {
        selected =
            find_for_owner(registry.active_game_owner);
    }
    if (selected == nullptr) {
        selected =
            find_for_owner(
                internal::engineRegistrationOwner);
    }
    if (selected == nullptr) {
        throw std::runtime_error(
            "render strategy provider '" + requested +
            "' is not registered for the active owner "
            "while compiling '" +
            std::string{strategy_name} + "'");
    }

    auto contract =
        RenderStrategy::descriptor<
            RenderStrategy::RendererFacadeContractV1>();
    contract.contract_id_utf8 =
        RenderStrategy::rendererFacadeContractIdV1;
    contract.contract_id_size =
        static_cast<std::uint32_t>(
            sizeof(
                RenderStrategy::rendererFacadeContractIdV1) -
            1);
    contract.output_contract_id_utf8 =
        RenderStrategy::authoredConfigContractIdV1;
    contract.output_contract_id_size =
        static_cast<std::uint32_t>(
            sizeof(
                RenderStrategy::authoredConfigContractIdV1) -
            1);
    contract.capability_bits =
        RenderStrategy::builtinFacadeCapabilitiesV1;
    contract.graph_variant_policy =
        makeGraphVariantPolicy(graph_variant_policy);
    contract.runtime_shader_compiler_enabled =
        runtime_shader_compiler_enabled ? 1U : 0U;

    auto input =
        RenderStrategy::descriptor<
            RenderStrategy::
                ResolveRenderStrategyInputV1>();
    input.contract = &contract;
    input.strategy_name_utf8 = strategy_name.data();
    input.strategy_name_size =
        abiSize(
            strategy_name.size(), "strategy name",
            strategy_name);
    input.parameters_json_utf8 =
        parameters_json.data();
    input.parameters_json_size =
        abiSize(
            parameters_json.size(), "parameters JSON",
            strategy_name);
    input.seed_config_json_utf8 =
        seed_config_json.data();
    input.seed_config_json_size =
        abiSize(
            seed_config_json.size(), "seed config JSON",
            strategy_name);
    input.seed_config_fingerprint =
        seed_config_fingerprint;

    auto output =
        RenderStrategy::descriptor<
            RenderStrategy::RenderStrategyOutputV1>();
    const auto status =
        selected->resolve_render_strategy(
            selected->context, &input, &output);
    if (status != Status::ok) {
        throw std::runtime_error(
            "render strategy provider '" + requested +
            "' failed for '" +
            std::string{strategy_name} +
            "' with status " + statusName(status));
    }
    validateOutputEnvelope(
        output, requested, strategy_name);
    auto implementation = copyProviderString(
        output.implementation_id_utf8,
        output.implementation_id_size,
        RenderStrategy::maximumImplementationIdBytesV1,
        "implementation id", requested, strategy_name);
    try {
        (void)parseSemanticTypeId(implementation);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "render strategy provider '" + requested +
            "' returned invalid implementation id '" +
            implementation + "' for '" +
            std::string{strategy_name} + "': " +
            error.what());
    }
    auto generated_config = copyProviderString(
        output.config_json_utf8,
        output.config_json_size,
        RenderStrategy::maximumConfigJsonBytesV1,
        "config JSON", requested, strategy_name);

    const auto slot_index =
        static_cast<std::uint64_t>(
            selected - registry.providers.data());
    return {
        .config_json = std::move(generated_config),
        .selection =
            RenderStrategySelection{
                .name = std::string{strategy_name},
                .provider = requested,
                .implementation =
                    std::move(implementation),
                .contract =
                    RenderStrategy::
                        rendererFacadeContractIdV1,
                .output_contract =
                    RenderStrategy::
                        authoredConfigContractIdV1,
                .graph_variant =
                    std::string{
                        renderPipelineGraphVariantName(
                            graph_variant_policy.variant)},
                .facade_capability_bits =
                    RenderStrategy::
                        builtinFacadeCapabilitiesV1,
                .input_config_fingerprint =
                    seed_config_fingerprint,
                .provider_owner = selected->owner,
                .provider_identity = slot_index + 1,
                .provider_generation =
                    selected->generation,
                .provider_version =
                    selected->provider_version,
                .provider_capability_bits =
                    selected->capability_bits,
                .explicitly_selected =
                    requested_provider.has_value(),
            },
    };
}

RenderStrategyRegistry::RenderStrategyRegistry()
    : impl_{
          std::make_unique<
              RenderStrategyRegistryState>()} {
    auto provider =
        RenderStrategy::descriptor<
            RenderStrategy::ProviderV1>();
    provider.capability_bits =
        RenderStrategy::builtinProviderCapabilitiesV1;
    provider.name_utf8 =
        builtinAuthoredRenderStrategyProvider.data();
    provider.name_size =
        static_cast<std::uint32_t>(
            builtinAuthoredRenderStrategyProvider.size());
    provider.resolve_render_strategy =
        builtinResolveRenderStrategy;
    RenderStrategy::ProviderHandleV1 handle{};
    if (registerProvider(
            provider,
            internal::engineRegistrationOwner,
            handle) != Status::ok) {
        throw std::logic_error(
            "failed to register builtin render strategy "
            "provider");
    }
}

RenderStrategyRegistry::~RenderStrategyRegistry() =
    default;

RenderStrategy::Status
RenderStrategyRegistry::registerProvider(
    const RenderStrategy::ProviderV1 &provider,
    internal::RegistrationOwner owner,
    RenderStrategy::ProviderHandleV1
        &out_handle) noexcept {
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
            provider.name_utf8,
            provider.name_size};
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
                std::numeric_limits<
                    std::uint32_t>::max()) {
                return Status::out_of_memory;
            }
            impl_->providers.emplace_back();
            slot_index =
                static_cast<std::uint32_t>(
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
        slot.resolve_render_strategy =
            provider.resolve_render_strategy;
        out_handle =
            RenderStrategy::ProviderHandleV1{
                .identity =
                    static_cast<std::uint64_t>(
                        slot_index) +
                    1,
                .generation = slot.generation,
            };
        return Status::ok;
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

RenderStrategy::Status
RenderStrategyRegistry::unregisterProvider(
    RenderStrategy::ProviderHandleV1 handle,
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
    const auto index =
        static_cast<std::uint32_t>(
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
    slot.owner =
        internal::engineRegistrationOwner;
    slot.provider_version = 0;
    slot.capability_bits = 0;
    slot.name.clear();
    slot.context = nullptr;
    slot.resolve_render_strategy = nullptr;
    impl_->free_slots.push_back(index);
    return Status::ok;
}

RenderStrategyRegistrySnapshot
RenderStrategyRegistry::snapshot() const {
    std::shared_lock lock{impl_->mutex};
    return RenderStrategyRegistrySnapshot{
        std::make_unique<
            RenderStrategyRegistrySnapshot::Impl>(
            std::move(lock), impl_.get())};
}

void RenderStrategyRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    std::unique_lock lock{impl_->mutex};
    impl_->active_game_owner =
        !impl_->ownerIsRetired(owner) &&
                internal::isRegistrationOwnerCurrent(owner)
            ? owner
            : internal::engineRegistrationOwner;
}

void RenderStrategyRegistry::releaseOwner(
    internal::RegistrationOwner owner) noexcept {
    if (owner ==
        internal::engineRegistrationOwner) {
        return;
    }
    std::unique_lock lock{impl_->mutex};
    impl_->retireOwner(owner);
    if (impl_->active_game_owner == owner) {
        impl_->active_game_owner =
            internal::engineRegistrationOwner;
    }
    for (std::uint32_t index = 0;
         index < impl_->providers.size();
         ++index) {
        auto &slot = impl_->providers[index];
        if (!slot.active ||
            slot.owner != owner) {
            continue;
        }
        slot.active = false;
        slot.owner =
            internal::engineRegistrationOwner;
        slot.provider_version = 0;
        slot.capability_bits = 0;
        slot.name.clear();
        slot.context = nullptr;
        slot.resolve_render_strategy = nullptr;
        impl_->free_slots.push_back(index);
    }
}

ResolvedRenderStrategyConfig resolveRenderStrategy(
    const nlohmann::json &config,
    const CompiledGraphVariantPolicy
        &graph_variant_policy,
    bool runtime_shader_compiler_enabled,
    const RenderStrategyRegistrySnapshot &providers) {
    if (!config.is_object()) {
        throw std::runtime_error(
            "render strategy config must be an object");
    }
    const auto request =
        parseStrategyRequest(config);
    if (!request) {
        return {.config = config};
    }

    auto seed = config;
    seed.erase("render_strategy");
    // Target/physical compiler controls belong to lower layers. They must
    // survive a renderer-wide strategy expansion, but including them in the
    // strategy ABI seed would make an ejected physical-plan fingerprint
    // self-referential when the pin is pasted back into the same config.
    std::vector<std::pair<std::string, nlohmann::json>>
        lower_layer_controls;
    for (const auto *field :
         {"target_planning", "vulkan_plan_pins"}) {
        if (!seed.contains(field)) continue;
        lower_layer_controls.emplace_back(
            field, seed.at(field));
        seed.erase(field);
    }
    const auto parameters_json =
        canonicalJson(request->parameters);
    const auto seed_json = canonicalJson(seed);
    if (seed_json.size() >
        RenderStrategy::maximumConfigJsonBytesV1) {
        throw std::runtime_error(
            "render strategy seed config is too large: " +
            request->name);
    }
    const auto input_fingerprint =
        configFingerprint(seed_json);
    auto resolved = providers.resolveStrategy(
        request->name, parameters_json, seed_json,
        input_fingerprint, graph_variant_policy,
        runtime_shader_compiler_enabled,
        request->provider);

    nlohmann::json candidate;
    try {
        candidate =
            nlohmann::json::parse(resolved.config_json);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "render strategy provider '" +
            resolved.selection.provider +
            "' returned malformed JSON for '" +
            request->name + "': " + error.what());
    }
    if (!candidate.is_object()) {
        throw std::runtime_error(
            "render strategy '" + request->name +
            "' returned a non-object config");
    }
    if (candidate.contains("render_strategy")) {
        throw std::runtime_error(
            "render strategy '" + request->name +
            "' reintroduced reserved render_strategy "
            "control data");
    }
    if (candidate.contains("pipeline")) {
        throw std::runtime_error(
            "render strategy '" + request->name +
            "' reintroduced reserved pipeline preset "
            "control data");
    }
    for (const auto &control :
         lower_layer_controls) {
        const auto &field = control.first;
        if (candidate.contains(field)) {
            throw std::runtime_error(
                "render strategy '" + request->name +
                "' reintroduced reserved lower-layer control data: " +
                field);
        }
    }
    const auto canonical_candidate =
        canonicalJson(candidate);
    if (canonical_candidate.size() >
        RenderStrategy::maximumConfigJsonBytesV1) {
        throw std::runtime_error(
            "render strategy generated config is too large: " +
            request->name);
    }
    resolved.selection.output_config_fingerprint =
        configFingerprint(canonical_candidate);
    for (auto &[field, value] :
         lower_layer_controls) {
        candidate[field] = std::move(value);
    }
    return {
        .config = std::move(candidate),
        .selection =
            std::move(resolved.selection),
    };
}

void applyResolvedRenderStrategySelection(
    std::span<FrameGraphDefinition> frame_graphs,
    const std::optional<RenderStrategySelection>
        &selection) {
    if (!selection) return;
    for (auto &frame_graph : frame_graphs) {
        if (frame_graph.render_strategy) {
            throw std::runtime_error(
                "resolved render strategy provenance is "
                "already populated: " +
                frame_graph.name);
        }
        frame_graph.render_strategy = selection;
    }
}

RenderStrategyRegistry &renderStrategyRegistry() {
    static auto *value =
        new RenderStrategyRegistry;
    return *value;
}

namespace render_strategy_internal {

void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        renderStrategyRegistry().activateOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        renderStrategyRegistry().releaseOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

} // namespace render_strategy_internal

namespace {

Status registerProviderApi(
    void *context,
    const RenderStrategy::ProviderV1 *provider,
    RenderStrategy::ProviderHandleV1
        *out_handle) noexcept {
    if (context != &api_context_token ||
        provider == nullptr ||
        out_handle == nullptr) {
        return Status::invalid_argument;
    }
    const auto owner =
        internal::currentRegistrationOwner();
    if (owner ==
        internal::engineRegistrationOwner) {
        return Status::wrong_owner;
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        return Status::stale_owner;
    }
    try {
        return renderStrategyRegistry().registerProvider(
            *provider, owner, *out_handle);
    } catch (const std::bad_alloc &) {
        return Status::out_of_memory;
    } catch (...) {
        return Status::provider_error;
    }
}

Status unregisterProviderApi(
    void *context,
    RenderStrategy::ProviderHandleV1
        handle) noexcept {
    if (context != &api_context_token) {
        return Status::invalid_argument;
    }
    const auto owner =
        internal::currentRegistrationOwner();
    if (owner ==
        internal::engineRegistrationOwner) {
        return Status::wrong_owner;
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        return Status::stale_owner;
    }
    try {
        return renderStrategyRegistry()
            .unregisterProvider(handle, owner);
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace

namespace RenderStrategy {

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
        (void)renderStrategyRegistry();
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

} // namespace RenderStrategy

} // namespace Pelican
