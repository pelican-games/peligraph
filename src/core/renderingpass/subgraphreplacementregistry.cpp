#include "subgraphreplacementregistry.hpp"

#include "renderingpassjsonhelpers.hpp"
#include "renderingsamplecount.hpp"
#include "rendertargetjsonparser.hpp"
#include "../../project/logicalrendertype.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Pelican {
namespace {

using RenderSubgraph::Status;

static_assert(
    maximumLogicalRegionTagBytes ==
    RenderSubgraph::maximumRegionTagBytesV1);

constexpr char authoredSubgraphImplementationIdV1[] =
    "pelican.render.authored_subgraph@1";
constexpr std::size_t maximumRegionRequestsPerGraphV1 = 64;

std::uint32_t nextGeneration(
    std::uint32_t generation) noexcept {
    if (++generation == 0) ++generation;
    return generation;
}

bool validHandle(
    RenderSubgraph::ProviderHandleV1 handle) noexcept {
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
    const RenderSubgraph::ProviderV1 &provider) noexcept {
    if (provider.struct_size <
            sizeof(RenderSubgraph::ProviderV1) ||
        provider.version !=
            RenderSubgraph::descriptorVersionV1 ||
        provider.reserved0 != 0 ||
        provider.reserved1 != 0 ||
        provider.reserved2 != 0 ||
        provider.provider_version !=
            RenderSubgraph::providerVersionV1 ||
        provider.minimum_engine_provider_version >
            RenderSubgraph::providerVersionV1 ||
        provider.capability_bits !=
            RenderSubgraph::
                builtinProviderCapabilitiesV1 ||
        provider.resolve_region == nullptr) {
        return false;
    }
    return validByteRange(
        provider.name_utf8, provider.name_size,
        RenderSubgraph::maximumProviderNameBytesV1);
}

std::uint32_t abiSize(
    std::size_t size, std::string_view field,
    std::string_view region) {
    if (size >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            "subgraph replacement " +
            std::string{field} +
            " exceeds the v1 ABI size limit for region '" +
            std::string{region} + "'");
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

Status builtinResolveRegion(
    void *,
    const RenderSubgraph::ResolveRegionInputV1 *input,
    RenderSubgraph::RegionReplacementV1 *output) noexcept {
    if (input == nullptr || output == nullptr ||
        input->struct_size <
            sizeof(
                RenderSubgraph::ResolveRegionInputV1) ||
        input->version !=
            RenderSubgraph::descriptorVersionV1 ||
        input->reserved0 != 0 ||
        input->reserved1 != 0 ||
        input->reserved2 != 0 ||
        input->contract == nullptr ||
        !validByteRange(
            input->authored_subgraph_json_utf8,
            input->authored_subgraph_json_size,
            RenderSubgraph::
                maximumSubgraphJsonBytesV1) ||
        output->struct_size <
            sizeof(RenderSubgraph::RegionReplacementV1) ||
        output->version !=
            RenderSubgraph::descriptorVersionV1 ||
        output->reserved0 != 0 ||
        output->reserved1 != 0 ||
        output->reserved2 != 0 ||
        output->reserved3 != 0) {
        return Status::invalid_argument;
    }
    *output =
        RenderSubgraph::descriptor<
            RenderSubgraph::RegionReplacementV1>();
    output->implementation_id_utf8 =
        authoredSubgraphImplementationIdV1;
    output->implementation_id_size =
        static_cast<std::uint32_t>(
            sizeof(authoredSubgraphImplementationIdV1) -
            1);
    output->subgraph_json_utf8 =
        input->authored_subgraph_json_utf8;
    output->subgraph_json_size =
        input->authored_subgraph_json_size;
    return Status::ok;
}

RenderSubgraph::MaterializationV1 materialization(
    LogicalMaterializationRequirement value) {
    switch (value) {
    case LogicalMaterializationRequirement::
        virtual_resource:
        return RenderSubgraph::MaterializationV1::
            virtual_resource;
    case LogicalMaterializationRequirement::preferred:
        return RenderSubgraph::MaterializationV1::preferred;
    case LogicalMaterializationRequirement::required:
        return RenderSubgraph::MaterializationV1::required;
    case LogicalMaterializationRequirement::external:
        return RenderSubgraph::MaterializationV1::external;
    }
    throw std::runtime_error(
        "unknown logical materialization in tagged region "
        "contract");
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

std::string copyProviderString(
    const char *data, std::uint32_t size,
    std::uint32_t maximum, std::string_view field,
    std::string_view provider,
    std::string_view region) {
    if (!validByteRange(data, size, maximum)) {
        throw std::runtime_error(
            "subgraph replacement provider '" +
            std::string{provider} +
            "' returned invalid " +
            std::string{field} + " for region '" +
            std::string{region} + "'");
    }
    return std::string{data, size};
}

void validateOutputEnvelope(
    const RenderSubgraph::RegionReplacementV1 &output,
    std::string_view provider,
    std::string_view region) {
    if (output.struct_size <
            sizeof(
                RenderSubgraph::RegionReplacementV1) ||
        output.version !=
            RenderSubgraph::descriptorVersionV1 ||
        output.reserved0 != 0 ||
        output.reserved1 != 0 ||
        output.reserved2 != 0 ||
        output.reserved3 != 0) {
        throw std::runtime_error(
            "subgraph replacement provider '" +
            std::string{provider} +
            "' returned an invalid output envelope for "
            "region '" +
            std::string{region} + "'");
    }
}

int api_context_token = 0;

struct RegionRequest {
    std::string region;
    std::optional<std::string> provider;
};

struct CompiledConfigState {
    std::vector<FrameGraphDefinition> frame_graphs;
    std::vector<CompiledLogicalRenderGraph>
        logical_graphs;
};

std::string graphName(
    const nlohmann::json &graph) {
    return graph.value(
        "name", std::string{"frame_graph"});
}

nlohmann::json &findGraphObject(
    nlohmann::json &config,
    std::string_view name) {
    nlohmann::json *result = nullptr;
    if (config.contains("rendering_passes")) {
        auto &graphs = config.at("rendering_passes");
        if (!graphs.is_array()) {
            throw std::runtime_error(
                "subgraph replacement rendering_passes must "
                "be an array");
        }
        for (auto &graph : graphs) {
            if (!graph.is_object()) {
                throw std::runtime_error(
                    "subgraph replacement graph entries must "
                    "be objects");
            }
            if (graphName(graph) != name) continue;
            if (result != nullptr) {
                throw std::runtime_error(
                    "subgraph replacement graph name is "
                    "ambiguous: " +
                    std::string{name});
            }
            result = &graph;
        }
    } else if (
        config.contains("passes") ||
        config.contains("compute_tasks")) {
        if (graphName(config) == name) {
            result = &config;
        }
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "subgraph replacement graph not found: " +
            std::string{name});
    }
    return *result;
}

const CompiledLogicalRenderGraph &findLogicalGraph(
    const CompiledConfigState &state,
    std::string_view name) {
    const CompiledLogicalRenderGraph *result = nullptr;
    for (const auto &graph : state.logical_graphs) {
        if (graph.name != name) continue;
        if (result != nullptr) {
            throw std::runtime_error(
                "subgraph replacement logical graph name is "
                "ambiguous: " +
                std::string{name});
        }
        result = &graph;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "subgraph replacement logical graph not found: " +
            std::string{name});
    }
    return *result;
}

CompiledConfigState compileConfigState(
    const nlohmann::json &config) {
    auto render_targets =
        parseRenderTargetDefinitionsFromJson(config);
    auto frame_graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    auto logical_graphs =
        compileRenderingLogicalGraphs(
            frame_graphs, render_targets);
    return {
        std::move(frame_graphs),
        std::move(logical_graphs),
    };
}

std::vector<std::string> graphNames(
    const nlohmann::json &config) {
    std::vector<std::string> result;
    std::set<std::string, std::less<>> unique;
    if (config.contains("rendering_passes")) {
        const auto &graphs =
            config.at("rendering_passes");
        if (!graphs.is_array()) {
            throw std::runtime_error(
                "subgraph replacement rendering_passes must "
                "be an array");
        }
        for (const auto &graph : graphs) {
            if (!graph.is_object()) {
                throw std::runtime_error(
                    "subgraph replacement graph entries must "
                    "be objects");
            }
            const auto name = graphName(graph);
            if (name.empty() ||
                !unique.insert(name).second) {
                throw std::runtime_error(
                    "subgraph replacement graph names must be "
                    "unique and non-empty: " +
                    name);
            }
            result.push_back(name);
        }
    } else if (
        config.contains("passes") ||
        config.contains("compute_tasks")) {
        const auto name = graphName(config);
        if (name.empty()) {
            throw std::runtime_error(
                "subgraph replacement graph name must not be "
                "empty");
        }
        result.push_back(name);
    }
    return result;
}

std::vector<RegionRequest> parseRegionRequests(
    const nlohmann::json &graph) {
    if (!graph.contains("region_replacements")) {
        return {};
    }
    const auto &encoded =
        graph.at("region_replacements");
    if (!encoded.is_array() ||
        encoded.size() >
            maximumRegionRequestsPerGraphV1) {
        throw std::runtime_error(
            "region_replacements must be a bounded array");
    }
    std::vector<RegionRequest> result;
    result.reserve(encoded.size());
    std::set<std::string, std::less<>> regions;
    for (const auto &request : encoded) {
        if (!request.is_object() ||
            !request.contains("region") ||
            !request.at("region").is_string() ||
            (request.size() != 1 &&
             request.size() != 2) ||
            (request.size() == 2 &&
             (!request.contains("provider") ||
              !request.at("provider").is_string()))) {
            throw std::runtime_error(
                "region replacement request must contain a "
                "region and optional provider string only");
        }
        auto region =
            request.at("region").get<std::string>();
        if (region.empty() ||
            region.size() >
                RenderSubgraph::
                    maximumRegionTagBytesV1) {
            throw std::runtime_error(
                "region replacement tag is empty or too "
                "long");
        }
        if (!regions.insert(region).second) {
            throw std::runtime_error(
                "duplicate region replacement request: " +
                region);
        }
        std::optional<std::string> provider;
        if (request.contains("provider")) {
            provider =
                request.at("provider")
                    .get<std::string>();
            if (provider->empty() ||
                provider->size() >
                    RenderSubgraph::
                        maximumProviderNameBytesV1) {
                throw std::runtime_error(
                    "region replacement provider name is "
                    "empty or too long: " +
                    region);
            }
        }
        result.push_back({
            .region = std::move(region),
            .provider = std::move(provider),
        });
    }
    return result;
}

bool hasRegion(
    const nlohmann::json &pass,
    std::string_view region) {
    const auto tags = parseOptionalRegionTags(
        pass, "Subgraph replacement pass");
    return std::find(
               tags.begin(), tags.end(), region) !=
           tags.end();
}

std::vector<std::string> relationList(
    const nlohmann::json &node,
    std::string_view field) {
    if (!node.contains(field)) return {};
    const auto &encoded = node.at(field);
    if (encoded.is_string()) {
        return {encoded.get<std::string>()};
    }
    if (!encoded.is_array()) {
        throw std::runtime_error(
            "subgraph replacement relation '" +
            std::string{field} +
            "' must be a string or string array");
    }
    std::vector<std::string> result;
    result.reserve(encoded.size());
    for (const auto &entry : encoded) {
        if (!entry.is_string()) {
            throw std::runtime_error(
                "subgraph replacement relation '" +
                std::string{field} +
                "' entries must be strings");
        }
        result.push_back(entry.get<std::string>());
    }
    return result;
}

void appendUnique(
    std::vector<std::string> &values,
    std::string value) {
    if (std::find(
            values.begin(), values.end(), value) ==
        values.end()) {
        values.push_back(std::move(value));
    }
}

void setRelationList(
    nlohmann::json &node, std::string_view field,
    std::vector<std::string> values) {
    std::vector<std::string> unique;
    unique.reserve(values.size());
    for (auto &value : values) {
        if (value.empty()) {
            throw std::runtime_error(
                "subgraph replacement relation must not be "
                "empty");
        }
        appendUnique(unique, std::move(value));
    }
    if (unique.empty()) {
        node.erase(std::string{field});
    } else {
        node[std::string{field}] =
            std::move(unique);
    }
}

std::vector<std::string> commonRegionTags(
    const nlohmann::json &passes,
    std::span<const std::size_t> indices) {
    auto common = parseOptionalRegionTags(
        passes.at(indices.front()),
        "Subgraph replacement source pass");
    for (std::size_t offset = 1;
         offset < indices.size(); ++offset) {
        const auto tags = parseOptionalRegionTags(
            passes.at(indices[offset]),
            "Subgraph replacement source pass");
        common.erase(
            std::remove_if(
                common.begin(), common.end(),
                [&](const auto &tag) {
                    return std::find(
                               tags.begin(), tags.end(),
                               tag) == tags.end();
                }),
            common.end());
    }
    return common;
}

std::vector<std::string> sourcePassNames(
    const nlohmann::json &passes,
    std::span<const std::size_t> indices,
    std::string_view region) {
    std::vector<std::string> result;
    result.reserve(indices.size());
    for (const auto index : indices) {
        const auto &pass = passes.at(index);
        if (!pass.is_object() ||
            pass.value("type", std::string{}) !=
                "fullscreen" ||
            !pass.contains("name") ||
            !pass.at("name").is_string()) {
            throw std::runtime_error(
                "tagged region v1 supports contiguous "
                "fullscreen passes only: " +
                std::string{region});
        }
        auto name =
            pass.at("name").get<std::string>();
        if (name.empty() ||
            name == "output_transform" ||
            name.starts_with("__anchor_")) {
            throw std::runtime_error(
                "tagged region v1 cannot replace protected "
                "or unnamed pass: " +
                name);
        }
        result.push_back(std::move(name));
    }
    return result;
}

void requireSameResources(
    const CompiledLogicalRenderGraph &before,
    const CompiledLogicalRenderGraph &after,
    std::string_view region) {
    std::map<std::string, const LogicalResourceDesc *,
             std::less<>>
        before_resources;
    std::map<std::string, const LogicalResourceDesc *,
             std::less<>>
        after_resources;
    for (const auto &resource : before.resources) {
        before_resources.emplace(
            resource.name, &resource);
    }
    for (const auto &resource : after.resources) {
        after_resources.emplace(
            resource.name, &resource);
    }
    if (before_resources.size() !=
        after_resources.size()) {
        throw std::runtime_error(
            "tagged region v1 replacement cannot add or "
            "remove logical resources: " +
            std::string{region});
    }
    for (const auto &[name, before_resource] :
         before_resources) {
        const auto found =
            after_resources.find(name);
        if (found == after_resources.end() ||
            before_resource->type !=
                found->second->type ||
            before_resource->materialization !=
                found->second->materialization) {
            throw std::runtime_error(
                "tagged region v1 replacement changed "
                "logical resource contract: " +
                name);
        }
    }
}

std::vector<std::string> replacementPassNames(
    nlohmann::json &replacement,
    std::span<const std::string> inherited_regions,
    std::string_view provider,
    std::string_view region) {
    if (!replacement.is_array() ||
        replacement.empty() ||
        replacement.size() >
            RenderSubgraph::
                maximumReplacementPassesV1) {
        throw std::runtime_error(
            "subgraph replacement provider '" +
            std::string{provider} +
            "' must return a non-empty bounded pass array "
            "for region '" +
            std::string{region} + "'");
    }
    std::vector<std::string> names;
    names.reserve(replacement.size());
    std::set<std::string, std::less<>> unique;
    for (auto &pass : replacement) {
        if (!pass.is_object() ||
            pass.value("type", std::string{}) !=
                "fullscreen" ||
            !pass.contains("name") ||
            !pass.at("name").is_string()) {
            throw std::runtime_error(
                "subgraph replacement provider '" +
                std::string{provider} +
                "' returned a non-fullscreen pass for "
                "region '" +
                std::string{region} + "'");
        }
        auto name =
            pass.at("name").get<std::string>();
        if (name.empty() ||
            name == "output_transform" ||
            name.starts_with("__anchor_") ||
            !unique.insert(name).second) {
            throw std::runtime_error(
                "subgraph replacement provider '" +
                std::string{provider} +
                "' returned a duplicate, protected, or "
                "empty pass name for region '" +
                std::string{region} + "'");
        }
        pass["regions"] = std::vector<std::string>(
            inherited_regions.begin(),
            inherited_regions.end());
        names.push_back(std::move(name));
    }
    return names;
}

void appendRelations(
    nlohmann::json &node, std::string_view field,
    std::span<const std::string> values) {
    auto relations = relationList(node, field);
    for (const auto &value : values) {
        appendUnique(relations, value);
    }
    setRelationList(
        node, field, std::move(relations));
}

void rewriteExternalRelations(
    nlohmann::json &pass, std::string_view field,
    const std::set<std::string, std::less<>>
        &source_names,
    std::span<const std::string>
        replacement_names) {
    auto relations = relationList(pass, field);
    std::vector<std::string> rewritten;
    for (auto &relation : relations) {
        if (!source_names.contains(relation)) {
            appendUnique(
                rewritten, std::move(relation));
            continue;
        }
        for (const auto &replacement :
             replacement_names) {
            appendUnique(rewritten, replacement);
        }
    }
    setRelationList(
        pass, field, std::move(rewritten));
}

void spliceReplacement(
    nlohmann::json &graph,
    std::span<const std::size_t> indices,
    std::span<const std::string> source_names,
    nlohmann::json replacement,
    std::span<const std::string> replacement_names) {
    auto &passes = graph.at("passes");
    std::set<std::string, std::less<>>
        source_name_set(
            source_names.begin(), source_names.end());
    std::vector<std::string> incoming;
    std::vector<std::string> outgoing;
    for (const auto index : indices) {
        for (auto relation :
             relationList(
                 passes.at(index), "after")) {
            if (!source_name_set.contains(relation)) {
                appendUnique(
                    incoming, std::move(relation));
            }
        }
        for (auto relation :
             relationList(
                 passes.at(index), "before")) {
            if (!source_name_set.contains(relation)) {
                appendUnique(
                    outgoing, std::move(relation));
            }
        }
    }
    for (auto &pass : replacement) {
        for (const auto field :
             {"after", "before"}) {
            for (const auto &relation :
                 relationList(pass, field)) {
                if (source_name_set.contains(relation) &&
                    std::find(
                        replacement_names.begin(),
                        replacement_names.end(),
                        relation) ==
                        replacement_names.end()) {
                    throw std::runtime_error(
                        "replacement pass relation references "
                        "a removed source node: " +
                        relation);
                }
            }
        }
        appendRelations(pass, "after", incoming);
        appendRelations(pass, "before", outgoing);
    }

    for (std::size_t index = 0;
         index < passes.size(); ++index) {
        if (index >= indices.front() &&
            index <= indices.back()) {
            continue;
        }
        rewriteExternalRelations(
            passes.at(index), "after",
            source_name_set, replacement_names);
        rewriteExternalRelations(
            passes.at(index), "before",
            source_name_set, replacement_names);
    }

    auto next = nlohmann::json::array();
    for (std::size_t index = 0;
         index < indices.front(); ++index) {
        next.push_back(passes.at(index));
    }
    for (auto &pass : replacement) {
        next.push_back(std::move(pass));
    }
    for (std::size_t index =
             indices.back() + 1;
         index < passes.size(); ++index) {
        next.push_back(passes.at(index));
    }
    passes = std::move(next);
}

} // namespace

struct SubgraphReplacementRegistryState {
    struct ProviderSlot {
        std::uint32_t generation = 0;
        bool active = false;
        internal::RegistrationOwner owner =
            internal::engineRegistrationOwner;
        std::uint32_t provider_version = 0;
        std::uint64_t capability_bits = 0;
        std::string name;
        void *context = nullptr;
        RenderSubgraph::ResolveRegionV1Fn
            resolve_region = nullptr;
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

struct SubgraphReplacementRegistrySnapshot::Impl {
    std::shared_lock<std::shared_mutex> lock;
    const SubgraphReplacementRegistryState *registry =
        nullptr;

    Impl(
        std::shared_lock<std::shared_mutex>
            registry_lock,
        const SubgraphReplacementRegistryState
            *value) noexcept
        : lock{std::move(registry_lock)},
          registry{value} {}
};

TaggedRegionContract makeTaggedRegionContract(
    const CompiledLogicalRenderGraph &graph,
    std::string_view region_tag) {
    if (graph.name.empty() ||
        graph.name.size() >
            RenderSubgraph::maximumGraphNameBytesV1 ||
        region_tag.empty() ||
        region_tag.size() >
            RenderSubgraph::maximumRegionTagBytesV1) {
        throw std::runtime_error(
            "tagged region contract graph or region name "
            "is empty or too long");
    }
    std::set<std::string, std::less<>>
        region_nodes;
    for (const auto &node : graph.nodes) {
        if (std::find(
                node.region_tags.begin(),
                node.region_tags.end(),
                region_tag) ==
            node.region_tags.end()) {
            continue;
        }
        region_nodes.insert(node.name);
    }
    if (region_nodes.empty()) {
        throw std::runtime_error(
            "tagged region has no logical nodes: " +
            std::string{region_tag});
    }

    std::map<std::string, const LogicalResourceDesc *,
             std::less<>>
        resources;
    for (const auto &resource : graph.resources) {
        resources.emplace(resource.name, &resource);
    }
    std::set<LogicalValueId> produced_inside;
    std::set<LogicalValueId> consumed_outside;
    for (const auto &node : graph.nodes) {
        const auto inside =
            region_nodes.contains(node.name);
        for (const auto &use : node.uses) {
            if (inside && use.output_value) {
                produced_inside.insert(
                    *use.output_value);
            }
            if (!inside && use.input_value) {
                consumed_outside.insert(
                    *use.input_value);
            }
        }
    }

    using PortKey =
        std::pair<RenderSubgraph::BoundaryDirectionV1,
                  std::string>;
    std::map<PortKey,
             TaggedRegionBoundaryPortContract>
        boundary;
    const auto add_port =
        [&](const LogicalValueId &value,
            RenderSubgraph::BoundaryDirectionV1
                direction) {
            const auto resource =
                resources.find(value.resource);
            if (resource == resources.end()) {
                throw std::runtime_error(
                    "tagged region boundary references "
                    "unknown resource: " +
                    value.resource);
            }
            if (value.resource.size() >
                RenderSubgraph::
                    maximumResourceNameBytesV1) {
                throw std::runtime_error(
                    "tagged region boundary resource name "
                    "is too long: " +
                    value.resource);
            }
            const auto type_json =
                logicalTypeToJson(
                    resource->second->type)
                    .dump();
            if (type_json.empty() ||
                type_json.size() >
                    RenderSubgraph::
                        maximumTypeJsonBytesV1) {
                throw std::runtime_error(
                    "tagged region boundary type JSON is "
                    "empty or too large: " +
                    value.resource);
            }
            boundary.try_emplace(
                PortKey{direction, value.resource},
                TaggedRegionBoundaryPortContract{
                    .resource = value.resource,
                    .type_json = type_json,
                    .direction = direction,
                    .materialization =
                        materialization(
                            resource->second
                                ->materialization),
                });
        };
    for (const auto &node : graph.nodes) {
        if (!region_nodes.contains(node.name)) {
            continue;
        }
        for (const auto &use : node.uses) {
            if (use.input_value &&
                !produced_inside.contains(
                    *use.input_value)) {
                add_port(
                    *use.input_value,
                    RenderSubgraph::
                        BoundaryDirectionV1::input);
            }
            if (use.output_value) {
                const auto resource =
                    resources.find(
                        use.output_value->resource);
                if (resource == resources.end()) {
                    throw std::runtime_error(
                        "tagged region output references "
                        "unknown resource: " +
                        use.output_value->resource);
                }
                const auto materialization =
                    resource->second->materialization;
                const auto externally_observable =
                    materialization ==
                        LogicalMaterializationRequirement::
                            external;
                if (!consumed_outside.contains(
                        *use.output_value) &&
                    !externally_observable) {
                    continue;
                }
                add_port(
                    *use.output_value,
                    RenderSubgraph::
                        BoundaryDirectionV1::output);
            }
        }
    }

    TaggedRegionContract result;
    result.id =
        RenderSubgraph::regionContractIdV1;
    result.graph_name = graph.name;
    result.region_tag = region_tag;
    for (const auto &node : graph.nodes) {
        if (region_nodes.contains(node.name)) {
            result.node_names.push_back(node.name);
        }
    }
    result.boundary_ports.reserve(boundary.size());
    for (auto &[unused, port] : boundary) {
        (void)unused;
        result.boundary_ports.push_back(
            std::move(port));
    }

    StableFingerprint fingerprint;
    fingerprint.appendString(result.id);
    fingerprint.appendUnsigned(
        result.boundary_ports.size());
    for (const auto &port :
         result.boundary_ports) {
        fingerprint.appendString(port.resource);
        fingerprint.appendString(port.type_json);
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(
                port.direction));
        fingerprint.appendUnsigned(
            static_cast<std::uint32_t>(
                port.materialization));
    }
    result.fingerprint = fingerprint.value();
    return result;
}

SubgraphReplacementRegistrySnapshot::
    SubgraphReplacementRegistrySnapshot() noexcept =
        default;
SubgraphReplacementRegistrySnapshot::
    ~SubgraphReplacementRegistrySnapshot() = default;
SubgraphReplacementRegistrySnapshot::
    SubgraphReplacementRegistrySnapshot(
        SubgraphReplacementRegistrySnapshot &&) noexcept =
        default;
SubgraphReplacementRegistrySnapshot &
SubgraphReplacementRegistrySnapshot::operator=(
    SubgraphReplacementRegistrySnapshot &&) noexcept =
    default;

SubgraphReplacementRegistrySnapshot::
    SubgraphReplacementRegistrySnapshot(
        std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

ResolvedTaggedRegionReplacement
SubgraphReplacementRegistrySnapshot::resolveRegion(
    const TaggedRegionContract &contract,
    std::string_view authored_subgraph_json,
    const std::optional<std::string>
        &requested_provider) const {
    if (impl_ == nullptr ||
        impl_->registry == nullptr) {
        throw std::runtime_error(
            "subgraph replacement registry snapshot is "
            "empty");
    }
    if (contract.id !=
            RenderSubgraph::regionContractIdV1 ||
        contract.graph_name.empty() ||
        contract.graph_name.size() >
            RenderSubgraph::maximumGraphNameBytesV1 ||
        contract.region_tag.empty() ||
        contract.region_tag.size() >
            RenderSubgraph::maximumRegionTagBytesV1) {
        throw std::runtime_error(
            "subgraph replacement contract envelope is "
            "invalid");
    }
    for (const auto &port :
         contract.boundary_ports) {
        if (port.resource.empty() ||
            port.resource.size() >
                RenderSubgraph::
                    maximumResourceNameBytesV1 ||
            port.type_json.empty() ||
            port.type_json.size() >
                RenderSubgraph::
                    maximumTypeJsonBytesV1) {
            throw std::runtime_error(
                "subgraph replacement boundary port is "
                "invalid for region '" +
                contract.region_tag + "'");
        }
    }
    const auto &registry = *impl_->registry;
    const auto requested =
        requested_provider.value_or(
            std::string{
                builtinTaggedSubgraphReplacementProvider});
    if (requested.empty()) {
        throw std::runtime_error(
            "subgraph replacement provider name must not "
            "be empty: " +
            contract.region_tag);
    }

    const auto find_for_owner =
        [&](internal::RegistrationOwner owner)
        -> const SubgraphReplacementRegistryState::
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
    const SubgraphReplacementRegistryState::
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
            "subgraph replacement provider '" +
            requested +
            "' is not registered for the active owner "
            "while compiling region '" +
            contract.region_tag + "'");
    }

    std::vector<RenderSubgraph::BoundaryPortV1>
        abi_ports;
    abi_ports.reserve(
        contract.boundary_ports.size());
    for (const auto &port :
         contract.boundary_ports) {
        auto abi =
            RenderSubgraph::descriptor<
                RenderSubgraph::BoundaryPortV1>();
        abi.resource_utf8 = port.resource.data();
        abi.resource_size =
            abiSize(
                port.resource.size(), "resource name",
                contract.region_tag);
        abi.type_json_utf8 = port.type_json.data();
        abi.type_json_size =
            abiSize(
                port.type_json.size(), "type JSON",
                contract.region_tag);
        abi.direction = port.direction;
        abi.materialization =
            port.materialization;
        abi_ports.push_back(abi);
    }

    auto abi_contract =
        RenderSubgraph::descriptor<
            RenderSubgraph::RegionContractV1>();
    abi_contract.contract_id_utf8 =
        contract.id.data();
    abi_contract.contract_id_size =
        abiSize(
            contract.id.size(), "contract id",
            contract.region_tag);
    abi_contract.graph_name_utf8 =
        contract.graph_name.data();
    abi_contract.graph_name_size =
        abiSize(
            contract.graph_name.size(), "graph name",
            contract.region_tag);
    abi_contract.region_tag_utf8 =
        contract.region_tag.data();
    abi_contract.region_tag_size =
        abiSize(
            contract.region_tag.size(), "region tag",
            contract.region_tag);
    abi_contract.boundary_ports =
        abi_ports.empty() ? nullptr
                          : abi_ports.data();
    abi_contract.boundary_port_count =
        abiSize(
            abi_ports.size(), "boundary port count",
            contract.region_tag);
    abi_contract.fingerprint =
        contract.fingerprint;

    if (authored_subgraph_json.empty() ||
        authored_subgraph_json.size() >
            RenderSubgraph::
                maximumSubgraphJsonBytesV1) {
        throw std::runtime_error(
            "authored subgraph JSON is empty or too large "
            "for region '" +
            contract.region_tag + "'");
    }
    auto input =
        RenderSubgraph::descriptor<
            RenderSubgraph::ResolveRegionInputV1>();
    input.contract = &abi_contract;
    input.authored_subgraph_json_utf8 =
        authored_subgraph_json.data();
    input.authored_subgraph_json_size =
        abiSize(
            authored_subgraph_json.size(),
            "authored subgraph JSON",
            contract.region_tag);
    auto output =
        RenderSubgraph::descriptor<
            RenderSubgraph::RegionReplacementV1>();
    const auto status = selected->resolve_region(
        selected->context, &input, &output);
    if (status != Status::ok) {
        throw std::runtime_error(
            "subgraph replacement provider '" +
            requested + "' failed for region '" +
            contract.region_tag + "' with status " +
            statusName(status));
    }
    validateOutputEnvelope(
        output, requested, contract.region_tag);
    auto implementation = copyProviderString(
        output.implementation_id_utf8,
        output.implementation_id_size,
        RenderSubgraph::
            maximumImplementationIdBytesV1,
        "implementation id", requested,
        contract.region_tag);
    try {
        (void)parseSemanticTypeId(implementation);
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "subgraph replacement provider '" +
            requested +
            "' returned invalid implementation id '" +
            implementation + "' for region '" +
            contract.region_tag + "': " +
            error.what());
    }
    auto subgraph_json = copyProviderString(
        output.subgraph_json_utf8,
        output.subgraph_json_size,
        RenderSubgraph::maximumSubgraphJsonBytesV1,
        "subgraph JSON", requested,
        contract.region_tag);

    const auto slot_index =
        static_cast<std::uint64_t>(
            selected - registry.providers.data());
    return {
        .subgraph_json = std::move(subgraph_json),
        .selection =
            LogicalSubgraphReplacementSelection{
                .region = contract.region_tag,
                .provider = requested,
                .implementation =
                    std::move(implementation),
                .contract = contract.id,
                .contract_fingerprint =
                    contract.fingerprint,
                .provider_owner = selected->owner,
                .provider_identity =
                    slot_index + 1,
                .provider_generation =
                    selected->generation,
                .provider_version =
                    selected->provider_version,
                .provider_capability_bits =
                    selected->capability_bits,
                .explicitly_selected =
                    requested_provider.has_value(),
                .source_nodes =
                    contract.node_names,
            },
    };
}

SubgraphReplacementRegistry::
    SubgraphReplacementRegistry()
    : impl_{
          std::make_unique<
              SubgraphReplacementRegistryState>()} {
    auto provider =
        RenderSubgraph::descriptor<
            RenderSubgraph::ProviderV1>();
    provider.capability_bits =
        RenderSubgraph::
            builtinProviderCapabilitiesV1;
    provider.name_utf8 =
        builtinTaggedSubgraphReplacementProvider.data();
    provider.name_size =
        static_cast<std::uint32_t>(
            builtinTaggedSubgraphReplacementProvider
                .size());
    provider.resolve_region =
        builtinResolveRegion;
    RenderSubgraph::ProviderHandleV1 handle{};
    if (registerProvider(
            provider,
            internal::engineRegistrationOwner,
            handle) != Status::ok) {
        throw std::logic_error(
            "failed to register builtin tagged subgraph "
            "replacement provider");
    }
}

SubgraphReplacementRegistry::
    ~SubgraphReplacementRegistry() = default;

RenderSubgraph::Status
SubgraphReplacementRegistry::registerProvider(
    const RenderSubgraph::ProviderV1 &provider,
    internal::RegistrationOwner owner,
    RenderSubgraph::ProviderHandleV1
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
        slot.resolve_region =
            provider.resolve_region;
        out_handle =
            RenderSubgraph::ProviderHandleV1{
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

RenderSubgraph::Status
SubgraphReplacementRegistry::unregisterProvider(
    RenderSubgraph::ProviderHandleV1 handle,
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
    slot.resolve_region = nullptr;
    impl_->free_slots.push_back(index);
    return Status::ok;
}

SubgraphReplacementRegistrySnapshot
SubgraphReplacementRegistry::snapshot() const {
    std::shared_lock lock{impl_->mutex};
    return SubgraphReplacementRegistrySnapshot{
        std::make_unique<
            SubgraphReplacementRegistrySnapshot::Impl>(
            std::move(lock), impl_.get())};
}

void SubgraphReplacementRegistry::activateOwner(
    internal::RegistrationOwner owner) noexcept {
    std::unique_lock lock{impl_->mutex};
    impl_->active_game_owner =
        !impl_->ownerIsRetired(owner) &&
                internal::isRegistrationOwnerCurrent(owner)
            ? owner
            : internal::engineRegistrationOwner;
}

void SubgraphReplacementRegistry::releaseOwner(
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
        slot.resolve_region = nullptr;
        impl_->free_slots.push_back(index);
    }
}

ResolvedTaggedSubgraphConfig
resolveTaggedSubgraphReplacements(
    const nlohmann::json &config,
    const SubgraphReplacementRegistrySnapshot
        &providers) {
    ResolvedTaggedSubgraphConfig result{
        .config = config,
    };
    const auto names = graphNames(result.config);
    for (const auto &name : names) {
        const auto requests = parseRegionRequests(
            findGraphObject(result.config, name));
        if (requests.empty()) continue;

        ResolvedTaggedSubgraphGraph
            resolved_graph{
                .graph = name,
            };
        for (const auto &request : requests) {
            const auto before_state =
                compileConfigState(result.config);
            const auto &before_graph =
                findLogicalGraph(
                    before_state, name);
            const auto contract =
                makeTaggedRegionContract(
                    before_graph,
                    request.region);

            auto &graph =
                findGraphObject(
                    result.config, name);
            if (!graph.contains("passes") ||
                !graph.at("passes").is_array()) {
                throw std::runtime_error(
                    "tagged region v1 graph requires a "
                    "pass array: " +
                    name);
            }
            const auto &passes =
                graph.at("passes");
            std::vector<std::size_t> indices;
            for (std::size_t index = 0;
                 index < passes.size(); ++index) {
                if (hasRegion(
                        passes.at(index),
                        request.region)) {
                    indices.push_back(index);
                }
            }
            if (indices.empty()) {
                throw std::runtime_error(
                    "tagged region request has no authored "
                    "passes: " +
                    request.region);
            }
            for (std::size_t offset = 0;
                 offset < indices.size(); ++offset) {
                if (indices[offset] !=
                    indices.front() + offset) {
                    throw std::runtime_error(
                        "tagged region v1 requires contiguous "
                        "authored passes: " +
                        request.region);
                }
            }
            const auto source_names =
                sourcePassNames(
                    passes, indices,
                    request.region);
            if (source_names !=
                contract.node_names) {
                throw std::runtime_error(
                    "tagged region v1 cannot mix pass and "
                    "non-pass logical nodes: " +
                    request.region);
            }
            auto inherited_regions =
                commonRegionTags(
                    passes, indices);
            if (std::find(
                    inherited_regions.begin(),
                    inherited_regions.end(),
                    request.region) ==
                inherited_regions.end()) {
                inherited_regions.push_back(
                    request.region);
            }
            auto authored =
                nlohmann::json::array();
            for (const auto index : indices) {
                authored.push_back(
                    passes.at(index));
            }
            auto resolved = providers.resolveRegion(
                contract, authored.dump(),
                request.provider);
            nlohmann::json replacement;
            try {
                replacement =
                    nlohmann::json::parse(
                        resolved.subgraph_json);
            } catch (const std::exception &error) {
                throw std::runtime_error(
                    "subgraph replacement provider '" +
                    resolved.selection.provider +
                    "' returned malformed JSON for region '" +
                    request.region + "': " +
                    error.what());
            }
            auto replacement_names =
                replacementPassNames(
                    replacement,
                    inherited_regions,
                    resolved.selection.provider,
                    request.region);

            auto candidate = result.config;
            auto &candidate_graph =
                findGraphObject(candidate, name);
            spliceReplacement(
                candidate_graph, indices,
                source_names,
                std::move(replacement),
                replacement_names);
            const auto after_state =
                compileConfigState(candidate);
            const auto &after_graph =
                findLogicalGraph(
                    after_state, name);
            requireSameResources(
                before_graph, after_graph,
                request.region);
            const auto after_contract =
                makeTaggedRegionContract(
                    after_graph,
                    request.region);
            if (after_contract.boundary_ports !=
                    contract.boundary_ports ||
                after_contract.fingerprint !=
                    contract.fingerprint) {
                throw std::runtime_error(
                    "subgraph replacement changed tagged "
                    "region boundary contract: " +
                    request.region);
            }
            if (after_contract.node_names !=
                replacement_names) {
                throw std::runtime_error(
                    "subgraph replacement candidate node "
                    "provenance is inconsistent: " +
                    request.region);
            }

            resolved.selection.replacement_nodes =
                std::move(replacement_names);
            resolved_graph.selections.push_back(
                std::move(resolved.selection));
            result.config = std::move(candidate);
        }
        result.graphs.push_back(
            std::move(resolved_graph));
    }
    return result;
}

void applyResolvedTaggedSubgraphSelections(
    std::span<FrameGraphDefinition> frame_graphs,
    std::span<const ResolvedTaggedSubgraphGraph>
        resolved_graphs) {
    std::set<std::string, std::less<>>
        applied;
    for (auto &frame_graph : frame_graphs) {
        const auto found = std::find_if(
            resolved_graphs.begin(),
            resolved_graphs.end(),
            [&](const auto &resolved) {
                return resolved.graph ==
                       frame_graph.name;
            });
        if (found == resolved_graphs.end()) {
            continue;
        }
        if (!applied.insert(
                 frame_graph.name)
                 .second ||
            !frame_graph.subgraph_replacements
                 .empty()) {
            throw std::runtime_error(
                "resolved tagged subgraph selections are "
                "ambiguous: " +
                frame_graph.name);
        }
        frame_graph.subgraph_replacements =
            found->selections;
    }
    for (const auto &resolved : resolved_graphs) {
        if (!applied.contains(resolved.graph)) {
            throw std::runtime_error(
                "resolved tagged subgraph graph was not "
                "compiled: " +
                resolved.graph);
        }
    }
}

SubgraphReplacementRegistry &
subgraphReplacementRegistry() {
    static auto *value =
        new SubgraphReplacementRegistry;
    return *value;
}

namespace render_subgraph_internal {

void activateProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        subgraphReplacementRegistry()
            .activateOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

void releaseProviderOwner(
    internal::RegistrationOwner owner) noexcept {
    try {
        subgraphReplacementRegistry()
            .releaseOwner(owner);
    } catch (...) {
        std::terminate();
    }
}

} // namespace render_subgraph_internal

namespace {

Status registerProviderApi(
    void *context,
    const RenderSubgraph::ProviderV1 *provider,
    RenderSubgraph::ProviderHandleV1
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
        return subgraphReplacementRegistry()
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
    RenderSubgraph::ProviderHandleV1
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
        return subgraphReplacementRegistry()
            .unregisterProvider(handle, owner);
    } catch (...) {
        return Status::provider_error;
    }
}

} // namespace

namespace RenderSubgraph {

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
        (void)subgraphReplacementRegistry();
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

} // namespace RenderSubgraph

} // namespace Pelican
