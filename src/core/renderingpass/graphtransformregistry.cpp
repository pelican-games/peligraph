#include "graphtransformregistry.hpp"

#include "renderingsamplecount.hpp"
#include "rendertargetjsonparser.hpp"
#include "../../project/logicalrendertype.hpp"
#include "../../project/renderpipeline.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace Pelican {
namespace {

using RenderGraphTransform::Status;

constexpr char identityImplementationIdV1[] =
    "pelican.render.graph_transform.identity@1";
constexpr std::size_t maximumGraphTransformsV1 = 32;

std::uint32_t nextGeneration(
    std::uint32_t generation) noexcept {
    if (++generation == 0) ++generation;
    return generation;
}

bool validHandle(
    RenderGraphTransform::ProviderHandleV1
        handle) noexcept {
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
    const RenderGraphTransform::ProviderV1
        &provider) noexcept {
    if (provider.struct_size <
            sizeof(
                RenderGraphTransform::ProviderV1) ||
        provider.version !=
            RenderGraphTransform::descriptorVersionV1 ||
        provider.reserved0 != 0 ||
        provider.reserved1 != 0 ||
        provider.reserved2 != 0 ||
        provider.provider_version !=
            RenderGraphTransform::providerVersionV1 ||
        provider.minimum_engine_provider_version >
            RenderGraphTransform::providerVersionV1 ||
        provider.capability_bits !=
            RenderGraphTransform::
                builtinProviderCapabilitiesV1 ||
        provider.resolve_graph_transform == nullptr) {
        return false;
    }
    return validByteRange(
        provider.name_utf8, provider.name_size,
        RenderGraphTransform::
            maximumProviderNameBytesV1);
}

std::uint32_t abiSize(
    std::size_t size, std::string_view field,
    std::string_view transform) {
    if (size >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "logical graph transform " +
            std::string{field} +
            " exceeds the v1 ABI size limit for '" +
            std::string{transform} + "'");
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

Status builtinResolveGraphTransform(
    void *,
    const RenderGraphTransform::
        ResolveGraphTransformInputV1 *input,
    RenderGraphTransform::GraphTransformOutputV1
        *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size <
            sizeof(RenderGraphTransform::
                       ResolveGraphTransformInputV1) ||
        input->version !=
            RenderGraphTransform::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->reserved3 != 0 ||
        input->reserved4 != 0 ||
        input->contract == nullptr ||
        !validByteRange(
            input->transform_name_utf8,
            input->transform_name_size,
            RenderGraphTransform::
                maximumTransformNameBytesV1) ||
        !validByteRange(
            input->parameters_json_utf8,
            input->parameters_json_size,
            RenderGraphTransform::
                maximumParametersJsonBytesV1) ||
        !validByteRange(
            input->config_json_utf8,
            input->config_json_size,
            RenderGraphTransform::
                maximumConfigJsonBytesV1) ||
        !validByteRange(
            input->logical_graphs_json_utf8,
            input->logical_graphs_json_size,
            RenderGraphTransform::
                maximumLogicalGraphsJsonBytesV1) ||
        output->struct_size <
            sizeof(RenderGraphTransform::
                       GraphTransformOutputV1) ||
        output->version !=
            RenderGraphTransform::descriptorVersionV1 ||
        output->reserved0 != 0 ||
        output->reserved1 != 0 ||
        output->reserved2 != 0 ||
        output->reserved3 != 0) {
        return Status::invalid_argument;
    }
    *output =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::
                GraphTransformOutputV1>();
    output->implementation_id_utf8 =
        identityImplementationIdV1;
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(identityImplementationIdV1) - 1);
    output->config_json_utf8 =
        input->config_json_utf8;
    output->config_json_size =
        input->config_json_size;
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
                    static_cast<unsigned char>(
                        byte)));
        }
    }

    std::uint64_t value() const noexcept {
        return value_;
    }
};

RenderGraphTransform::MaterializationV1
materialization(
    LogicalMaterializationRequirement value) {
    switch (value) {
    case LogicalMaterializationRequirement::
        virtual_resource:
        return RenderGraphTransform::MaterializationV1::
            virtual_resource;
    case LogicalMaterializationRequirement::preferred:
        return RenderGraphTransform::MaterializationV1::
            preferred;
    case LogicalMaterializationRequirement::required:
        return RenderGraphTransform::MaterializationV1::
            required;
    case LogicalMaterializationRequirement::external:
        return RenderGraphTransform::MaterializationV1::
            external;
    }
    throw std::runtime_error(
        "unknown logical materialization in graph transform "
        "contract");
}

RenderGraphTransform::ImportKindV1 importKind(
    LogicalValueImportKind value) {
    switch (value) {
    case LogicalValueImportKind::graph_input:
        return RenderGraphTransform::ImportKindV1::
            graph_input;
    case LogicalValueImportKind::previous_epoch:
        return RenderGraphTransform::ImportKindV1::
            previous_epoch;
    case LogicalValueImportKind::external:
        return RenderGraphTransform::ImportKindV1::
            external;
    case LogicalValueImportKind::legacy_implicit:
        return RenderGraphTransform::ImportKindV1::none;
    }
    throw std::runtime_error(
        "unknown logical import kind in graph transform "
        "contract");
}

std::string copyProviderString(
    const char *data, std::uint32_t size,
    std::uint32_t maximum, std::string_view field,
    std::string_view provider,
    std::string_view transform) {
    if (!validByteRange(data, size, maximum)) {
        throw std::runtime_error(
            "logical graph transform provider '" +
            std::string{provider} +
            "' returned invalid " +
            std::string{field} + " for '" +
            std::string{transform} + "'");
    }
    return std::string{data, size};
}

void validateOutputEnvelope(
    const RenderGraphTransform::
        GraphTransformOutputV1 &output,
    std::string_view provider,
    std::string_view transform) {
    if (output.struct_size <
            sizeof(RenderGraphTransform::
                       GraphTransformOutputV1) ||
        output.version !=
            RenderGraphTransform::descriptorVersionV1 ||
        output.reserved0 != 0 ||
        output.reserved1 != 0 ||
        output.reserved2 != 0 ||
        output.reserved3 != 0) {
        throw std::runtime_error(
            "logical graph transform provider '" +
            std::string{provider} +
            "' returned an invalid output envelope for '" +
            std::string{transform} + "'");
    }
}

std::string canonicalJson(
    const nlohmann::json &value) {
    return nlohmann::ordered_json(value).dump();
}

std::string logicalGraphsJson(
    std::span<const CompiledLogicalRenderGraph> graphs) {
    std::vector<const CompiledLogicalRenderGraph *>
        ordered;
    ordered.reserve(graphs.size());
    for (const auto &graph : graphs) {
        ordered.push_back(&graph);
    }
    std::sort(
        ordered.begin(), ordered.end(),
        [](const auto *left, const auto *right) {
            return left->name < right->name;
        });
    auto encoded = nlohmann::ordered_json::array();
    for (const auto *graph : ordered) {
        encoded.push_back(
            compiledLogicalRenderGraphToJson(*graph));
    }
    return encoded.dump();
}

struct CompiledConfigState {
    std::vector<FrameGraphDefinition> frame_graphs;
    std::vector<CompiledLogicalRenderGraph>
        logical_graphs;
};

CompiledConfigState compileConfigState(
    const nlohmann::json &config) {
    auto render_targets =
        parseRenderTargetDefinitionsFromJson(config);
    auto frame_graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    auto logical_graphs =
        compileRenderingLogicalGraphs(
            frame_graphs, render_targets);
    if (logical_graphs.empty()) {
        throw std::runtime_error(
            "logical graph transform requires at least one "
            "render graph");
    }
    return {
        std::move(frame_graphs),
        std::move(logical_graphs),
    };
}

struct TransformRequest {
    std::string name;
    std::optional<std::string> provider;
    nlohmann::json parameters =
        nlohmann::json::object();
};

std::vector<TransformRequest>
parseTransformRequests(
    const nlohmann::json &config) {
    if (!config.contains("graph_transforms")) {
        return {};
    }
    const auto &encoded =
        config.at("graph_transforms");
    if (!encoded.is_array() ||
        encoded.size() > maximumGraphTransformsV1) {
        throw std::runtime_error(
            "graph_transforms must be an array with at "
            "most 32 entries");
    }
    std::vector<TransformRequest> result;
    result.reserve(encoded.size());
    std::set<std::string, std::less<>> names;
    for (std::size_t index = 0;
         index < encoded.size(); ++index) {
        const auto &request = encoded.at(index);
        if (!request.is_object()) {
            throw std::runtime_error(
                "graph_transforms[" +
                std::to_string(index) +
                "] must be an object");
        }
        for (auto field = request.begin();
             field != request.end(); ++field) {
            if (field.key() != "name" &&
                field.key() != "provider" &&
                field.key() != "parameters") {
                throw std::runtime_error(
                    "graph_transforms[" +
                    std::to_string(index) +
                    "] has unknown key '" +
                    field.key() + "'");
            }
        }
        if (!request.contains("name") ||
            !request.at("name").is_string()) {
            throw std::runtime_error(
                "graph transform requires a name string");
        }
        auto name =
            request.at("name").get<std::string>();
        if (name.empty() ||
            name.size() >
                RenderGraphTransform::
                    maximumTransformNameBytesV1 ||
            !names.insert(name).second) {
            throw std::runtime_error(
                "graph transform names must be unique, "
                "non-empty, and within the v1 limit: " +
                name);
        }
        std::optional<std::string> provider;
        if (request.contains("provider")) {
            if (!request.at("provider").is_string()) {
                throw std::runtime_error(
                    "graph transform provider must be a "
                    "string: " +
                    name);
            }
            provider =
                request.at("provider")
                    .get<std::string>();
            if (provider->empty() ||
                provider->size() >
                    RenderGraphTransform::
                        maximumProviderNameBytesV1) {
                throw std::runtime_error(
                    "graph transform provider is empty or "
                    "too long: " +
                    name);
            }
        }
        auto parameters = nlohmann::json::object();
        if (request.contains("parameters")) {
            parameters = request.at("parameters");
            if (!parameters.is_object()) {
                throw std::runtime_error(
                    "graph transform parameters must be an "
                    "object: " +
                    name);
            }
        }
        const auto parameters_json =
            canonicalJson(parameters);
        if (parameters_json.size() >
            RenderGraphTransform::
                maximumParametersJsonBytesV1) {
            throw std::runtime_error(
                "graph transform parameters are too large: " +
                name);
        }
        result.push_back({
            .name = std::move(name),
            .provider = std::move(provider),
            .parameters = std::move(parameters),
        });
    }
    return result;
}

nlohmann::json controlEnvelope(
    const nlohmann::json &config) {
    auto result = config;
    for (const auto field :
         {"graph_transforms", "render_targets",
          "rendering_passes", "passes", "compute_tasks",
          "buffers"}) {
        result.erase(field);
    }
    return result;
}

std::string graphName(
    const nlohmann::json &graph) {
    return graph.value(
        "name", std::string{"frame_graph"});
}

std::map<std::string, nlohmann::json, std::less<>>
graphControlEnvelopes(
    const nlohmann::json &config) {
    std::map<std::string, nlohmann::json, std::less<>>
        result;
    const auto add_graph =
        [&](const nlohmann::json &graph) {
            if (!graph.is_object()) {
                throw std::runtime_error(
                    "logical graph transform graph entry must "
                    "be an object");
            }
            auto envelope = graph;
            envelope.erase("passes");
            envelope.erase("compute_tasks");
            envelope.erase("buffers");
            const auto name = graphName(graph);
            if (name.empty() ||
                !result.emplace(name, std::move(envelope))
                     .second) {
                throw std::runtime_error(
                    "logical graph transform graph names must "
                    "be unique and non-empty: " +
                    name);
            }
        };
    if (config.contains("rendering_passes")) {
        const auto &graphs =
            config.at("rendering_passes");
        if (!graphs.is_array()) {
            throw std::runtime_error(
                "logical graph transform rendering_passes "
                "must be an array");
        }
        for (const auto &graph : graphs) {
            add_graph(graph);
        }
    } else if (
        config.contains("passes") ||
        config.contains("compute_tasks")) {
        add_graph(config);
    }
    return result;
}

using ProtectedNodeList =
    std::map<std::string, std::vector<nlohmann::json>,
             std::less<>>;

ProtectedNodeList protectedNodes(
    const nlohmann::json &config) {
    ProtectedNodeList result;
    const auto add_graph =
        [&](const nlohmann::json &graph) {
            const auto name = graphName(graph);
            auto &nodes = result[name];
            if (!graph.contains("passes")) return;
            const auto &passes = graph.at("passes");
            if (!passes.is_array()) {
                throw std::runtime_error(
                    "logical graph transform passes must be "
                    "an array: " +
                    name);
            }
            for (const auto &pass : passes) {
                if (!pass.is_object()) continue;
                const auto type =
                    pass.value(
                        "type", std::string{});
                if (type == "canonical_anchor" ||
                    type == "output_transform") {
                    nodes.push_back(pass);
                }
            }
        };
    if (config.contains("rendering_passes")) {
        for (const auto &graph :
             config.at("rendering_passes")) {
            add_graph(graph);
        }
    } else if (
        config.contains("passes") ||
        config.contains("compute_tasks")) {
        add_graph(config);
    }
    return result;
}

void requireValidProtectedTerminals(
    const nlohmann::json &config,
    std::string_view transform,
    std::string_view side) {
    const auto check_graph =
        [&](const nlohmann::json &graph) {
            if (!graph.contains("passes")) return;
            const auto &passes = graph.at("passes");
            std::size_t output_transforms = 0;
            for (const auto &pass : passes) {
                if (pass.is_object() &&
                    pass.value(
                        "type", std::string{}) ==
                        "output_transform") {
                    ++output_transforms;
                }
            }
            if (output_transforms == 0) return;
            if (output_transforms != 1 ||
                passes.empty() ||
                !passes.back().is_object() ||
                passes.back().value(
                    "type", std::string{}) !=
                    "output_transform") {
                throw std::runtime_error(
                    "logical graph transform '" +
                    std::string{transform} +
                    "' has a non-terminal or duplicate "
                    "output_transform in the " +
                    std::string{side} + " graph '" +
                    graphName(graph) + "'");
            }
        };
    if (config.contains("rendering_passes")) {
        for (const auto &graph :
             config.at("rendering_passes")) {
            check_graph(graph);
        }
    } else if (
        config.contains("passes") ||
        config.contains("compute_tasks")) {
        check_graph(config);
    }
}

void requireSameProtectedStructure(
    const nlohmann::json &before,
    const nlohmann::json &after,
    std::string_view transform) {
    requireValidProtectedTerminals(
        before, transform, "input");
    requireValidProtectedTerminals(
        after, transform, "candidate");
    if (controlEnvelope(before) !=
        controlEnvelope(after)) {
        throw std::runtime_error(
            "logical graph transform '" +
            std::string{transform} +
            "' changed pipeline policy or control fields");
    }
    if (graphControlEnvelopes(before) !=
        graphControlEnvelopes(after)) {
        throw std::runtime_error(
            "logical graph transform '" +
            std::string{transform} +
            "' changed graph-local policy or control fields");
    }
    if (protectedNodes(before) !=
        protectedNodes(after)) {
        throw std::runtime_error(
            "logical graph transform '" +
            std::string{transform} +
            "' changed canonical anchors or output_transform");
    }
    if (resolveMaterialRoutingTable(before) !=
        resolveMaterialRoutingTable(after)) {
        throw std::runtime_error(
            "logical graph transform '" +
            std::string{transform} +
            "' changed material routing");
    }
}

void requirePreservedBoundary(
    const LogicalGraphSetContract &before,
    const LogicalGraphSetContract &after,
    std::string_view transform) {
    if (before.graph_names != after.graph_names) {
        throw std::runtime_error(
            "logical graph transform '" +
            std::string{transform} +
            "' changed the graph entrypoint set");
    }
    using Key = std::tuple<
        std::string, std::string,
        RenderGraphTransform::BoundaryRoleV1>;
    std::map<Key,
             const LogicalGraphSetBoundaryPortContract *>
        after_ports;
    for (const auto &port :
         after.boundary_ports) {
        after_ports.emplace(
            Key{port.graph, port.resource, port.role},
            &port);
    }
    std::set<Key> original_keys;
    for (const auto &port :
         before.boundary_ports) {
        const Key key{
            port.graph, port.resource, port.role};
        original_keys.insert(key);
        const auto found = after_ports.find(key);
        if (found == after_ports.end() ||
            *found->second != port) {
            throw std::runtime_error(
                "logical graph transform '" +
                std::string{transform} +
                "' changed or removed protected boundary "
                "resource '" +
                port.graph + ":" + port.resource + "'");
        }
    }
    for (const auto &port :
         after.boundary_ports) {
        const Key key{
            port.graph, port.resource, port.role};
        if (original_keys.contains(key)) continue;
        // A transform may introduce a runtime-materialized image or
        // buffer as an implementation detail. New imports and external
        // resources, however, enlarge the callable graph-set boundary.
        if (port.import_kind !=
                RenderGraphTransform::ImportKindV1::none ||
            port.materialization ==
                RenderGraphTransform::MaterializationV1::
                    external) {
            throw std::runtime_error(
                "logical graph transform '" +
                std::string{transform} +
                "' introduced a new external or history "
                "boundary resource '" +
                port.graph + ":" + port.resource + "'");
        }
    }
}

int api_context_token = 0;

} // namespace

struct GraphTransformRegistryState {
    struct ProviderSlot {
        std::uint32_t generation = 0;
        bool active = false;
        internal::RegistrationOwner owner =
            internal::engineRegistrationOwner;
        std::uint32_t provider_version = 0;
        std::uint64_t capability_bits = 0;
        std::string name;
        void *context = nullptr;
        RenderGraphTransform::
            ResolveGraphTransformV1Fn
                resolve_graph_transform = nullptr;
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
                   internal::
                       registrationOwnerGeneration(owner);
    }

    void retireOwner(
        internal::RegistrationOwner owner) {
        retired_owner_generations.insert_or_assign(
            internal::registrationOwnerIdentity(owner),
            internal::registrationOwnerGeneration(owner));
    }
};

struct GraphTransformRegistrySnapshot::Impl {
    std::shared_lock<std::shared_mutex> lock;
    const GraphTransformRegistryState *registry =
        nullptr;

    Impl(
        std::shared_lock<std::shared_mutex>
            registry_lock,
        const GraphTransformRegistryState
            *value) noexcept
        : lock{std::move(registry_lock)},
          registry{value} {}
};

LogicalGraphSetContract makeLogicalGraphSetContract(
    std::span<const CompiledLogicalRenderGraph> graphs) {
    if (graphs.empty() ||
        graphs.size() >
            RenderGraphTransform::maximumGraphCountV1) {
        throw std::runtime_error(
            "logical graph transform contract requires a "
            "bounded non-empty graph set");
    }

    LogicalGraphSetContract result;
    result.id =
        RenderGraphTransform::graphSetContractIdV1;
    std::map<std::string,
             const CompiledLogicalRenderGraph *,
             std::less<>>
        graph_by_name;
    for (const auto &graph : graphs) {
        if (graph.name.empty() ||
            graph.name.size() >
                RenderGraphTransform::
                    maximumGraphNameBytesV1 ||
            !graph_by_name.emplace(
                graph.name, &graph)
                 .second) {
            throw std::runtime_error(
                "logical graph transform contract graph "
                "names must be unique, non-empty, and "
                "within the v1 limit: " +
                graph.name);
        }
    }

    for (const auto &[graph_name, graph] :
         graph_by_name) {
        result.graph_names.push_back(graph_name);
        std::map<std::string,
                 const LogicalResourceDesc *,
                 std::less<>>
            resources;
        for (const auto &resource :
             graph->resources) {
            resources.emplace(
                resource.name, &resource);
        }
        std::map<std::string, LogicalValueImportKind,
                 std::less<>>
            imports;
        for (const auto &imported :
             graph->imports) {
            const auto [found, inserted] =
                imports.emplace(
                    imported.value.resource,
                    imported.kind);
            if (!inserted &&
                found->second != imported.kind) {
                throw std::runtime_error(
                    "logical graph transform contract has "
                    "conflicting imports for resource: " +
                    imported.value.resource);
            }
        }
        std::set<std::string, std::less<>>
            written;
        for (const auto &node : graph->nodes) {
            for (const auto &use : node.uses) {
                if (use.output_value) {
                    written.insert(
                        use.output_value->resource);
                }
            }
        }

        for (const auto &[resource_name, resource] :
             resources) {
            const auto imported =
                imports.find(resource_name);
            const auto non_legacy_import =
                imported != imports.end() &&
                imported->second !=
                    LogicalValueImportKind::
                        legacy_implicit;
            const auto retained =
                non_legacy_import ||
                resource->materialization ==
                    LogicalMaterializationRequirement::
                        required ||
                resource->materialization ==
                    LogicalMaterializationRequirement::
                        external;
            if (!retained) continue;
            if (resource_name.empty() ||
                resource_name.size() >
                    RenderGraphTransform::
                        maximumResourceNameBytesV1) {
                throw std::runtime_error(
                    "logical graph transform boundary "
                    "resource is empty or too long: " +
                    resource_name);
            }
            const auto type_json =
                logicalTypeToJson(
                    resource->type)
                    .dump();
            if (type_json.empty() ||
                type_json.size() >
                    RenderGraphTransform::
                        maximumTypeJsonBytesV1) {
                throw std::runtime_error(
                    "logical graph transform boundary type "
                    "JSON is empty or too large: " +
                    resource_name);
            }
            const auto encoded_import =
                imported == imports.end()
                    ? RenderGraphTransform::
                          ImportKindV1::none
                    : importKind(imported->second);
            const auto append =
                [&](RenderGraphTransform::
                        BoundaryRoleV1 role) {
                    result.boundary_ports.push_back(
                        LogicalGraphSetBoundaryPortContract{
                            .graph = graph_name,
                            .resource =
                                resource_name,
                            .type_json =
                                type_json,
                            .role = role,
                            .materialization =
                                materialization(
                                    resource
                                        ->materialization),
                            .import_kind =
                                encoded_import,
                        });
                };
            append(
                RenderGraphTransform::
                    BoundaryRoleV1::
                        retained_resource);
            if (non_legacy_import) {
                append(
                    RenderGraphTransform::
                        BoundaryRoleV1::input);
            }
            if (written.contains(resource_name)) {
                append(
                    RenderGraphTransform::
                        BoundaryRoleV1::output);
            }
        }
    }

    if (result.boundary_ports.size() >
        RenderGraphTransform::maximumBoundaryPortsV1) {
        throw std::runtime_error(
            "logical graph transform boundary has too many "
            "ports");
    }
    std::sort(
        result.boundary_ports.begin(),
        result.boundary_ports.end(),
        [](const auto &left, const auto &right) {
            if (left.graph != right.graph) {
                return left.graph < right.graph;
            }
            if (left.resource != right.resource) {
                return left.resource < right.resource;
            }
            return left.role < right.role;
        });

    StableFingerprint fingerprint;
    fingerprint.appendString(result.id);
    fingerprint.appendUnsigned(
        result.graph_names.size());
    for (const auto &name : result.graph_names) {
        fingerprint.appendString(name);
    }
    fingerprint.appendUnsigned(
        result.boundary_ports.size());
    for (const auto &port :
         result.boundary_ports) {
        fingerprint.appendString(port.graph);
        fingerprint.appendString(port.resource);
        fingerprint.appendString(port.type_json);
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(port.role));
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(
                port.materialization));
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(
                port.import_kind));
    }
    result.fingerprint = fingerprint.value();
    return result;
}

std::uint64_t logicalGraphSetFingerprint(
    std::span<const CompiledLogicalRenderGraph> graphs) {
    StableFingerprint fingerprint;
    fingerprint.appendString(
        logicalGraphsJson(graphs));
    return fingerprint.value();
}

GraphTransformRegistrySnapshot::
    GraphTransformRegistrySnapshot() noexcept =
        default;
GraphTransformRegistrySnapshot::
    ~GraphTransformRegistrySnapshot() = default;
GraphTransformRegistrySnapshot::
    GraphTransformRegistrySnapshot(
        GraphTransformRegistrySnapshot &&) noexcept =
        default;
GraphTransformRegistrySnapshot &
GraphTransformRegistrySnapshot::operator=(
    GraphTransformRegistrySnapshot &&) noexcept =
    default;

GraphTransformRegistrySnapshot::
    GraphTransformRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

ResolvedLogicalGraphTransform
GraphTransformRegistrySnapshot::resolveTransform(
    const LogicalGraphSetContract &contract,
    std::string_view transform_name,
    std::uint32_t transform_index,
    std::string_view parameters_json,
    std::string_view config_json,
    std::string_view logical_graphs_json,
    std::uint64_t logical_graphs_fingerprint,
    const std::optional<std::string>
        &requested_provider) const {
    if (impl_ == nullptr ||
        impl_->registry == nullptr) {
        throw std::runtime_error(
            "logical graph transform registry snapshot is "
            "empty");
    }
    if (contract.id !=
            RenderGraphTransform::
                graphSetContractIdV1 ||
        contract.graph_names.empty() ||
        contract.graph_names.size() >
            RenderGraphTransform::maximumGraphCountV1 ||
        contract.boundary_ports.size() >
            RenderGraphTransform::
                maximumBoundaryPortsV1 ||
        transform_name.empty() ||
        transform_name.size() >
            RenderGraphTransform::
                maximumTransformNameBytesV1) {
        throw std::runtime_error(
            "logical graph transform contract envelope is "
            "invalid");
    }
    const auto &registry = *impl_->registry;
    const auto requested =
        requested_provider.value_or(
            std::string{
                builtinLogicalGraphTransformProvider});

    const auto find_for_owner =
        [&](internal::RegistrationOwner owner)
        -> const GraphTransformRegistryState::
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
    const GraphTransformRegistryState::
        ProviderSlot *selected = nullptr;
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
            "logical graph transform provider '" +
            requested +
            "' is not registered for the active owner "
            "while compiling '" +
            std::string{transform_name} + "'");
    }

    std::vector<
        RenderGraphTransform::BoundaryPortV1>
        abi_ports;
    abi_ports.reserve(
        contract.boundary_ports.size());
    for (const auto &port :
         contract.boundary_ports) {
        auto abi =
            RenderGraphTransform::descriptor<
                RenderGraphTransform::
                    BoundaryPortV1>();
        abi.graph_name_utf8 = port.graph.data();
        abi.graph_name_size =
            abiSize(
                port.graph.size(), "graph name",
                transform_name);
        abi.resource_utf8 =
            port.resource.data();
        abi.resource_size =
            abiSize(
                port.resource.size(),
                "resource name", transform_name);
        abi.type_json_utf8 =
            port.type_json.data();
        abi.type_json_size =
            abiSize(
                port.type_json.size(),
                "type JSON", transform_name);
        abi.role = port.role;
        abi.materialization =
            port.materialization;
        abi.import_kind = port.import_kind;
        abi_ports.push_back(abi);
    }

    auto abi_contract =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::
                GraphSetContractV1>();
    abi_contract.contract_id_utf8 =
        contract.id.data();
    abi_contract.contract_id_size =
        abiSize(
            contract.id.size(), "contract id",
            transform_name);
    abi_contract.graph_count =
        abiSize(
            contract.graph_names.size(),
            "graph count", transform_name);
    abi_contract.boundary_ports =
        abi_ports.empty() ? nullptr
                          : abi_ports.data();
    abi_contract.boundary_port_count =
        abiSize(
            abi_ports.size(),
            "boundary port count", transform_name);
    abi_contract.boundary_fingerprint =
        contract.fingerprint;

    const auto require_range =
        [&](std::string_view value,
            std::uint32_t maximum,
            std::string_view field) {
            if (value.empty() ||
                value.size() > maximum) {
                throw std::runtime_error(
                    "logical graph transform " +
                    std::string{field} +
                    " is empty or too large for '" +
                    std::string{transform_name} + "'");
            }
        };
    require_range(
        parameters_json,
        RenderGraphTransform::
            maximumParametersJsonBytesV1,
        "parameters JSON");
    require_range(
        config_json,
        RenderGraphTransform::
            maximumConfigJsonBytesV1,
        "config JSON");
    require_range(
        logical_graphs_json,
        RenderGraphTransform::
            maximumLogicalGraphsJsonBytesV1,
        "logical graphs JSON");

    auto input =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::
                ResolveGraphTransformInputV1>();
    input.contract = &abi_contract;
    input.transform_name_utf8 =
        transform_name.data();
    input.transform_name_size =
        abiSize(
            transform_name.size(),
            "transform name", transform_name);
    input.transform_index = transform_index;
    input.parameters_json_utf8 =
        parameters_json.data();
    input.parameters_json_size =
        abiSize(
            parameters_json.size(),
            "parameters JSON", transform_name);
    input.config_json_utf8 =
        config_json.data();
    input.config_json_size =
        abiSize(
            config_json.size(),
            "config JSON", transform_name);
    input.logical_graphs_json_utf8 =
        logical_graphs_json.data();
    input.logical_graphs_json_size =
        abiSize(
            logical_graphs_json.size(),
            "logical graphs JSON", transform_name);
    input.logical_graphs_fingerprint =
        logical_graphs_fingerprint;

    auto output =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::
                GraphTransformOutputV1>();
    const auto status =
        selected->resolve_graph_transform(
            selected->context, &input, &output);
    if (status != Status::ok) {
        throw std::runtime_error(
            "logical graph transform provider '" +
            requested + "' failed for '" +
            std::string{transform_name} +
            "' with status " +
            statusName(status));
    }
    validateOutputEnvelope(
        output, requested, transform_name);
    auto implementation = copyProviderString(
        output.implementation_id_utf8,
        output.implementation_id_size,
        RenderGraphTransform::
            maximumImplementationIdBytesV1,
        "implementation id", requested,
        transform_name);
    try {
        (void)parseSemanticTypeId(implementation);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "logical graph transform provider '" +
            requested +
            "' returned invalid implementation id '" +
            implementation + "' for '" +
            std::string{transform_name} + "': " +
            error.what());
    }
    auto transformed_config = copyProviderString(
        output.config_json_utf8,
        output.config_json_size,
        RenderGraphTransform::
            maximumConfigJsonBytesV1,
        "config JSON", requested, transform_name);

    const auto slot_index =
        static_cast<std::uint64_t>(
            selected - registry.providers.data());
    return {
        .config_json = std::move(transformed_config),
        .selection =
            LogicalGraphTransformSelection{
                .name =
                    std::string{transform_name},
                .provider = requested,
                .implementation =
                    std::move(implementation),
                .contract = contract.id,
                .boundary_fingerprint =
                    contract.fingerprint,
                .input_graph_fingerprint =
                    logical_graphs_fingerprint,
                .provider_owner = selected->owner,
                .provider_identity =
                    slot_index + 1,
                .provider_generation =
                    selected->generation,
                .provider_version =
                    selected->provider_version,
                .provider_capability_bits =
                    selected->capability_bits,
                .transform_index =
                    transform_index,
                .explicitly_selected =
                    requested_provider.has_value(),
            },
    };
}

GraphTransformRegistry::GraphTransformRegistry()
    : impl_{
          std::make_unique<
              GraphTransformRegistryState>()} {
    auto provider =
        RenderGraphTransform::descriptor<
            RenderGraphTransform::ProviderV1>();
    provider.capability_bits =
        RenderGraphTransform::
            builtinProviderCapabilitiesV1;
    provider.name_utf8 =
        builtinLogicalGraphTransformProvider.data();
    provider.name_size =
        static_cast<std::uint32_t>(
            builtinLogicalGraphTransformProvider
                .size());
    provider.resolve_graph_transform =
        builtinResolveGraphTransform;
    RenderGraphTransform::ProviderHandleV1 handle{};
    if (registerProvider(
            provider,
            internal::engineRegistrationOwner,
            handle) != Status::ok) {
        throw std::logic_error(
            "failed to register builtin logical graph "
            "transform provider");
    }
}

GraphTransformRegistry::~GraphTransformRegistry() =
    default;

RenderGraphTransform::Status
GraphTransformRegistry::registerProvider(
    const RenderGraphTransform::ProviderV1 &provider,
    internal::RegistrationOwner owner,
    RenderGraphTransform::ProviderHandleV1
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
            slot_index =
                impl_->free_slots.back();
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

        auto &slot =
            impl_->providers[slot_index];
        slot.generation =
            nextGeneration(slot.generation);
        slot.active = true;
        slot.owner = owner;
        slot.provider_version =
            provider.provider_version;
        slot.capability_bits =
            provider.capability_bits;
        slot.name.assign(
            provider.name_utf8,
            provider.name_size);
        slot.context = provider.context;
        slot.resolve_graph_transform =
            provider.resolve_graph_transform;
        out_handle =
            RenderGraphTransform::ProviderHandleV1{
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

RenderGraphTransform::Status
GraphTransformRegistry::unregisterProvider(
    RenderGraphTransform::ProviderHandleV1 handle,
    internal::RegistrationOwner owner) noexcept {
    if (!validHandle(handle)) {
        return Status::invalid_argument;
    }
    if (!internal::isRegistrationOwnerCurrent(owner)) {
        return Status::stale_owner;
    }
    std::unique_lock lock{impl_->mutex};
    if (handle.identity >
        impl_->providers.size()) {
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
    slot.resolve_graph_transform = nullptr;
    impl_->free_slots.push_back(index);
    return Status::ok;
}

GraphTransformRegistrySnapshot
GraphTransformRegistry::snapshot() const {
    std::shared_lock lock{impl_->mutex};
    return GraphTransformRegistrySnapshot{
        std::make_unique<
            GraphTransformRegistrySnapshot::Impl>(
            std::move(lock), impl_.get())};
}

void GraphTransformRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    std::unique_lock lock{impl_->mutex};
    impl_->active_game_owner =
        !impl_->ownerIsRetired(owner) &&
                internal::isRegistrationOwnerCurrent(owner)
            ? owner
            : internal::engineRegistrationOwner;
}

void GraphTransformRegistry::releaseOwner(
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
        slot.resolve_graph_transform = nullptr;
        impl_->free_slots.push_back(index);
    }
}

ResolvedLogicalGraphTransformConfig
resolveLogicalGraphTransforms(
    const nlohmann::json &config,
    const GraphTransformRegistrySnapshot &providers) {
    if (!config.is_object()) {
        throw std::runtime_error(
            "logical graph transform config must be an "
            "object");
    }
    const auto requests =
        parseTransformRequests(config);
    ResolvedLogicalGraphTransformConfig result{
        .config = config,
    };
    result.config.erase("graph_transforms");
    for (std::size_t index = 0;
         index < requests.size(); ++index) {
        const auto &request = requests[index];
        const auto before =
            compileConfigState(result.config);
        const auto contract =
            makeLogicalGraphSetContract(
                before.logical_graphs);
        const auto input_fingerprint =
            logicalGraphSetFingerprint(
                before.logical_graphs);
        const auto parameters_json =
            canonicalJson(request.parameters);
        const auto config_json =
            canonicalJson(result.config);
        const auto graphs_json =
            logicalGraphsJson(
                before.logical_graphs);
        auto resolved = providers.resolveTransform(
            contract, request.name,
            static_cast<std::uint32_t>(index),
            parameters_json, config_json,
            graphs_json, input_fingerprint,
            request.provider);

        nlohmann::json candidate;
        try {
            candidate =
                nlohmann::json::parse(
                    resolved.config_json);
        } catch (const std::exception &error) {
            throw std::runtime_error(
                "logical graph transform provider '" +
                resolved.selection.provider +
                "' returned malformed JSON for '" +
                request.name + "': " +
                error.what());
        }
        if (!candidate.is_object()) {
            throw std::runtime_error(
                "logical graph transform '" +
                request.name +
                "' returned a non-object config");
        }
        if (candidate.contains("graph_transforms")) {
            throw std::runtime_error(
                "logical graph transform '" +
                request.name +
                "' reintroduced reserved graph_transforms "
                "control data");
        }
        requireSameProtectedStructure(
            result.config, candidate,
            request.name);

        const auto after =
            compileConfigState(candidate);
        const auto after_contract =
            makeLogicalGraphSetContract(
                after.logical_graphs);
        requirePreservedBoundary(
            contract, after_contract,
            request.name);
        resolved.selection.output_graph_fingerprint =
            logicalGraphSetFingerprint(
                after.logical_graphs);
        result.selections.push_back(
            std::move(resolved.selection));
        result.config = std::move(candidate);
    }
    return result;
}

void applyResolvedLogicalGraphTransformSelections(
    std::span<FrameGraphDefinition> frame_graphs,
    std::span<const LogicalGraphTransformSelection>
        selections) {
    for (auto &frame_graph : frame_graphs) {
        if (!frame_graph.graph_transforms.empty()) {
            throw std::runtime_error(
                "resolved logical graph transform "
                "provenance is already populated: " +
                frame_graph.name);
        }
        frame_graph.graph_transforms.assign(
            selections.begin(), selections.end());
    }
}

GraphTransformRegistry &graphTransformRegistry() {
    static auto *value =
        new GraphTransformRegistry;
    return *value;
}

namespace render_graph_transform_internal {

void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        graphTransformRegistry()
            .activateOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        graphTransformRegistry()
            .releaseOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

} // namespace render_graph_transform_internal

namespace {

Status registerProviderApi(
    void *context,
    const RenderGraphTransform::ProviderV1 *provider,
    RenderGraphTransform::ProviderHandleV1
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
        return graphTransformRegistry()
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
    RenderGraphTransform::ProviderHandleV1
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
        return graphTransformRegistry()
            .unregisterProvider(handle, owner);
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace

namespace RenderGraphTransform {

Status getApiV1(
    std::uint32_t client_abi_version,
    ApiV1 *out_api) noexcept {
    if (out_api == nullptr ||
        out_api->struct_size < sizeof(ApiV1)) {
        return Status::invalid_argument;
    }
    if (out_api->version !=
        descriptorVersionV1) {
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
        (void)graphTransformRegistry();
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

} // namespace RenderGraphTransform

} // namespace Pelican
