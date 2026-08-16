#include "frameplanmodel.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
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
        const auto &execution_nodes =
            requireArrayField(*execution, "nodes", "execution_plan");
        for (std::size_t index = 0; index < execution_nodes.size(); ++index) {
            const auto &value = execution_nodes.at(index);
            const std::string context =
                "execution_plan.nodes[" + std::to_string(index) + "]";
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            const std::string name = requireStringField(value, "name", context);
            const auto destination = node_indices.find(name);
            if (destination == node_indices.end()) {
                continue;
            }
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
    }

    std::unordered_map<std::string, std::size_t> decision_group_indices;
    if (const auto physical = root.find("physical_target_plan");
        physical != root.end() && !physical->is_null()) {
        if (!physical->is_object()) {
            throw invalid("physical_target_plan must be an object");
        }
        appendDecisions(*physical, "physical_target_plan",
                        model.decision_groups, decision_group_indices);
        if (const auto opportunities =
                physical->find("planning_opportunities");
            opportunities != physical->end() && !opportunities->is_null()) {
            if (!opportunities->is_object()) {
                throw invalid(
                    "physical_target_plan.planning_opportunities must be an object");
            }
            appendDecisions(*opportunities,
                            "physical_target_plan.planning_opportunities",
                            model.decision_groups, decision_group_indices);
        }
        if (const auto backend = physical->find("backend_selection");
            backend != physical->end() && !backend->is_null()) {
            parseBackendSelection(*backend, model, decision_group_indices);
        }
        if (const auto lowering = physical->find("lowering_graph");
            lowering != physical->end() && !lowering->is_null()) {
            if (!lowering->is_object()) {
                throw invalid(
                    "physical_target_plan.lowering_graph must be an object");
            }
            appendDecisions(*lowering,
                            "physical_target_plan.lowering_graph",
                            model.decision_groups, decision_group_indices);
        }
        if (const auto attachments = physical->find("attachments");
            attachments != physical->end()) {
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
                // Physical-only scopes are deliberately outside this
                // high-level model.
                if (destination == node_indices.end()) {
                    continue;
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
        for (std::size_t index = 0; index < declarations->size(); ++index) {
            const auto &value = declarations->at(index);
            const std::string context =
                "resources[" + std::to_string(index) + "]";
            if (!value.is_object()) {
                throw invalid(context + " must be an object");
            }
            const std::string name =
                requireStringField(value, "name", context);
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

    if (const auto physical = root.find("physical_target_plan");
        physical != root.end() && physical->is_object()) {
        if (const auto physical_resources = physical->find("resources");
            physical_resources != physical->end()) {
            if (!physical_resources->is_array()) {
                throw invalid("physical_target_plan.resources must be an array");
            }
            for (std::size_t index = 0; index < physical_resources->size();
                 ++index) {
                const auto &value = physical_resources->at(index);
                const std::string context =
                    "physical_target_plan.resources[" +
                    std::to_string(index) + "]";
                if (!value.is_object()) {
                    throw invalid(context + " must be an object");
                }
                const std::string name =
                    requireStringField(value, "logical_resource", context);
                auto &resource = resources[name];
                resource.name = name;
                const std::string format =
                    optionalStringField(value, "format", context);
                if (!format.empty()) {
                    resource.format = format;
                }
                resource.reason =
                    optionalStringField(value, "reason", context);
                resource.dimension =
                    optionalStringField(value, "dimension", context);
                if (const auto extent = value.find("extent");
                    extent != value.end() && !extent->is_null()) {
                    if (!extent->is_object()) {
                        throw invalid(context + ".extent must be an object");
                    }
                    const auto width = optionalSizeField(
                        *extent, "width", context + ".extent");
                    const auto height = optionalSizeField(
                        *extent, "height", context + ".extent");
                    if (width && height && *width > 0 && *height > 0) {
                        resource.width = width;
                        resource.height = height;
                    }
                }
            }
        }
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
