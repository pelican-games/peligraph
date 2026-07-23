#include "targetplanning.hpp"

#include "logicalrendertype.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

void requireNonEmpty(std::string_view value, std::string_view subject) {
    if (value.empty()) {
        throw std::runtime_error(std::string{subject} + " must not be empty");
    }
}

void requireVersionedName(std::string_view value, std::string_view subject) {
    requireNonEmpty(value, subject);
    try {
        const auto parsed = parseSemanticTypeId(value);
        if (semanticTypeIdName(parsed) != value) {
            throw std::runtime_error("name is not canonical");
        }
    } catch (const std::runtime_error &error) {
        throw std::runtime_error(std::string{subject} +
                                 " must use namespace.name@major: " +
                                 std::string{value} + " (" + error.what() + ")");
    }
}

void canonicalizeVersionedNames(std::vector<std::string> &values,
                                std::string_view subject) {
    for (const auto &value : values) requireVersionedName(value, subject);
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

void validateProviderDescriptor(const CompilerProviderDescriptor &provider) {
    requireVersionedName(provider.id, "compiler provider id");
    const auto has_identity = provider.owner_identity != 0;
    const auto has_generation = provider.owner_generation != 0;
    if (has_identity != has_generation) {
        throw std::runtime_error(
            "compiler provider owner identity and generation must both be zero "
            "for static providers or both be non-zero for reloadable providers: " +
            provider.id);
    }
    requireNonEmpty(provider.content_hash, "compiler provider content hash");
}

auto providerKey(const CompilerProviderDescriptor &provider) {
    return std::tuple{provider.kind, provider.id};
}

bool hasCapability(std::span<const std::string> capabilities,
                   std::string_view required) {
    return std::binary_search(capabilities.begin(), capabilities.end(), required);
}

nlohmann::ordered_json providerToJson(
    const CompilerProviderDescriptor &provider) {
    return nlohmann::ordered_json{
        {"id", provider.id},
        {"kind", compilerProviderKindName(provider.kind)},
        {"owner_identity", provider.owner_identity},
        {"owner_generation", provider.owner_generation},
        {"content_hash", provider.content_hash},
        {"fingerprint", compilerProviderFingerprint(provider)},
    };
}

nlohmann::ordered_json costToJson(const BackendCostEstimate &cost) {
    return nlohmann::ordered_json{
        {"rendering_scopes", cost.rendering_scopes},
        {"materialized_resources", cost.materialized_resources},
        {"external_stores", cost.external_stores},
        {"transient_bytes", cost.transient_bytes},
        {"bandwidth_class", cost.bandwidth_class},
    };
}

auto costKey(const BackendProbeResult &result) {
    return std::tuple{
        result.cost.rendering_scopes,
        result.cost.materialized_resources,
        result.cost.external_stores,
        result.cost.transient_bytes,
        result.cost.bandwidth_class,
        result.candidate,
    };
}

nlohmann::ordered_json diagnosticToJson(
    const PlanningDiagnostic &diagnostic) {
    return nlohmann::ordered_json{
        {"id", diagnostic.id},
        {"severity", planningDiagnosticSeverityName(diagnostic.severity)},
        {"subject", diagnostic.subject},
        {"detail", diagnostic.detail},
    };
}

nlohmann::ordered_json probeResultToJson(const BackendProbeResult &result) {
    nlohmann::ordered_json encoded{
        {"candidate", result.candidate},
        {"provider", providerToJson(result.provider)},
        {"endpoint", result.endpoint},
        {"feasible", result.feasible},
        {"required_physical_features", result.required_physical_features},
        {"cost", costToJson(result.cost)},
    };
    if (result.selected_link) {
        encoded["selected_link"] = *result.selected_link;
    }
    encoded["bridge_offers"] = result.bridge_offers;
    encoded["failures"] = nlohmann::ordered_json::array();
    for (const auto &failure : result.failures) {
        encoded["failures"].push_back(nlohmann::ordered_json{
            {"id", failure.id},
            {"subject", failure.subject},
            {"detail", failure.detail},
        });
    }
    encoded["diagnostics"] = nlohmann::ordered_json::array();
    for (const auto &diagnostic : result.diagnostics) {
        encoded["diagnostics"].push_back(diagnosticToJson(diagnostic));
    }
    return encoded;
}

PlanningNamePair canonicalPair(PlanningNamePair pair,
                               std::string_view subject) {
    requireNonEmpty(pair.first, subject);
    requireNonEmpty(pair.second, subject);
    if (pair.first == pair.second) {
        throw std::runtime_error(std::string{subject} +
                                 " must contain two distinct names: " +
                                 pair.first);
    }
    if (pair.second < pair.first) std::swap(pair.first, pair.second);
    return pair;
}

std::uint64_t stableRank(std::uint64_t seed, std::uint64_t salt,
                         std::string_view value) {
    std::uint64_t hash = 1469598103934665603ULL;
    auto add_byte = [&](std::uint8_t byte) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    };
    for (std::uint32_t shift = 0; shift < 64; shift += 8) {
        add_byte(static_cast<std::uint8_t>(seed >> shift));
        add_byte(static_cast<std::uint8_t>(salt >> shift));
    }
    for (const auto character : value) {
        add_byte(static_cast<std::uint8_t>(character));
    }
    hash ^= hash >> 30U;
    hash *= 0xbf58476d1ce4e5b9ULL;
    hash ^= hash >> 27U;
    hash *= 0x94d049bb133111ebULL;
    return hash ^ (hash >> 31U);
}

std::string pairKey(const PlanningNamePair &pair) {
    return pair.first + '\n' + pair.second;
}

template <typename Value>
void sortAndUnique(std::vector<Value> &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

nlohmann::ordered_json pairToJson(const PlanningNamePair &pair) {
    return nlohmann::ordered_json{{"first", pair.first},
                                  {"second", pair.second}};
}

} // namespace

std::string_view targetEndpointKindName(TargetEndpointKind kind) {
    switch (kind) {
    case TargetEndpointKind::host: return "host";
    case TargetEndpointKind::vulkan_device: return "vulkan_device";
    case TargetEndpointKind::external: return "external";
    }
    throw std::runtime_error("unknown target endpoint kind");
}

TargetTopologySnapshot canonicalizeTargetTopology(
    TargetTopologySnapshot topology) {
    requireNonEmpty(topology.name, "target topology name");
    for (auto &endpoint : topology.endpoints) {
        requireNonEmpty(endpoint.id, "target endpoint id");
        canonicalizeVersionedNames(endpoint.capabilities,
                                   "target endpoint capability");
        for (const auto &fact : endpoint.facts) {
            requireVersionedName(fact.name, "target endpoint fact");
            requireNonEmpty(fact.value, "target endpoint fact value");
        }
        std::sort(endpoint.facts.begin(), endpoint.facts.end(),
                  [](const auto &left, const auto &right) {
                      return left.name < right.name;
                  });
        for (std::size_t index = 1; index < endpoint.facts.size(); ++index) {
            if (endpoint.facts[index - 1].name == endpoint.facts[index].name) {
                throw std::runtime_error(
                    "duplicate target endpoint fact '" +
                    endpoint.facts[index].name + "' on endpoint '" +
                    endpoint.id + "'");
            }
        }
    }
    std::sort(topology.endpoints.begin(), topology.endpoints.end(),
              [](const auto &left, const auto &right) {
                  return left.id < right.id;
              });
    for (std::size_t index = 1; index < topology.endpoints.size(); ++index) {
        if (topology.endpoints[index - 1].id == topology.endpoints[index].id) {
            throw std::runtime_error("duplicate target endpoint: " +
                                     topology.endpoints[index].id);
        }
    }

    for (auto &link : topology.links) {
        requireNonEmpty(link.id, "target endpoint link id");
        requireNonEmpty(link.source_endpoint,
                        "target endpoint link source");
        requireNonEmpty(link.destination_endpoint,
                        "target endpoint link destination");
        canonicalizeVersionedNames(link.capabilities,
                                   "target endpoint link capability");
        canonicalizeVersionedNames(link.bridge_offers,
                                   "target endpoint bridge offer");
        if (findTargetEndpoint(topology, link.source_endpoint) == nullptr) {
            throw std::runtime_error("target endpoint link '" + link.id +
                                     "' references unknown source endpoint '" +
                                     link.source_endpoint + "'");
        }
        if (findTargetEndpoint(topology, link.destination_endpoint) == nullptr) {
            throw std::runtime_error(
                "target endpoint link '" + link.id +
                "' references unknown destination endpoint '" +
                link.destination_endpoint + "'");
        }
    }
    std::sort(topology.links.begin(), topology.links.end(),
              [](const auto &left, const auto &right) {
                  return left.id < right.id;
              });
    for (std::size_t index = 1; index < topology.links.size(); ++index) {
        if (topology.links[index - 1].id == topology.links[index].id) {
            throw std::runtime_error("duplicate target endpoint link: " +
                                     topology.links[index].id);
        }
    }
    return topology;
}

const TargetEndpoint *findTargetEndpoint(
    const TargetTopologySnapshot &topology, std::string_view id) noexcept {
    const auto found = std::find_if(
        topology.endpoints.begin(), topology.endpoints.end(),
        [&](const TargetEndpoint &endpoint) { return endpoint.id == id; });
    return found != topology.endpoints.end() ? &*found : nullptr;
}

const TargetEndpointLink *findTargetEndpointLink(
    const TargetTopologySnapshot &topology, std::string_view id) noexcept {
    const auto found = std::find_if(
        topology.links.begin(), topology.links.end(),
        [&](const TargetEndpointLink &link) { return link.id == id; });
    return found != topology.links.end() ? &*found : nullptr;
}

nlohmann::ordered_json targetTopologySnapshotToJson(
    const TargetTopologySnapshot &source) {
    const auto topology = canonicalizeTargetTopology(source);
    nlohmann::ordered_json result{
        {"schema", "pelican.target_topology"},
        {"version", 1},
        {"name", topology.name},
    };
    result["endpoints"] = nlohmann::ordered_json::array();
    for (const auto &endpoint : topology.endpoints) {
        nlohmann::ordered_json encoded{
            {"id", endpoint.id},
            {"kind", targetEndpointKindName(endpoint.kind)},
            {"capabilities", endpoint.capabilities},
        };
        encoded["facts"] = nlohmann::ordered_json::array();
        for (const auto &fact : endpoint.facts) {
            encoded["facts"].push_back(
                nlohmann::ordered_json{{"name", fact.name},
                                       {"value", fact.value}});
        }
        result["endpoints"].push_back(std::move(encoded));
    }
    result["links"] = nlohmann::ordered_json::array();
    for (const auto &link : topology.links) {
        result["links"].push_back(nlohmann::ordered_json{
            {"id", link.id},
            {"source", link.source_endpoint},
            {"destination", link.destination_endpoint},
            {"capabilities", link.capabilities},
            {"bridge_offers", link.bridge_offers},
        });
    }
    return result;
}

std::string_view compilerProviderKindName(CompilerProviderKind kind) {
    switch (kind) {
    case CompilerProviderKind::conversion: return "conversion";
    case CompilerProviderKind::target_lowering: return "target_lowering";
    }
    throw std::runtime_error("unknown compiler provider kind");
}

std::string compilerProviderFingerprint(
    const CompilerProviderDescriptor &provider) {
    validateProviderDescriptor(provider);
    std::ostringstream stream;
    stream << provider.id << ':' << compilerProviderKindName(provider.kind)
           << ':' << provider.owner_identity << ':'
           << provider.owner_generation << ':' << provider.content_hash;
    return stream.str();
}

CompilerProviderLease::CompilerProviderLease(
    CompilerProviderDescriptor descriptor,
    std::shared_ptr<const void> generation_lease)
    : descriptor_{std::move(descriptor)},
      generation_lease_{std::move(generation_lease)} {}

CompilerProviderRegistrySnapshot::CompilerProviderRegistrySnapshot(
    std::vector<CompilerProviderLease> providers)
    : providers_{std::move(providers)} {}

const CompilerProviderLease &CompilerProviderRegistrySnapshot::require(
    CompilerProviderKind kind, std::string_view id) const {
    const auto found = std::lower_bound(
        providers_.begin(), providers_.end(), std::tuple{kind, id},
        [](const CompilerProviderLease &provider, const auto &candidate) {
            return providerKey(provider.descriptor()) < candidate;
        });
    if (found == providers_.end() || found->descriptor().kind != kind ||
        found->descriptor().id != id) {
        throw std::runtime_error(
            "compiler provider is not registered in snapshot: " +
            std::string{compilerProviderKindName(kind)} + " '" +
            std::string{id} + "'");
    }
    return *found;
}

void CompilerProviderRegistry::registerProvider(
    CompilerProviderDescriptor descriptor,
    std::shared_ptr<const void> generation_lease) {
    validateProviderDescriptor(descriptor);
    if (descriptor.owner_identity != 0 && generation_lease == nullptr) {
        throw std::runtime_error(
            "reloadable compiler provider requires a generation lease: " +
            descriptor.id);
    }
    std::unique_lock lock{mutex_};
    const auto key = providerKey(descriptor);
    const auto found = std::lower_bound(
        providers_.begin(), providers_.end(), key,
        [](const CompilerProviderLease &provider, const auto &candidate) {
            return providerKey(provider.descriptor()) < candidate;
        });
    if (found != providers_.end() &&
        providerKey(found->descriptor()) == key) {
        throw std::runtime_error("duplicate compiler provider: " +
                                 descriptor.id);
    }
    providers_.insert(
        found, CompilerProviderLease{std::move(descriptor),
                                     std::move(generation_lease)});
}

bool CompilerProviderRegistry::unregisterProvider(
    CompilerProviderKind kind, std::string_view id) {
    std::unique_lock lock{mutex_};
    const auto found = std::lower_bound(
        providers_.begin(), providers_.end(), std::tuple{kind, id},
        [](const CompilerProviderLease &provider, const auto &candidate) {
            return providerKey(provider.descriptor()) < candidate;
        });
    if (found == providers_.end() || found->descriptor().kind != kind ||
        found->descriptor().id != id) {
        return false;
    }
    providers_.erase(found);
    return true;
}

CompilerProviderRegistrySnapshot CompilerProviderRegistry::snapshot() const {
    std::shared_lock lock{mutex_};
    return CompilerProviderRegistrySnapshot{providers_};
}

std::string_view planningDiagnosticSeverityName(
    PlanningDiagnosticSeverity severity) {
    switch (severity) {
    case PlanningDiagnosticSeverity::info: return "info";
    case PlanningDiagnosticSeverity::warning: return "warning";
    case PlanningDiagnosticSeverity::error: return "error";
    }
    throw std::runtime_error("unknown planning diagnostic severity");
}

std::vector<PlanningDiagnostic> applyPlanningDiagnosticPolicy(
    std::span<const PlanningDiagnostic> diagnostics,
    const PlanningDiagnosticPolicy &policy) {
    auto strict = policy.strict_warning_ids;
    canonicalizeVersionedNames(strict, "strict planning diagnostic id");

    std::vector<PlanningDiagnostic> result{diagnostics.begin(),
                                           diagnostics.end()};
    for (auto &diagnostic : result) {
        requireVersionedName(diagnostic.id, "planning diagnostic id");
        requireNonEmpty(diagnostic.subject, "planning diagnostic subject");
        requireNonEmpty(diagnostic.detail, "planning diagnostic detail");
        if (diagnostic.severity == PlanningDiagnosticSeverity::warning &&
            std::binary_search(strict.begin(), strict.end(), diagnostic.id)) {
            diagnostic.severity = PlanningDiagnosticSeverity::error;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto &left,
                                               const auto &right) {
        return std::tie(left.id, left.subject, left.severity, left.detail) <
               std::tie(right.id, right.subject, right.severity, right.detail);
    });
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void requireNoPlanningErrors(
    std::span<const PlanningDiagnostic> diagnostics) {
    const auto error = std::find_if(
        diagnostics.begin(), diagnostics.end(), [](const auto &diagnostic) {
            return diagnostic.severity == PlanningDiagnosticSeverity::error;
        });
    if (error != diagnostics.end()) {
        throw std::runtime_error(
            "planning diagnostic is an error: " + error->id + " (" +
            error->subject + ": " + error->detail + ")");
    }
}

BackendProbeResult probeVulkanBackend(
    const TargetTopologySnapshot &source_topology,
    const CompilerProviderRegistrySnapshot &providers,
    BackendProbeInput input) {
    requireNonEmpty(input.candidate, "backend candidate name");
    requireNonEmpty(input.endpoint, "backend candidate endpoint");
    canonicalizeVersionedNames(input.required_endpoint_capabilities,
                               "backend endpoint capability");
    canonicalizeVersionedNames(input.required_physical_features,
                               "backend physical feature");
    input.diagnostics =
        applyPlanningDiagnosticPolicy(input.diagnostics);
    const auto &provider =
        providers.require(input.provider_kind, input.provider).descriptor();
    const auto topology = canonicalizeTargetTopology(source_topology);

    BackendProbeResult result{
        .candidate = std::move(input.candidate),
        .provider = provider,
        .endpoint = std::move(input.endpoint),
        .required_physical_features =
            std::move(input.required_physical_features),
        .cost = input.cost,
        .diagnostics = std::move(input.diagnostics),
    };
    const auto reject = [&](std::string id, std::string detail) {
        result.failures.push_back(BackendConstraintFailure{
            std::move(id), result.candidate, std::move(detail)});
    };

    const auto *endpoint = findTargetEndpoint(topology, result.endpoint);
    if (endpoint == nullptr) {
        reject("pelican.plan.endpoint_missing@1",
               "target endpoint '" + result.endpoint + "' does not exist");
    } else {
        for (const auto &required : input.required_endpoint_capabilities) {
            if (!hasCapability(endpoint->capabilities, required)) {
                reject("pelican.plan.endpoint_capability_missing@1",
                       "endpoint '" + endpoint->id +
                           "' lacks capability '" + required + "'");
            }
        }
    }

    if (input.bridge) {
        auto bridge = std::move(*input.bridge);
        requireNonEmpty(bridge.source_endpoint,
                        "backend bridge source endpoint");
        requireNonEmpty(bridge.destination_endpoint,
                        "backend bridge destination endpoint");
        canonicalizeVersionedNames(bridge.required_link_capabilities,
                                   "backend bridge link capability");
        const auto source_exists =
            findTargetEndpoint(topology, bridge.source_endpoint) != nullptr;
        const auto destination_exists =
            findTargetEndpoint(topology, bridge.destination_endpoint) != nullptr;
        if (!source_exists) {
            reject("pelican.plan.bridge_endpoint_missing@1",
                   "bridge source endpoint '" + bridge.source_endpoint +
                       "' does not exist");
        }
        if (!destination_exists) {
            reject("pelican.plan.bridge_endpoint_missing@1",
                   "bridge destination endpoint '" +
                       bridge.destination_endpoint + "' does not exist");
        }
        if (source_exists && destination_exists) {
            std::vector<const TargetEndpointLink *> directed_links;
            for (const auto &link : topology.links) {
                if (link.source_endpoint == bridge.source_endpoint &&
                    link.destination_endpoint ==
                        bridge.destination_endpoint) {
                    directed_links.push_back(&link);
                }
            }
            const auto selected = std::find_if(
                directed_links.begin(), directed_links.end(),
                [&](const TargetEndpointLink *link) {
                    return std::all_of(
                        bridge.required_link_capabilities.begin(),
                        bridge.required_link_capabilities.end(),
                        [&](const auto &required) {
                            return hasCapability(link->capabilities, required);
                        });
                });
            if (selected != directed_links.end()) {
                result.selected_link = (*selected)->id;
                result.bridge_offers = (*selected)->bridge_offers;
            } else if (directed_links.empty()) {
                reject("pelican.plan.directed_link_missing@1",
                       "no directed link exists from '" +
                           bridge.source_endpoint + "' to '" +
                           bridge.destination_endpoint + "'");
            } else {
                const auto *best = directed_links.front();
                for (const auto &required :
                     bridge.required_link_capabilities) {
                    if (!hasCapability(best->capabilities, required)) {
                        reject(
                            "pelican.plan.link_capability_missing@1",
                            "directed link '" + best->id +
                                "' lacks capability '" + required + "'");
                    }
                }
            }
        }
    }

    std::sort(result.failures.begin(), result.failures.end(),
              [](const auto &left, const auto &right) {
                  return std::tie(left.id, left.subject, left.detail) <
                         std::tie(right.id, right.subject, right.detail);
              });
    result.feasible = result.failures.empty();
    return result;
}

BackendSelection selectBackendCandidate(
    std::vector<BackendProbeResult> candidates,
    const PlanningDiagnosticPolicy &diagnostic_policy) {
    if (candidates.empty()) {
        throw std::runtime_error(
            "backend candidate selection requires a finite non-empty set");
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto &left, const auto &right) {
                  return left.candidate < right.candidate;
              });
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        requireNonEmpty(candidates[index].candidate,
                        "backend probe result candidate");
        validateProviderDescriptor(candidates[index].provider);
        if (index != 0 &&
            candidates[index - 1].candidate == candidates[index].candidate) {
            throw std::runtime_error("duplicate backend candidate result: " +
                                     candidates[index].candidate);
        }
    }

    const BackendProbeResult *selected = nullptr;
    for (const auto &candidate : candidates) {
        if (!candidate.feasible) continue;
        if (selected == nullptr || costKey(candidate) < costKey(*selected)) {
            selected = &candidate;
        }
    }
    if (selected == nullptr) {
        std::ostringstream error;
        error << "no feasible backend candidate";
        for (const auto &candidate : candidates) {
            error << "; " << candidate.candidate << " ["
                  << compilerProviderFingerprint(candidate.provider) << ']';
            for (const auto &failure : candidate.failures) {
                error << ' ' << failure.id << ": " << failure.detail;
            }
        }
        throw std::runtime_error(error.str());
    }

    BackendSelection result;
    result.selected_candidate = selected->candidate;
    result.candidates = std::move(candidates);
    const auto selected_result = std::find_if(
        result.candidates.begin(), result.candidates.end(),
        [&](const auto &candidate) {
            return candidate.candidate == result.selected_candidate;
        });
    result.diagnostics = applyPlanningDiagnosticPolicy(
        selected_result->diagnostics, diagnostic_policy);
    requireNoPlanningErrors(result.diagnostics);

    for (const auto &candidate : result.candidates) {
        if (!candidate.feasible) {
            std::ostringstream detail;
            for (std::size_t index = 0; index < candidate.failures.size();
                 ++index) {
                if (index != 0) detail << ", ";
                detail << candidate.failures[index].id;
            }
            result.decisions.push_back(PlanningDecision{
                "pelican.plan.backend_candidate_rejected@1",
                candidate.candidate,
                compilerProviderFingerprint(candidate.provider),
                detail.str(),
            });
        } else if (candidate.candidate == result.selected_candidate) {
            result.decisions.push_back(PlanningDecision{
                "pelican.plan.backend_candidate_selected@1",
                candidate.candidate,
                compilerProviderFingerprint(candidate.provider),
                "lowest deterministic cost tuple",
            });
        } else {
            result.decisions.push_back(PlanningDecision{
                "pelican.plan.backend_candidate_not_selected@1",
                candidate.candidate,
                compilerProviderFingerprint(candidate.provider),
                "feasible candidate has a higher deterministic cost tuple",
            });
        }
    }
    return result;
}

nlohmann::ordered_json backendSelectionToJson(
    const BackendSelection &selection) {
    requireNonEmpty(selection.selected_candidate,
                    "selected backend candidate");
    nlohmann::ordered_json result{
        {"schema", "pelican.backend_selection"},
        {"version", 1},
        {"selected_candidate", selection.selected_candidate},
    };
    result["candidates"] = nlohmann::ordered_json::array();
    for (const auto &candidate : selection.candidates) {
        result["candidates"].push_back(probeResultToJson(candidate));
    }
    result["diagnostics"] = nlohmann::ordered_json::array();
    for (const auto &diagnostic : selection.diagnostics) {
        result["diagnostics"].push_back(diagnosticToJson(diagnostic));
    }
    result["decisions"] = nlohmann::ordered_json::array();
    for (const auto &decision : selection.decisions) {
        result["decisions"].push_back(nlohmann::ordered_json{
            {"id", decision.id},
            {"subject", decision.subject},
            {"selected", decision.selected},
            {"detail", decision.detail},
        });
    }
    return result;
}

std::string_view planningProfileKindName(PlanningProfileKind kind) {
    switch (kind) {
    case PlanningProfileKind::optimized: return "optimized";
    case PlanningProfileKind::conservative_debug:
        return "conservative_debug";
    case PlanningProfileKind::hazard_stress: return "hazard_stress";
    }
    throw std::runtime_error("unknown planning profile kind");
}

LogicalPlanningOpportunityReport analyzeLogicalPlanningOpportunities(
    const CompiledLogicalRenderGraph &graph,
    LogicalPlanningOpportunityInput input) {
    requireNonEmpty(graph.name, "logical planning graph name");
    if (input.profile.kind != PlanningProfileKind::hazard_stress &&
        input.profile.seed != 0) {
        throw std::runtime_error(
            "planning seed is only valid for hazard_stress profile");
    }

    std::map<std::string, std::size_t, std::less<>> node_indices;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        requireNonEmpty(graph.nodes[index].name, "logical planning node");
        if (!node_indices.emplace(graph.nodes[index].name, index).second) {
            throw std::runtime_error("duplicate logical planning node: " +
                                     graph.nodes[index].name);
        }
    }
    std::set<std::string, std::less<>> resource_names;
    for (const auto &resource : graph.resources) {
        if (!resource_names.insert(resource.name).second) {
            throw std::runtime_error("duplicate logical planning resource: " +
                                     resource.name);
        }
    }

    std::vector<PlanningNodeConstraint> node_constraints(
        graph.nodes.size());
    std::vector<bool> has_node_constraint(graph.nodes.size(), false);
    for (auto constraint : input.node_constraints) {
        const auto node = node_indices.find(constraint.node);
        if (node == node_indices.end()) {
            throw std::runtime_error(
                "planning constraint references unknown node: " +
                constraint.node);
        }
        if (has_node_constraint[node->second]) {
            throw std::runtime_error(
                "duplicate planning constraint for node: " +
                constraint.node);
        }
        has_node_constraint[node->second] = true;
        node_constraints[node->second] = std::move(constraint);
    }

    std::map<std::string, PlanningResourceConstraint, std::less<>>
        resource_constraints;
    for (auto constraint : input.resource_constraints) {
        if (!resource_names.contains(constraint.resource)) {
            throw std::runtime_error(
                "planning constraint references unknown resource: " +
                constraint.resource);
        }
        if (!resource_constraints
                 .emplace(constraint.resource, std::move(constraint))
                 .second) {
            throw std::runtime_error(
                "duplicate planning constraint for resource: " +
                constraint.resource);
        }
    }

    const auto count = graph.nodes.size();
    std::vector<std::set<std::size_t>> edges(count);
    const auto add_edge = [&](std::string_view source,
                              std::string_view destination) {
        const auto from = node_indices.find(source);
        const auto to = node_indices.find(destination);
        if (from == node_indices.end() || to == node_indices.end()) {
            throw std::runtime_error(
                "logical planning dependency references unknown node: " +
                std::string{source} + " -> " +
                std::string{destination});
        }
        if (from->second == to->second) {
            throw std::runtime_error(
                "logical planning dependency is self-referential: " +
                std::string{source});
        }
        edges[from->second].insert(to->second);
    };
    for (const auto &edge : deriveLogicalDataEdges(graph)) {
        add_edge(edge.producer_node, edge.consumer_node);
    }
    for (const auto &node : graph.nodes) {
        for (const auto &dependency : node.after) {
            add_edge(dependency, node.name);
        }
        for (const auto &dependent : node.before) {
            add_edge(node.name, dependent);
        }
    }

    std::vector<std::size_t> indegree(count, 0);
    for (const auto &outgoing : edges) {
        for (const auto destination : outgoing) ++indegree[destination];
    }
    std::vector<std::size_t> ready;
    for (std::size_t index = 0; index < count; ++index) {
        if (indegree[index] == 0) ready.push_back(index);
    }

    LogicalPlanningOpportunityReport report{
        .graph = graph.name,
        .profile = input.profile,
    };
    std::uint64_t step = 0;
    while (!ready.empty()) {
        const auto selected = std::min_element(
            ready.begin(), ready.end(), [&](std::size_t left,
                                            std::size_t right) {
                if (input.profile.kind ==
                    PlanningProfileKind::hazard_stress) {
                    const auto left_rank = stableRank(
                        input.profile.seed, step, graph.nodes[left].name);
                    const auto right_rank = stableRank(
                        input.profile.seed, step, graph.nodes[right].name);
                    if (left_rank != right_rank) return left_rank < right_rank;
                }
                return graph.nodes[left].name < graph.nodes[right].name;
            });
        const auto node_index = *selected;
        ready.erase(selected);
        report.node_order.push_back(graph.nodes[node_index].name);
        for (const auto destination : edges[node_index]) {
            if (--indegree[destination] == 0) ready.push_back(destination);
        }
        ++step;
    }
    if (report.node_order.size() != count) {
        throw std::runtime_error(
            "logical planning graph contains a dependency cycle");
    }

    std::vector<std::vector<bool>> reachable(
        count, std::vector<bool>(count, false));
    for (std::size_t source = 0; source < count; ++source) {
        for (const auto destination : edges[source]) {
            reachable[source][destination] = true;
        }
    }
    for (std::size_t intermediate = 0; intermediate < count;
         ++intermediate) {
        for (std::size_t source = 0; source < count; ++source) {
            if (!reachable[source][intermediate]) continue;
            for (std::size_t destination = 0; destination < count;
                 ++destination) {
                reachable[source][destination] =
                    reachable[source][destination] ||
                    reachable[intermediate][destination];
            }
        }
    }

    if (input.profile.kind !=
        PlanningProfileKind::conservative_debug) {
        for (std::size_t left = 0; left < count; ++left) {
            for (std::size_t right = left + 1; right < count; ++right) {
                if (reachable[left][right] || reachable[right][left]) {
                    continue;
                }
                const auto &left_constraint = node_constraints[left];
                const auto &right_constraint = node_constraints[right];
                if (left_constraint.serial || left_constraint.isolate ||
                    right_constraint.serial || right_constraint.isolate) {
                    continue;
                }
                auto pair = canonicalPair(
                    {graph.nodes[left].name, graph.nodes[right].name},
                    "logical planning node pair");
                report.parallel_candidates.push_back(pair);
                if (graph.nodes[left].kind == graph.nodes[right].kind) {
                    report.fusion_candidates.push_back(std::move(pair));
                }
            }
        }
    }

    std::set<std::string, std::less<>> isolated_resources;
    for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
        if (!node_constraints[index].isolate) continue;
        for (const auto &use : graph.nodes[index].uses) {
            if (use.input_value) {
                isolated_resources.insert(use.input_value->resource);
            }
            if (use.output_value) {
                isolated_resources.insert(use.output_value->resource);
            }
        }
    }
    std::vector<PlanningNamePair> legal_aliases;
    for (auto pair : input.legal_alias_candidates) {
        pair = canonicalPair(std::move(pair),
                             "legal alias candidate");
        if (!resource_names.contains(pair.first) ||
            !resource_names.contains(pair.second)) {
            throw std::runtime_error(
                "legal alias candidate references unknown resource: " +
                pair.first + ", " + pair.second);
        }
        const auto first_constraint =
            resource_constraints.find(pair.first);
        const auto second_constraint =
            resource_constraints.find(pair.second);
        if ((first_constraint != resource_constraints.end() &&
             first_constraint->second.no_alias) ||
            (second_constraint != resource_constraints.end() &&
             second_constraint->second.no_alias) ||
            isolated_resources.contains(pair.first) ||
            isolated_resources.contains(pair.second)) {
            continue;
        }
        legal_aliases.push_back(std::move(pair));
    }
    sortAndUnique(legal_aliases);

    const auto seeded_sort = [&](auto &values, std::uint64_t salt) {
        std::sort(values.begin(), values.end(),
                  [&](const auto &left, const auto &right) {
                      const auto left_rank = stableRank(
                          input.profile.seed, salt, pairKey(left));
                      const auto right_rank = stableRank(
                          input.profile.seed, salt, pairKey(right));
                      if (left_rank != right_rank) {
                          return left_rank < right_rank;
                      }
                      return left < right;
                  });
    };
    if (input.profile.kind == PlanningProfileKind::hazard_stress) {
        seeded_sort(report.parallel_candidates, 0x50415241ULL);
        seeded_sort(report.fusion_candidates, 0x46555345ULL);
        seeded_sort(legal_aliases, 0x414C4941ULL);
        if (!legal_aliases.empty()) {
            legal_aliases.resize((legal_aliases.size() + 1) / 2);
        }
        report.alias_candidates = std::move(legal_aliases);
    } else if (input.profile.kind == PlanningProfileKind::optimized) {
        sortAndUnique(report.parallel_candidates);
        sortAndUnique(report.fusion_candidates);
        report.alias_candidates = std::move(legal_aliases);
    }

    report.decisions.push_back(PlanningDecision{
        "pelican.plan.profile_selected@1",
        graph.name,
        std::string{planningProfileKindName(input.profile.kind)},
        input.profile.kind == PlanningProfileKind::hazard_stress
            ? "seed=" + std::to_string(input.profile.seed)
            : "seed=0",
    });
    report.decisions.push_back(PlanningDecision{
        "pelican.plan.parallel_candidates@1",
        graph.name,
        std::to_string(report.parallel_candidates.size()),
        "derived from logical data and explicit dependency edges",
    });
    report.decisions.push_back(PlanningDecision{
        "pelican.plan.fusion_candidates@1",
        graph.name,
        std::to_string(report.fusion_candidates.size()),
        "same-kind independent nodes without serial/isolate constraints",
    });
    report.decisions.push_back(PlanningDecision{
        "pelican.plan.alias_stress_candidates@1",
        graph.name,
        std::to_string(report.alias_candidates.size()),
        "policy applied to target-validated alias candidates",
    });
    return report;
}

nlohmann::ordered_json logicalPlanningOpportunityReportToJson(
    const LogicalPlanningOpportunityReport &report) {
    nlohmann::ordered_json result{
        {"schema", "pelican.logical_planning_opportunities"},
        {"version", 1},
        {"graph", report.graph},
        {"profile", planningProfileKindName(report.profile.kind)},
        {"seed", report.profile.seed},
        {"node_order", report.node_order},
    };
    const auto encode_pairs = [](const auto &pairs) {
        auto result = nlohmann::ordered_json::array();
        for (const auto &pair : pairs) result.push_back(pairToJson(pair));
        return result;
    };
    result["parallel_candidates"] =
        encode_pairs(report.parallel_candidates);
    result["fusion_candidates"] =
        encode_pairs(report.fusion_candidates);
    result["alias_candidates"] =
        encode_pairs(report.alias_candidates);
    result["decisions"] = nlohmann::ordered_json::array();
    for (const auto &decision : report.decisions) {
        result["decisions"].push_back(nlohmann::ordered_json{
            {"id", decision.id},
            {"subject", decision.subject},
            {"selected", decision.selected},
            {"detail", decision.detail},
        });
    }
    return result;
}

} // namespace Pelican
