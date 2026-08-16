#include "frameplanmodel.hpp"

#include "physicaltargetplanwire.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

std::runtime_error invalid(std::string message) {
    return std::runtime_error("frame plan response " + std::move(message));
}

const Json &requireObjectField(const Json &object, std::string_view field,
                               std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_object()) {
        throw invalid(std::string{context} + " requires object field '" +
                      std::string{field} + "'");
    }
    return *found;
}

const Json &requireArrayField(const Json &object, std::string_view field,
                              std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_array()) {
        throw invalid(std::string{context} + " requires array field '" +
                      std::string{field} + "'");
    }
    return *found;
}

std::string requireStringField(const Json &object, std::string_view field,
                               std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_string()) {
        throw invalid(std::string{context} + " requires string field '" +
                      std::string{field} + "'");
    }
    return found->get<std::string>();
}

std::string optionalStringField(const Json &object, std::string_view field,
                                std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || found->is_null()) {
        return {};
    }
    if (!found->is_string()) {
        throw invalid(std::string{context} + " field '" + std::string{field} +
                      "' must be a string");
    }
    return found->get<std::string>();
}

bool requireBoolField(const Json &object, std::string_view field,
                      std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_boolean()) {
        throw invalid(std::string{context} + " requires boolean field '" +
                      std::string{field} + "'");
    }
    return found->get<bool>();
}

std::optional<bool> optionalBoolField(const Json &object,
                                      std::string_view field,
                                      std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || found->is_null()) {
        return std::nullopt;
    }
    if (!found->is_boolean()) {
        throw invalid(std::string{context} + " field '" +
                      std::string{field} + "' must be a boolean");
    }
    return found->get<bool>();
}

double requireNumberField(const Json &object, std::string_view field,
                          std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || !found->is_number()) {
        throw invalid(std::string{context} + " requires number field '" +
                      std::string{field} + "'");
    }
    const auto value = found->get<double>();
    if (!std::isfinite(value)) {
        throw invalid(std::string{context} + "." + std::string{field} +
                      " must be finite");
    }
    return value;
}

std::uint64_t unsignedInteger(const Json &value, std::string_view context) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            return static_cast<std::uint64_t>(signed_value);
        }
    }
    throw invalid(std::string{context} + " must be a non-negative integer");
}

std::size_t requireSizeField(const Json &object, std::string_view field,
                             std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end()) {
        throw invalid(std::string{context} + " requires integer field '" +
                      std::string{field} + "'");
    }
    const auto value = unsignedInteger(*found, std::string{context} + "." +
                                                   std::string{field});
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw invalid(std::string{context} + "." + std::string{field} +
                      " is too large");
    }
    return static_cast<std::size_t>(value);
}

std::optional<std::size_t> optionalSizeField(const Json &object,
                                             std::string_view field,
                                             std::string_view context) {
    const auto found = object.find(field);
    if (found == object.end() || found->is_null()) {
        return std::nullopt;
    }
    const auto value = unsignedInteger(*found, std::string{context} + "." +
                                                   std::string{field});
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw invalid(std::string{context} + "." + std::string{field} +
                      " is too large");
    }
    return static_cast<std::size_t>(value);
}

std::vector<std::string> stringArray(const Json &object,
                                     std::string_view field,
                                     std::string_view context,
                                     bool required = false) {
    const auto found = object.find(field);
    if (found == object.end()) {
        if (required) {
            throw invalid(std::string{context} + " requires array field '" +
                          std::string{field} + "'");
        }
        return {};
    }
    if (!found->is_array()) {
        throw invalid(std::string{context} + " field '" + std::string{field} +
                      "' must be an array");
    }

    std::vector<std::string> result;
    result.reserve(found->size());
    for (const auto &entry : *found) {
        if (!entry.is_string()) {
            throw invalid(std::string{context} + "." + std::string{field} +
                          " entries must be strings");
        }
        result.push_back(entry.get<std::string>());
    }
    return result;
}

FramePlanExtent parseExtent(const Json &value, std::string_view context) {
    if (!value.is_object()) {
        throw invalid(std::string{context} + " must be an object");
    }
    return FramePlanExtent{
        .kind = requireStringField(value, "kind", context),
        .scale_x = requireNumberField(value, "scale_x", context),
        .scale_y = requireNumberField(value, "scale_y", context),
        .width = requireSizeField(value, "width", context),
        .height = requireSizeField(value, "height", context),
    };
}

FramePlanLifetime parseLifetime(const Json &value, std::string_view context) {
    if (!value.is_object()) {
        throw invalid(std::string{context} + " must be an object");
    }
    FramePlanLifetime result;
    result.used = requireBoolField(value, "used", context);
    result.first_use = optionalSizeField(value, "first_use", context);
    result.last_use = optionalSizeField(value, "last_use", context);
    if (result.used && (!result.first_use || !result.last_use)) {
        throw invalid(std::string{context} +
                      " used lifetime requires first_use and last_use");
    }
    if (!result.used && (result.first_use || result.last_use)) {
        throw invalid(std::string{context} +
                      " unused lifetime must not publish use indices");
    }
    if (result.first_use && result.last_use &&
        *result.first_use > *result.last_use) {
        throw invalid(std::string{context} +
                      " first_use must not exceed last_use");
    }
    return result;
}

bool collectionContainsPair(
    const std::vector<FramePlanAliasGroup> &groups, std::string_view first,
    std::string_view second) {
    return std::any_of(
        groups.begin(), groups.end(), [&](const FramePlanAliasGroup &group) {
            const auto contains = [&](std::string_view name) {
                return std::find(group.resources.begin(), group.resources.end(),
                                 name) != group.resources.end();
            };
            return contains(first) && contains(second);
        });
}

bool scopeContainsPair(const std::vector<FramePlanPhysicalScope> &scopes,
                       std::string_view first, std::string_view second) {
    return std::any_of(
        scopes.begin(), scopes.end(), [&](const FramePlanPhysicalScope &scope) {
            if (!scope.single_rendering_instance) {
                return false;
            }
            const auto contains = [&](std::string_view name) {
                return std::find(scope.nodes.begin(), scope.nodes.end(), name) !=
                       scope.nodes.end();
            };
            return contains(first) && contains(second);
        });
}

template <typename Adopted>
std::vector<FramePlanOpportunityPair> parseOpportunityPairs(
    const Json &report, std::string_view field, Adopted &&adopted) {
    const auto &entries =
        requireArrayField(report, field, "physical_target_plan.planning_opportunities");
    std::vector<FramePlanOpportunityPair> result;
    result.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto context =
            "physical_target_plan.planning_opportunities." +
            std::string{field} + "[" + std::to_string(index) + "]";
        if (!entries[index].is_object()) {
            throw invalid(context + " must be an object");
        }
        FramePlanOpportunityPair pair{
            .first = requireStringField(entries[index], "first", context),
            .second = requireStringField(entries[index], "second", context),
        };
        pair.adopted = adopted(pair.first, pair.second);
        result.push_back(std::move(pair));
    }
    return result;
}

std::string footprintName(const Json &use, std::string_view context) {
    const auto found = use.find("footprint");
    if (found == use.end() || found->is_null()) {
        return {};
    }
    if (found->is_string()) {
        return found->get<std::string>();
    }
    if (!found->is_object()) {
        throw invalid(std::string{context} + ".footprint must be an object");
    }
    return optionalStringField(*found, "kind", std::string{context} +
                                                   ".footprint");
}

std::string aggregateAttachmentOp(const FramePlanNode &node,
                                  std::string_view aspect,
                                  bool load) {
    std::string result;
    for (const auto &attachment : node.attachments) {
        if (attachment.aspect != aspect) {
            continue;
        }
        const std::string &candidate =
            load ? attachment.load_op : attachment.store_op;
        if (result.empty()) {
            result = candidate;
        } else if (result != candidate) {
            return "mixed";
        }
    }
    return result;
}

void appendUnique(std::vector<std::string> &values, const std::string &value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

FramePlanMaterialFilter parseMaterialFilter(const Json &value,
                                            std::string_view context) {
    if (!value.is_object()) {
        throw invalid(std::string{context} + " must be an object");
    }
    FramePlanMaterialFilter result;
    result.include = stringArray(value, "include", context);
    result.exclude = stringArray(value, "exclude", context);
    result.unmatched_include =
        stringArray(value, "unmatched_include", context);
    result.unmatched_exclude =
        stringArray(value, "unmatched_exclude", context);
    result.filter_id = optionalStringField(value, "filter_id", context);
    result.resolution_state =
        optionalStringField(value, "resolution_state", context);
    result.resolution_provenance =
        optionalStringField(value, "resolution_provenance", context);
    result.resolved_draw_count =
        optionalSizeField(value, "resolved_draw_count", context);
    return result;
}

FramePlanDecision parseDecision(const Json &value, std::string_view context) {
    if (!value.is_object()) {
        throw invalid(std::string{context} + " must be an object");
    }
    return FramePlanDecision{
        .id = requireStringField(value, "id", context),
        .subject = requireStringField(value, "subject", context),
        .selected = requireStringField(value, "selected", context),
        .detail = requireStringField(value, "detail", context),
    };
}

void appendDecisions(
    const Json &container, std::string_view context,
    std::vector<FramePlanDecisionGroup> &groups,
    std::unordered_map<std::string, std::size_t> &group_indices) {
    const auto decisions = container.find("decisions");
    if (decisions == container.end()) {
        return;
    }
    if (!decisions->is_array()) {
        throw invalid(std::string{context} + ".decisions must be an array");
    }
    for (std::size_t index = 0; index < decisions->size(); ++index) {
        FramePlanDecision decision = parseDecision(
            decisions->at(index), std::string{context} + ".decisions[" +
                                      std::to_string(index) + "]");
        auto found = group_indices.find(decision.subject);
        if (found == group_indices.end()) {
            const std::size_t group_index = groups.size();
            found = group_indices.emplace(decision.subject, group_index).first;
            groups.push_back(FramePlanDecisionGroup{
                .subject = decision.subject,
            });
        }
        groups[found->second].decisions.push_back(std::move(decision));
    }
}

FramePlanBackendFailure parseBackendFailure(const Json &value,
                                            std::string_view context) {
    if (!value.is_object()) {
        throw invalid(std::string{context} + " must be an object");
    }
    return FramePlanBackendFailure{
        .id = requireStringField(value, "id", context),
        .subject = requireStringField(value, "subject", context),
        .detail = requireStringField(value, "detail", context),
    };
}

FramePlanPlanningDiagnostic parsePlanningDiagnostic(
    const Json &value, std::string_view context) {
    if (!value.is_object()) {
        throw invalid(std::string{context} + " must be an object");
    }
    return FramePlanPlanningDiagnostic{
        .id = requireStringField(value, "id", context),
        .severity = requireStringField(value, "severity", context),
        .subject = requireStringField(value, "subject", context),
        .detail = requireStringField(value, "detail", context),
    };
}

template <typename Entry, typename Parser>
std::vector<Entry> parseOptionalArray(const Json &object,
                                      std::string_view field,
                                      std::string_view context,
                                      Parser &&parser) {
    const auto found = object.find(field);
    if (found == object.end()) {
        return {};
    }
    if (!found->is_array()) {
        throw invalid(std::string{context} + "." + std::string{field} +
                      " must be an array");
    }
    std::vector<Entry> result;
    result.reserve(found->size());
    for (std::size_t index = 0; index < found->size(); ++index) {
        result.push_back(parser(
            found->at(index), std::string{context} + "." +
                                  std::string{field} + "[" +
                                  std::to_string(index) + "]"));
    }
    return result;
}

void parseBackendSelection(
    const Json &selection, FramePlanModel &model,
    std::unordered_map<std::string, std::size_t> &decision_group_indices) {
    if (!selection.is_object()) {
        throw invalid("physical_target_plan.backend_selection must be an object");
    }
    constexpr std::string_view context =
        "physical_target_plan.backend_selection";
    model.selected_backend_candidate =
        optionalStringField(selection, "selected_candidate", context);
    model.backend_diagnostics =
        parseOptionalArray<FramePlanPlanningDiagnostic>(
            selection, "diagnostics", context, parsePlanningDiagnostic);
    appendDecisions(selection, context, model.decision_groups,
                    decision_group_indices);

    const auto candidates = selection.find("candidates");
    if (candidates == selection.end()) {
        return;
    }
    if (!candidates->is_array()) {
        throw invalid("physical_target_plan.backend_selection.candidates must be an array");
    }
    model.backend_candidates.reserve(candidates->size());
    for (std::size_t index = 0; index < candidates->size(); ++index) {
        const auto &value = candidates->at(index);
        const std::string candidate_context =
            std::string{context} + ".candidates[" +
            std::to_string(index) + "]";
        if (!value.is_object()) {
            throw invalid(candidate_context + " must be an object");
        }
        FramePlanBackendCandidate candidate;
        candidate.candidate =
            requireStringField(value, "candidate", candidate_context);
        candidate.endpoint =
            optionalStringField(value, "endpoint", candidate_context);
        candidate.feasible =
            requireBoolField(value, "feasible", candidate_context);
        candidate.selected =
            candidate.candidate == model.selected_backend_candidate;
        candidate.failures = parseOptionalArray<FramePlanBackendFailure>(
            value, "failures", candidate_context, parseBackendFailure);
        candidate.diagnostics =
            parseOptionalArray<FramePlanPlanningDiagnostic>(
                value, "diagnostics", candidate_context,
                parsePlanningDiagnostic);
        model.backend_candidates.push_back(std::move(candidate));
    }
}

} // namespace

FramePlanModel buildFramePlanModel(std::string_view response_json) {
    Json root;
    try {
        root = Json::parse(response_json);
    } catch (const Json::parse_error &error) {
        throw invalid("is not valid JSON: " + std::string{error.what()});
    }
    if (!root.is_object()) {
        throw invalid("must be a JSON object");
    }
    if (optionalStringField(root, "schema", "root") !=
        "pelican.frame_plan") {
        throw invalid("requires schema 'pelican.frame_plan'");
    }
    const auto version = root.find("version");
    if (version == root.end() || unsignedInteger(*version, "root.version") != 1) {
        throw invalid("requires pelican.frame_plan version 1");
    }

    FramePlanModel model;
    model.response_bytes = response_json.size();
    model.raw_json = root.dump(2);
    model.graph = requireStringField(root, "graph", "root");
    if (model.graph.empty()) {
        throw invalid("graph must not be empty");
    }
    if (const auto generation = root.find("runtime_generation");
        generation != root.end() && !generation->is_null()) {
        model.runtime_generation =
            unsignedInteger(*generation, "root.runtime_generation");
    }

    const auto &nodes = requireArrayField(root, "nodes", "root");
    const auto &barriers = requireArrayField(root, "barriers", "root");
    std::unordered_set<std::string> names;
    std::unordered_set<std::size_t> orders;
    model.nodes.reserve(nodes.size());
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto &value = nodes.at(index);
        const std::string context = "node[" + std::to_string(index) + "]";
        if (!value.is_object()) {
            throw invalid(context + " must be an object");
        }
        FramePlanNode node;
        node.name = requireStringField(value, "name", context);
        node.kind = requireStringField(value, "kind", context);
        node.source = optionalStringField(value, "source", context);
        node.provider_feature =
            optionalStringField(value, "provider_feature", context);
        node.provider_reference =
            optionalStringField(value, "provider_ref", context);
        node.declaration_index =
            requireSizeField(value, "declaration_index", context);
        node.order = requireSizeField(value, "order", context);
        node.level = requireSizeField(value, "level", context);
        node.reads = stringArray(value, "reads", context, true);
        node.history_reads = stringArray(value, "reads_history", context);
        node.writes = stringArray(value, "writes", context, true);
        node.view_family = optionalStringField(value, "view_family", context);
        node.snapshot_after =
            optionalStringField(value, "snapshot_after", context);
        node.byte_size = optionalSizeField(value, "byte_size", context).value_or(0);
        node.material_variant =
            optionalStringField(value, "material_variant", context);
        if (const auto filter = value.find("material_filter");
            filter != value.end()) {
            node.material_filter =
                parseMaterialFilter(*filter, context + ".material_filter");
        }
        node.color_load_op =
            optionalStringField(value, "color_load_op", context);
        node.color_store_op =
            optionalStringField(value, "color_store_op", context);
        node.depth_load_op =
            optionalStringField(value, "depth_load_op", context);
        node.depth_store_op =
            optionalStringField(value, "depth_store_op", context);

        if (node.name.empty() || !names.insert(node.name).second) {
            throw invalid("contains an empty or duplicate node name: '" +
                          node.name + "'");
        }
        if (!orders.insert(node.order).second) {
            throw invalid("contains duplicate node order " +
                          std::to_string(node.order));
        }
        model.nodes.push_back(std::move(node));
    }
    std::stable_sort(model.nodes.begin(), model.nodes.end(),
                     [](const FramePlanNode &left, const FramePlanNode &right) {
                         return left.order < right.order;
                     });

    std::unordered_map<std::string, std::size_t> node_indices;
    node_indices.reserve(model.nodes.size());
    for (std::size_t index = 0; index < model.nodes.size(); ++index) {
        node_indices.emplace(model.nodes[index].name, index);
    }

    model.barriers.reserve(barriers.size());
    for (std::size_t index = 0; index < barriers.size(); ++index) {
        const auto &value = barriers.at(index);
        const std::string context = "barrier[" + std::to_string(index) + "]";
        if (!value.is_object()) {
            throw invalid(context + " must be an object");
        }
        FramePlanBarrier barrier{
            .kind = requireStringField(value, "kind", context),
            .resource = requireStringField(value, "resource", context),
            .from = requireStringField(value, "from", context),
            .to = requireStringField(value, "to", context),
        };
        const auto from = node_indices.find(barrier.from);
        const auto to = node_indices.find(barrier.to);
        if (from == node_indices.end() || to == node_indices.end()) {
            throw invalid(context + " references an unknown node");
        }
        model.nodes[from->second].outgoing_barriers.push_back(index);
        model.nodes[to->second].incoming_barriers.push_back(index);
        model.barriers.push_back(std::move(barrier));
    }

    std::unordered_set<std::string> logical_resource_names;
    for (const auto &node : model.nodes) {
        logical_resource_names.insert(node.reads.begin(), node.reads.end());
        logical_resource_names.insert(node.history_reads.begin(),
                                      node.history_reads.end());
        logical_resource_names.insert(node.writes.begin(), node.writes.end());
    }
    for (const auto &barrier : model.barriers) {
        logical_resource_names.insert(barrier.resource);
    }

    std::vector<std::string> execution_node_names;

    if (const auto execution = root.find("execution_plan");
        execution != root.end() && !execution->is_null()) {
        if (!execution->is_object()) {
            throw invalid("execution_plan must be an object");
        }
        if (optionalStringField(*execution, "schema", "execution_plan") !=
            "pelican.frame_execution_plan") {
            throw invalid("execution_plan requires schema 'pelican.frame_execution_plan'");
        }
        const auto schema_version = execution->find("schema_version");
        if (schema_version == execution->end() ||
            unsignedInteger(*schema_version, "execution_plan.schema_version") != 1) {
            throw invalid("execution_plan requires schema version 1");
        }
        if (requireStringField(*execution, "graph", "execution_plan") !=
            model.graph) {
            throw invalid(
                "execution_plan_graph_mismatch: execution_plan graph does not "
                "match the frame plan graph");
        }
        if (requireStringField(*execution, "fingerprint", "execution_plan")
                .empty()) {
            throw invalid(
                "execution_plan_fingerprint_invalid: fingerprint must not be "
                "empty");
        }
        requireArrayField(*execution, "bridges", "execution_plan");
        requireArrayField(*execution, "endpoints", "execution_plan");
        const auto &execution_nodes =
            requireArrayField(*execution, "nodes", "execution_plan");
        std::unordered_set<std::string> seen_execution_nodes;
        execution_node_names.reserve(execution_nodes.size());
        for (std::size_t index = 0; index < execution_nodes.size(); ++index) {
            const auto &value = execution_nodes.at(index);
            const std::string context =
                "execution_plan.nodes[" + std::to_string(index) + "]";
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            const std::string name = requireStringField(value, "name", context);
            if (!seen_execution_nodes.insert(name).second) {
                throw invalid("execution_plan_duplicate: duplicate execution "
                              "node '" +
                              name + "'");
            }
            const auto destination = node_indices.find(name);
            if (destination == node_indices.end()) {
                throw invalid(
                    "execution_plan_missing_reference: execution node '" +
                    name + "' is absent from the frame plan nodes");
            }
            execution_node_names.push_back(name);
            auto &node = model.nodes[destination->second];
            node.semantic_dialect =
                optionalStringField(value, "semantic_dialect", context);
            node.selected_implementation =
                optionalStringField(value, "selected_implementation", context);
            node.selected_endpoint =
                optionalStringField(value, "selected_endpoint", context);
            node.required_capabilities =
                stringArray(value, "required_capabilities", context);
            if (const auto uses = value.find("resource_uses");
                uses != value.end()) {
                if (!uses->is_array()) {
                    throw invalid(context + ".resource_uses must be an array");
                }
                node.resource_uses.reserve(uses->size());
                for (std::size_t use_index = 0; use_index < uses->size();
                     ++use_index) {
                    const auto &use = uses->at(use_index);
                    const std::string use_context =
                        context + ".resource_uses[" +
                        std::to_string(use_index) + "]";
                    if (!use.is_object()) {
                        throw invalid(use_context + " must be an object");
                    }
                    node.resource_uses.push_back(FramePlanResourceUse{
                        .resource = requireStringField(use, "resource", use_context),
                        .epoch = optionalStringField(use, "epoch", use_context),
                        .access = optionalStringField(use, "access", use_context),
                        .intent = optionalStringField(use, "intent", use_context),
                        .footprint = footprintName(use, use_context),
                    });
                }
            }
        }
        if (seen_execution_nodes.size() != model.nodes.size()) {
            const auto missing = std::find_if(
                model.nodes.begin(), model.nodes.end(),
                [&](const FramePlanNode &node) {
                    return !seen_execution_nodes.contains(node.name);
                });
            throw invalid(
                "execution_plan_missing_reference: frame node '" +
                (missing == model.nodes.end() ? std::string{"?"}
                                              : missing->name) +
                "' is absent from execution_plan.nodes");
        }

        const auto &dependencies =
            requireArrayField(*execution, "dependencies", "execution_plan");
        std::set<std::tuple<std::string, std::string, std::string, std::string>>
            dependency_identities;
        model.dependencies.reserve(dependencies.size());
        for (std::size_t index = 0; index < dependencies.size(); ++index) {
            const auto context =
                "execution_plan.dependencies[" + std::to_string(index) + "]";
            const auto &value = dependencies[index];
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            FramePlanDependency dependency{
                .from = requireStringField(value, "from", context),
                .to = requireStringField(value, "to", context),
                .reason = requireStringField(value, "reason", context),
                .resource = optionalStringField(value, "resource", context),
            };
            if (!seen_execution_nodes.contains(dependency.from) ||
                !seen_execution_nodes.contains(dependency.to)) {
                throw invalid(
                    "execution_plan_missing_reference: dependency references "
                    "an unknown execution node");
            }
            if (!dependency.resource.empty() &&
                !logical_resource_names.contains(dependency.resource)) {
                throw invalid(
                    "execution_plan_missing_reference: dependency references "
                    "unknown resource '" +
                    dependency.resource + "'");
            }
            if (!dependency_identities
                     .emplace(dependency.from, dependency.to,
                              dependency.reason, dependency.resource)
                     .second) {
                throw invalid(
                    "execution_plan_duplicate: duplicate dependency");
            }
            model.dependencies.push_back(std::move(dependency));
        }
    } else {
        execution_node_names.reserve(model.nodes.size());
        for (const auto &node : model.nodes) {
            execution_node_names.push_back(node.name);
        }
    }

    std::unordered_map<std::string, std::size_t> decision_group_indices;
    const Json *physical_plan_json = nullptr;
    if (const auto physical = root.find("physical_target_plan");
        physical != root.end() && !physical->is_null()) {
        physical_plan_json = &*physical;
    }
    std::vector<std::string> physical_resource_context{
        logical_resource_names.begin(), logical_resource_names.end()};
    const auto physical_validation = Pelican::validatePhysicalTargetPlanWire(
        physical_plan_json,
        Pelican::PhysicalTargetPlanWireContext{
            .expected_graph = model.graph,
            .execution_nodes = execution_node_names,
            .logical_resources = std::move(physical_resource_context),
        });
    if (!physical_validation.available()) {
        model.physical_plan.state = FramePlanPhysicalPlanState::unavailable;
        model.physical_plan.unavailable_reason_code =
            physical_validation.reason_code;
        model.physical_plan.unavailable_reason = physical_validation.reason();
    } else {
        const Json &physical = *physical_plan_json;
        model.physical_plan.state = FramePlanPhysicalPlanState::available;
        model.physical_plan.unavailable_reason_code.clear();
        model.physical_plan.unavailable_reason.clear();
        model.physical_plan.schema =
            requireStringField(physical, "schema", "physical_target_plan");
        model.physical_plan.version =
            requireSizeField(physical, "version", "physical_target_plan");
        model.physical_plan.graph =
            requireStringField(physical, "graph", "physical_target_plan");
        model.physical_plan.logical_graph_fingerprint = requireStringField(
            physical, "logical_graph_fingerprint", "physical_target_plan");
        model.physical_plan.automatic_plan_fingerprint = requireStringField(
            physical, "automatic_plan_fingerprint", "physical_target_plan");
        model.physical_plan.wire_sections.reserve(physical.size());
        for (const auto &[name, value] : physical.items()) {
            model.physical_plan.wire_sections.push_back(
                FramePlanWireSection{.name = name, .json = value.dump()});
        }

        const auto &alias_groups =
            requireArrayField(physical, "alias_groups", "physical_target_plan");
        model.physical_plan.alias_groups.reserve(alias_groups.size());
        for (std::size_t index = 0; index < alias_groups.size(); ++index) {
            const auto context = "physical_target_plan.alias_groups[" +
                                 std::to_string(index) + "]";
            model.physical_plan.alias_groups.push_back(FramePlanAliasGroup{
                .id = requireStringField(alias_groups[index], "id", context),
                .resources = stringArray(alias_groups[index], "resources",
                                         context, true),
            });
        }

        const auto &scopes =
            requireArrayField(physical, "scopes", "physical_target_plan");
        model.physical_plan.scopes.reserve(scopes.size());
        for (std::size_t index = 0; index < scopes.size(); ++index) {
            const auto context = "physical_target_plan.scopes[" +
                                 std::to_string(index) + "]";
            const auto &value = scopes[index];
            FramePlanPhysicalScope scope{
                .id = requireStringField(value, "id", context),
                .kind = requireStringField(value, "kind", context),
                .nodes = stringArray(value, "nodes", context, true),
                .single_rendering_instance = requireBoolField(
                    value, "single_rendering_instance", context),
                .local_reads =
                    stringArray(value, "local_reads", context, true),
                .regions = stringArray(value, "regions", context, true),
                .view_execution =
                    requireStringField(value, "view_execution", context),
                .view_count = requireSizeField(value, "view_count", context),
                .execution_count =
                    requireSizeField(value, "execution_count", context),
                .view_mask = requireSizeField(value, "view_mask", context),
                .rasterization_samples = optionalSizeField(
                    value, "rasterization_samples", context),
            };
            model.physical_plan.scopes.push_back(std::move(scope));
        }

        const auto &lowering = requireObjectField(
            physical, "lowering_graph", "physical_target_plan");
        const auto &lowering_nodes =
            requireArrayField(lowering, "nodes", "lowering_graph");
        model.physical_plan.lowering_nodes.reserve(lowering_nodes.size());
        for (std::size_t index = 0; index < lowering_nodes.size(); ++index) {
            const auto context =
                "physical_target_plan.lowering_graph.nodes[" +
                std::to_string(index) + "]";
            const auto &value = lowering_nodes[index];
            model.physical_plan.lowering_nodes.push_back(
                FramePlanLoweringNode{
                    .name = requireStringField(value, "name", context),
                    .kind = requireStringField(value, "kind", context),
                    .dialect = requireStringField(value, "dialect", context),
                    .sources = stringArray(value, "sources", context, true),
                    .regions = stringArray(value, "regions", context, true),
                    .required_physical_features = stringArray(
                        value, "required_physical_features", context, true),
                });
        }

        if (const auto resolution = physical.find("resolution_plan");
            resolution != physical.end() && !resolution->is_null()) {
            if (!resolution->is_object()) {
                throw invalid(
                    "physical_target_plan.resolution_plan must be an object");
            }
            model.physical_plan.resolution_plan = FramePlanResolutionPlan{
                .render_source_resource = requireStringField(
                    *resolution, "render_source_resource", "resolution_plan"),
                .render_extent = parseExtent(
                    requireObjectField(*resolution, "render_extent",
                                       "resolution_plan"),
                    "resolution_plan.render_extent"),
                .output_source_resource = requireStringField(
                    *resolution, "output_source_resource", "resolution_plan"),
                .output_extent = parseExtent(
                    requireObjectField(*resolution, "output_extent",
                                       "resolution_plan"),
                    "resolution_plan.output_extent"),
                .scene_resources = stringArray(
                    *resolution, "scene_resources", "resolution_plan", true),
            };
        }

        const auto &opportunities = requireObjectField(
            physical, "planning_opportunities", "physical_target_plan");
        model.physical_plan.planning_profile = requireStringField(
            opportunities, "profile", "physical_target_plan.planning_opportunities");
        model.physical_plan.alias_candidates = parseOpportunityPairs(
            opportunities, "alias_candidates",
            [&](std::string_view first, std::string_view second) {
                return collectionContainsPair(model.physical_plan.alias_groups,
                                              first, second);
            });
        model.physical_plan.fusion_candidates = parseOpportunityPairs(
            opportunities, "fusion_candidates",
            [&](std::string_view first, std::string_view second) {
                return scopeContainsPair(model.physical_plan.scopes, first,
                                         second);
            });
        model.physical_plan.parallel_candidates = parseOpportunityPairs(
            opportunities, "parallel_candidates",
            [](std::string_view, std::string_view) { return false; });

        appendDecisions(physical, "physical_target_plan",
                        model.decision_groups, decision_group_indices);
        appendDecisions(opportunities,
                            "physical_target_plan.planning_opportunities",
                            model.decision_groups, decision_group_indices);
        const auto &backend = requireObjectField(
            physical, "backend_selection", "physical_target_plan");
        parseBackendSelection(backend, model, decision_group_indices);
        const auto selected_backend = std::find_if(
            model.backend_candidates.begin(), model.backend_candidates.end(),
            [](const FramePlanBackendCandidate &candidate) {
                return candidate.selected;
            });
        if (selected_backend == model.backend_candidates.end()) {
            throw invalid(
                "physical_plan_missing_reference: selected backend candidate "
                "is absent from backend_selection.candidates");
        }
        model.physical_plan.planning_endpoint = selected_backend->endpoint;
        appendDecisions(lowering,
                            "physical_target_plan.lowering_graph",
                            model.decision_groups, decision_group_indices);
        if (const auto attachments = physical.find("attachments");
            attachments != physical.end()) {
            if (!attachments->is_array()) {
                throw invalid("physical_target_plan.attachments must be an array");
            }
            for (std::size_t index = 0; index < attachments->size(); ++index) {
                const auto &value = attachments->at(index);
                const std::string context =
                    "physical_target_plan.attachments[" +
                    std::to_string(index) + "]";
                if (!value.is_object()) {
                    throw invalid(context + " must be an object");
                }
                const std::string node_name =
                    requireStringField(value, "node", context);
                const auto destination = node_indices.find(node_name);
                if (destination == node_indices.end()) {
                    throw invalid(
                        "physical_plan_missing_reference: attachment node '" +
                        node_name + "' is absent from frame plan nodes");
                }
                model.nodes[destination->second].attachments.push_back(
                    FramePlanAttachmentOps{
                        .resource = requireStringField(
                            value, "logical_resource", context),
                        .aspect = requireStringField(value, "aspect", context),
                        .load_op = requireStringField(value, "load_op", context),
                        .store_op = requireStringField(value, "store_op", context),
                    });
            }
        }
    }
    for (auto &node : model.nodes) {
        if (node.color_load_op.empty()) {
            node.color_load_op = aggregateAttachmentOp(node, "color", true);
        }
        if (node.color_store_op.empty()) {
            node.color_store_op = aggregateAttachmentOp(node, "color", false);
        }
        if (node.depth_load_op.empty()) {
            node.depth_load_op = aggregateAttachmentOp(node, "depth", true);
        }
        if (node.depth_store_op.empty()) {
            node.depth_store_op = aggregateAttachmentOp(node, "depth", false);
        }
    }

    std::map<std::string, FramePlanResource, std::less<>> resources;
    for (const auto &node : model.nodes) {
        for (const auto &name : node.reads) {
            auto &resource = resources[name];
            resource.name = name;
            appendUnique(resource.readers, node.name);
        }
        for (const auto &name : node.history_reads) {
            auto &resource = resources[name];
            resource.name = name;
            appendUnique(resource.history_readers, node.name);
        }
        for (const auto &name : node.writes) {
            auto &resource = resources[name];
            resource.name = name;
            appendUnique(resource.writers, node.name);
        }
        for (const auto &attachment : node.attachments) {
            auto &resource = resources[attachment.resource];
            resource.name = attachment.resource;
        }
    }
    for (const auto &barrier : model.barriers) {
        auto &resource = resources[barrier.resource];
        resource.name = barrier.resource;
    }

    if (const auto declarations = root.find("resources");
        declarations != root.end()) {
        if (!declarations->is_array()) {
            throw invalid("resources must be an array");
        }
        std::unordered_set<std::string> declaration_names;
        for (std::size_t index = 0; index < declarations->size(); ++index) {
            const auto &value = declarations->at(index);
            const std::string context =
                "resources[" + std::to_string(index) + "]";
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            const std::string name =
                requireStringField(value, "name", context);
            if (!declaration_names.insert(name).second) {
                throw invalid("duplicate_resource: duplicate resource "
                              "declaration '" +
                              name + "'");
            }
            auto &resource = resources[name];
            resource.name = name;
            resource.kind = optionalStringField(value, "kind", context);
            resource.source = optionalStringField(value, "source", context);
            resource.provider_feature =
                optionalStringField(value, "provider_feature", context);
            resource.provider_reference =
                optionalStringField(value, "provider_ref", context);
            if (resource.kind == "buffer") {
                resource.format = "buffer";
            }
        }
    }

    if (model.physical_plan.available()) {
        const Json &physical = *physical_plan_json;
        const auto &physical_resources =
            requireArrayField(physical, "resources", "physical_target_plan");
        for (std::size_t index = 0; index < physical_resources.size(); ++index) {
            const auto &value = physical_resources[index];
            const std::string context = "physical_target_plan.resources[" +
                                        std::to_string(index) + "]";
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            const std::string name =
                requireStringField(value, "logical_resource", context);
            auto &resource = resources[name];
            resource.name = name;
            resource.format = requireStringField(value, "format", context);
            resource.pattern =
                requireStringField(value, "pattern", context);
            resource.representation =
                requireStringField(value, "representation", context);
            resource.widest_read =
                requireStringField(value, "widest_read", context);
            resource.lifetime = parseLifetime(
                requireObjectField(value, "lifetime", context),
                context + ".lifetime");
            resource.stored = requireBoolField(value, "stored", context);
            resource.aliasable =
                requireBoolField(value, "aliasable", context);
            resource.required_physical_features = stringArray(
                value, "required_physical_features", context, true);
            resource.reason = optionalStringField(value, "reason", context);
            resource.view_layout =
                requireStringField(value, "view_layout", context);
            resource.array_layers =
                requireSizeField(value, "array_layers", context);
            resource.dimension =
                requireStringField(value, "dimension", context);
            const auto &mip_levels =
                requireObjectField(value, "mip_levels", context);
            resource.mip_level_mode = requireStringField(
                mip_levels, "mode", context + ".mip_levels");
            resource.mip_level_count = requireSizeField(
                mip_levels, "count", context + ".mip_levels");
            resource.rasterization_samples = optionalSizeField(
                value, "rasterization_samples", context);
            resource.resolve_required =
                optionalBoolField(value, "resolve_required", context);
            if (const auto extent = value.find("extent");
                extent != value.end() && !extent->is_null()) {
                resource.extent = parseExtent(*extent, context + ".extent");
            }
        }

        // get_frame_plan does not publish the live swapchain extent as a
        // separate field.  The fixed `display` target is therefore Studio's
        // canonical output extent.  This mirrors the producer's truncating
        // float-to-uint resolution rule for output_relative extents.
        std::optional<std::pair<std::size_t, std::size_t>> output_extent;
        if (const auto display = resources.find("display");
            display != resources.end() && display->second.extent &&
            display->second.extent->kind == "fixed") {
            output_extent = std::pair{display->second.extent->width,
                                      display->second.extent->height};
        } else if (model.physical_plan.resolution_plan &&
                   model.physical_plan.resolution_plan->output_extent.kind ==
                       "fixed") {
            const auto &extent =
                model.physical_plan.resolution_plan->output_extent;
            output_extent = std::pair{extent.width, extent.height};
        }

        bool requires_output_extent = false;
        for (auto &[name, resource] : resources) {
            (void)name;
            if (!resource.extent) {
                continue;
            }
            if (resource.extent->kind == "fixed") {
                resource.width = resource.extent->width;
                resource.height = resource.extent->height;
                continue;
            }
            requires_output_extent = true;
            if (!output_extent) {
                continue;
            }
            resource.width = static_cast<std::size_t>(
                static_cast<double>(output_extent->first) *
                resource.extent->scale_x);
            resource.height = static_cast<std::size_t>(
                static_cast<double>(output_extent->second) *
                resource.extent->scale_y);
        }
        if (requires_output_extent && !output_extent) {
            model.physical_plan.state =
                FramePlanPhysicalPlanState::unavailable;
            model.physical_plan.unavailable_reason_code =
                "physical_plan_output_extent_unavailable";
            model.physical_plan.unavailable_reason =
                "physical_plan_output_extent_unavailable: no fixed display "
                "extent was published";
        } else if (output_extent) {
            model.physical_plan.output_width = output_extent->first;
            model.physical_plan.output_height = output_extent->second;
        }

        for (const auto &group : model.physical_plan.alias_groups) {
            for (const auto &name : group.resources) {
                const auto resource = resources.find(name);
                if (resource == resources.end()) {
                    throw invalid(
                        "physical_plan_missing_reference: alias group "
                        "references unknown resource '" +
                        name + "'");
                }
                resource->second.alias_group = group.id;
            }
        }
    }

    if (const auto arena = root.find("gpu_resource_arena");
        arena != root.end() && !arena->is_null()) {
        if (!arena->is_object()) {
            throw invalid("gpu_resource_arena must be an object");
        }
        const auto generation = arena->find("runtime_generation");
        if (generation == arena->end()) {
            throw invalid(
                "gpu_resource_arena requires integer field "
                "'runtime_generation'");
        }
        FramePlanGpuResourceArena parsed_arena{
            .runtime_generation = unsignedInteger(
                *generation, "gpu_resource_arena.runtime_generation"),
            .resource_count = requireSizeField(
                *arena, "resource_count", "gpu_resource_arena"),
        };
        if (model.runtime_generation &&
            parsed_arena.runtime_generation != *model.runtime_generation) {
            throw invalid(
                "gpu_resource_arena_generation_mismatch: arena generation "
                "does not match frame plan generation");
        }
        const auto &arena_scopes =
            requireArrayField(*arena, "scopes", "gpu_resource_arena");
        std::unordered_set<std::string> owner_scopes;
        std::size_t counted_resources = 0;
        parsed_arena.scopes.reserve(arena_scopes.size());
        for (std::size_t scope_index = 0; scope_index < arena_scopes.size();
             ++scope_index) {
            const auto context = "gpu_resource_arena.scopes[" +
                                 std::to_string(scope_index) + "]";
            const auto &value = arena_scopes[scope_index];
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            FramePlanGpuResourceScope scope{
                .owner_scope =
                    requireStringField(value, "owner_scope", context),
                .resource_lease_count = requireSizeField(
                    value, "resource_lease_count", context),
            };
            if (!owner_scopes.insert(scope.owner_scope).second) {
                throw invalid(
                    "gpu_resource_arena_duplicate: duplicate owner scope '" +
                    scope.owner_scope + "'");
            }
            const auto &arena_resources =
                requireArrayField(value, "resources", context);
            counted_resources += arena_resources.size();
            scope.resources.reserve(arena_resources.size());
            std::set<std::pair<std::string, std::uint64_t>> identities;
            for (std::size_t resource_index = 0;
                 resource_index < arena_resources.size(); ++resource_index) {
                const auto resource_context =
                    context + ".resources[" +
                    std::to_string(resource_index) + "]";
                const auto &resource = arena_resources[resource_index];
                if (!resource.is_object()) {
                    throw invalid(resource_context + " must be an object");
                }
                const auto handle = resource.find("handle");
                if (handle == resource.end()) {
                    throw invalid(resource_context +
                                  " requires integer field 'handle'");
                }
                FramePlanGpuResource parsed_resource{
                    .kind = requireStringField(resource, "kind",
                                               resource_context),
                    .handle = unsignedInteger(*handle,
                                              resource_context + ".handle"),
                    .name = requireStringField(resource, "name",
                                               resource_context),
                    .declared_bytes = requireSizeField(
                        resource, "declared_bytes", resource_context),
                };
                if (!identities
                         .emplace(parsed_resource.kind,
                                  parsed_resource.handle)
                         .second) {
                    throw invalid(
                        "gpu_resource_arena_duplicate: duplicate kind/handle "
                        "identity in owner scope '" +
                        scope.owner_scope + "'");
                }
                scope.resources.push_back(std::move(parsed_resource));
            }
            parsed_arena.scopes.push_back(std::move(scope));
        }
        if (counted_resources != parsed_arena.resource_count) {
            throw invalid(
                "gpu_resource_arena_count_mismatch: resource_count does not "
                "match the published scope resources");
        }
        model.gpu_resource_arena = std::move(parsed_arena);
    }

    if (const auto contracts = root.find("surface_resource_contracts");
        contracts != root.end()) {
        if (!contracts->is_array()) {
            throw invalid("surface_resource_contracts must be an array");
        }
        for (std::size_t index = 0; index < contracts->size(); ++index) {
            const auto &value = contracts->at(index);
            const std::string context =
                "surface_resource_contracts[" + std::to_string(index) + "]";
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            const std::string name =
                requireStringField(value, "resource", context);
            auto &resource = resources[name];
            resource.name = name;
            resource.provider_feature =
                optionalStringField(value, "provider_feature", context);
            resource.provider_reference =
                optionalStringField(value, "provider_ref", context);
            resource.sampling = optionalStringField(value, "sampling", context);
            resource.view_policy =
                optionalStringField(value, "view_policy", context);
            resource.fallback = optionalStringField(value, "fallback", context);
            resource.material_consumers =
                stringArray(value, "material_consumers", context);
            resource.fullscreen_consumers =
                stringArray(value, "fullscreen_consumers", context);
        }
    }

    model.resources.reserve(resources.size());
    for (auto &[name, resource] : resources) {
        (void)name;
        model.resources.push_back(std::move(resource));
    }

    if (const auto routing = root.find("material_routing");
        routing != root.end() && !routing->is_null()) {
        if (!routing->is_object()) {
            throw invalid("material_routing must be an object");
        }
        const auto &routes =
            requireObjectField(*routing, "routes", "material_routing");
        model.material_routes.reserve(routes.size());
        for (const auto &[route_name, value] : routes.items()) {
            const std::string context =
                "material_routing.routes." + route_name;
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            model.material_routes.push_back(FramePlanMaterialRoute{
                .route = route_name,
                .pass = optionalStringField(value, "pass", context),
                .contract = optionalStringField(value, "contract", context),
                .shader_contract =
                    optionalStringField(value, "shader_contract", context),
                .phase = optionalStringField(value, "phase", context),
            });
        }
    }

    return model;
}

} // namespace PelicanStudio
