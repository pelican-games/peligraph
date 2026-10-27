#include "frameplanner.hpp"
#include "../../project/imageformat.hpp"
#include "../../project/rasterrayquery.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "../../project/materialformat.hpp"
#include "../../project/imagesubresourcejson.hpp"
#include "../../project/passshapepolicy.hpp"
#include "../../project/renderresourcename.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "../../project/materialscreeninput.hpp"
#include "../../project/renderconfigsection.hpp"
#include "../renderer/viewfamilyproviderregistry.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {

namespace {

// LP-Va2: get_frame_plan's nodes[].view_axes. The table is the provider's
// (renderViewFamilyAxes); this only writes it out.
nlohmann::json viewAxesJson(
    std::string_view family_id,
    const RenderViewAxisLengthContext &context) {
    auto axes = nlohmann::json::array();
    for (const auto &axis :
         renderViewFamilyAxes(family_id, context)) {
        const bool runtime =
            axis.length.kind ==
            RenderViewAxisLengthKind::runtime;
        auto entry = nlohmann::json{
            {"axis", axis.name},
            {"length", runtime ? nlohmann::json(nullptr)
                               : nlohmann::json(axis.length.length)},
            {"runtime", runtime},
        };
        if (!axis.labels.empty()) {
            entry["labels"] = axis.labels;
        }
        axes.push_back(std::move(entry));
    }
    return axes;
}

// LP-1a: `loop_origin` on a node or resource a graph.loop expanded.
nlohmann::json loopOriginJson(const RenderLoopOrigin &origin) {
    return nlohmann::json{
        {"loop", origin.loop},
        {"element", origin.element},
        {"node", origin.node},
    };
}

// GL-2a: the node's `origin` (see framePlanToJson in the header).
nlohmann::json graphNodeOriginJson(
    const RenderPassProvenance &provenance,
    std::string_view graph_reference) {
    const auto &origin = *provenance.graph_origin;
    auto result = nlohmann::json{
        {"graph", std::string{graph_reference}},
        {"node", origin.node},
    };
    if (!origin.definition.empty()) {
        result["definition"] = origin.definition;
    }
    auto instance = origin.instance_path;
    while (instance.ends_with('/')) instance.pop_back();
    if (!instance.empty()) result["instance_path"] = std::move(instance);
    if (!origin.loop.empty()) result["loop"] = origin.loop;
    if (origin.loop_branch) result["loop_branch"] = *origin.loop_branch;
    if (provenance.loop_origin) {
        result["element"] = provenance.loop_origin->element;
    }
    return result;
}

std::string_view renderPipelineDiagnosticKindName(
    RenderPipelineDiagnosticKind kind) {
    switch (kind) {
    case RenderPipelineDiagnosticKind::graph_variant_selected:
        return "graph_variant_selected";
    case RenderPipelineDiagnosticKind::feature_excluded:
        return "feature_excluded";
    case RenderPipelineDiagnosticKind::pipeline_preset_resolved:
        return "pipeline_preset_resolved";
    case RenderPipelineDiagnosticKind::graph_node_deactivated:
        return "graph_node_deactivated";
    case RenderPipelineDiagnosticKind::fold_stale:
        return "fold_stale";
    case RenderPipelineDiagnosticKind::anchor_ignored:
        return "frame_node_order.anchor_ignored@1";
    case RenderPipelineDiagnosticKind::fold_unverified:
        return "fold_unverified";
    }
    throw std::runtime_error("unknown render pipeline diagnostic kind");
}

} // namespace

nlohmann::json framePlanToJson(
    const FramePlan &plan,
    const CompiledRenderPipeline *render_pipeline,
    std::string_view graph_reference) {
    std::unordered_map<std::string, const RenderPassProvenance *>
        pass_provenance;
    if (render_pipeline != nullptr) {
        pass_provenance.reserve(
            render_pipeline->pass_provenance.size());
        for (const auto &entry :
             render_pipeline->pass_provenance) {
            pass_provenance.emplace(entry.name, &entry);
        }
    }
    // LP-Va2: compile-time axis lengths read the composed pipeline of the
    // generation this plan belongs to. Without one (a raw plan) the providers
    // see no feature and answer their defaults.
    const auto axis_context =
        render_pipeline != nullptr
            ? renderViewAxisLengthContext(*render_pipeline)
            : RenderViewAxisLengthContext{};
    const auto findExtentReference =
        [&](std::string_view family)
        -> const RenderViewFamilyExtentReference * {
        if (render_pipeline == nullptr ||
            family == mainRenderViewFamilyId) {
            return nullptr;
        }
        for (const auto &reference :
             render_pipeline->view_family_extents) {
            if (reference.family == family) {
                return &reference;
            }
        }
        return nullptr;
    };
    auto nodes_json = nlohmann::json::array();
    for (const auto &node : plan.nodes) {
        auto node_json = nlohmann::json{
            {"declaration_index", node.declaration_index},
            {"kind", framePlanNodeKindName(node.kind)},
            {"name", node.name},
            {"order", node.order},
            {"reads", node.reads},
            {"reads_history", node.reads_history},
            {"writes", node.writes},
        };
        if (const auto found = pass_provenance.find(node.name);
            found != pass_provenance.end()) {
            const auto &provenance = *found->second;
            if (provenance.fragment)
                node_json["fragment"] = {{"name", provenance.fragment->name}, {"reference", provenance.fragment->reference}};
            if (!provenance.authored_name.empty())
                node_json["authored_name"] = provenance.authored_name;
            node_json["source"] =
                renderPipelineProvenanceSourceName(
                    provenance.source,
                    provenance.provider_feature);
            if (!provenance.provider_feature.empty()) {
                node_json["provider_feature"] =
                    provenance.provider_feature;
            }
            if (!provenance.provider_reference.empty()) {
                node_json["provider_ref"] =
                    provenance.provider_reference;
            }
            if (provenance.loop_origin) {
                node_json["loop_origin"] =
                    loopOriginJson(*provenance.loop_origin);
            }
            if (provenance.graph_origin) {
                node_json["origin"] =
                    graphNodeOriginJson(provenance, graph_reference);
            }
            if (!provenance.replaces.empty()) {
                auto replaces = nlohmann::json::array();
                for (const auto &entry : provenance.replaces) {
                    replaces.push_back(nlohmann::json{
                        {"feature", entry.feature},
                        {"pass", entry.pass},
                    });
                }
                node_json["replaces"] = std::move(replaces);
            }
            if (!provenance.added_after.empty()) {
                node_json["added_after"] = provenance.added_after;
            }
            if (!provenance.loop_outputs.empty()) {
                auto references = nlohmann::json::array();
                for (const auto &entry : provenance.loop_outputs) {
                    references.push_back(nlohmann::json{
                        {"reference", entry.reference},
                        {"resource", entry.resource},
                    });
                }
                node_json["loop_outputs"] = std::move(references);
            }
        }
        if (node.kind == FramePlanNodeKind::snapshot_copy) {
            node_json["byte_size"] = node.byte_size;
            node_json["snapshot_after"] = node.snapshot_after;
            node_json["semantics"] = "fixed_once_before_transparency";
            node_json["sequential_refraction"] = false;
        }
        if (node.material_filter) {
            node_json["material_filter"] = {
                {"include",
                 node.material_filter->include},
                {"exclude",
                 node.material_filter->exclude},
                {"filter_id",
                 stableFingerprint64String(
                     node.material_filter
                         ->id.value)},
                {"resolved_draw_count", nullptr},
                {"resolution_provenance",
                 materialDrawTagFilterProvenance},
                {"resolution_state",
                 "pending_draw_queue_compile"},
            };
        }
        if (node.material_variant) {
            node_json["material_variant"] =
                *node.material_variant;
        }
        if (node.view_family !=
            mainRenderViewFamilyId) {
            node_json["view_family"] =
                node.view_family;
        }
        // LP-Va: the view axes this node is restricted to (Studio shows
        // them). Absent when the node runs every view of its family.
        if (!node.view_select.empty()) {
            node_json["view_select"] =
                node.view_select;
        }
        // PS-1: a once node. The renderer's get_frame_plan adds when it last
        // ran (last_run_generation, last_run_frame). Absent on a node that
        // runs every frame.
        if (node.once) {
            node_json["schedule"] = "once";
        }
        // LP-Va2: the axes of the node's family, so Studio builds the
        // view_select field from the engine's table. `[]` for `$main` and a
        // family without a provider. A snapshot copy is an authored pass too
        // (the cube capture loop body selects faces on its copies).
        if (node.kind == FramePlanNodeKind::render ||
            node.kind == FramePlanNodeKind::compute ||
            node.kind == FramePlanNodeKind::snapshot_copy) {
            node_json["view_axes"] =
                viewAxesJson(node.view_family, axis_context);
            // VF-2: the extent VF-1 holds the node's secondary family to,
            // the same reference its refusal names as expected_*. Absent on
            // `$main`, on a raw plan, and for a family none of whose raster
            // passes writes a declared target.
            if (const auto *reference =
                    findExtentReference(node.view_family)) {
                node_json["family_extent"] =
                    renderViewFamilyExtentJson(*reference);
                node_json["family_layers"] = reference->layers;
                node_json["family_extent_pass"] = reference->pass;
            }
        }
        nodes_json.push_back(std::move(node_json));
    }

    auto barriers_json = nlohmann::json::array();
    for (const auto &barrier : plan.barriers) {
        barriers_json.push_back(nlohmann::json{
            {"from", barrier.from},
            {"kind", barrier.kind},
            {"resource", barrier.resource},
            {"to", barrier.to},
        });
    }

    auto result = nlohmann::json{
        {"barriers", barriers_json},
        {"graph", plan.name},
        {"nodes", nodes_json},
        {"schema", "pelican.frame_plan"},
        {"version", 1},
    };
    if (render_pipeline != nullptr) {
        const auto metadata =
            serializeCompiledRenderPipelineMetadata(*render_pipeline);
        for (auto field = metadata.begin(); field != metadata.end(); ++field) {
            result[field.key()] = field.value();
        }
        if (!render_pipeline->resource_provenance.empty()) {
            auto resources = nlohmann::json::array();
            for (const auto &provenance :
                 render_pipeline->resource_provenance) {
                auto resource = nlohmann::json{
                    {"kind", provenance.kind},
                    {"name", provenance.name},
                    {"source",
                     renderPipelineProvenanceSourceName(
                         provenance.source,
                         provenance.provider_feature)},
                };
                if (!provenance.provider_feature.empty()) {
                    resource["provider_feature"] =
                        provenance.provider_feature;
                }
                if (!provenance.provider_reference.empty()) {
                    resource["provider_ref"] =
                        provenance.provider_reference;
                }
                if (provenance.usage) {
                    // Fragment identity is authoring ownership, separate from usage.
                    resource["usage"] = *provenance.usage;
                }
                if (provenance.fragment)
                    resource["fragment"] = {{"name", provenance.fragment->name}, {"reference", provenance.fragment->reference}};
                if (!provenance.added_usage.empty()) {
                    resource["added_usage"] = provenance.added_usage;
                }
                if (provenance.loop_origin) {
                    resource["loop_origin"] =
                        loopOriginJson(*provenance.loop_origin);
                }
                resources.push_back(std::move(resource));
            }
            result["resources"] = std::move(resources);
        }
        // GL-2a: the pipeline's diagnostics and the graph nodes the variant
        // made inactive, in the form the compiled pipeline keeps them.
        auto diagnostics = nlohmann::json::array();
        for (const auto &diagnostic : render_pipeline->diagnostics) {
            diagnostics.push_back(nlohmann::json{
                {"kind", renderPipelineDiagnosticKindName(diagnostic.kind)},
                {"subject", diagnostic.subject},
                // FG-1: a detail with fields is reported as they are.
                {"detail", diagnostic.detail_fields.is_null()
                               ? nlohmann::json(diagnostic.detail)
                               : diagnostic.detail_fields},
            });
        }
        // PO-1: what a node writes that nothing consumes, judged by the
        // usage the pipeline's render targets declare.
        std::unordered_map<std::string, std::vector<std::string>> image_usage;
        for (const auto &provenance : render_pipeline->resource_provenance) {
            if (provenance.usage) {
                image_usage.emplace(provenance.name, *provenance.usage);
            }
        }
        for (const auto &unread :
             findFramePlanUnreadOutputs(plan, image_usage)) {
            diagnostics.push_back(nlohmann::json{
                {"kind", "unread_output"},
                {"subject", unread.node},
                {"detail", {{"resource", unread.resource}}},
            });
        }
        result["diagnostics"] = std::move(diagnostics);
        auto deactivated = nlohmann::json::array();
        for (const auto &node : render_pipeline->deactivated_graph_nodes) {
            deactivated.push_back(nlohmann::json{
                {"node", node.node},
                {"feature", node.feature},
                {"reason", node.reason},
            });
        }
        result["deactivated_graph_nodes"] = std::move(deactivated);
        // FG-1: the target_planning entries of this graph the verified fold
        // groups added. Absent when there is none.
        auto fold_decisions = nlohmann::json::array();
        for (const auto &entry : render_pipeline->fold_planning) {
            if (entry.graph != plan.name) continue;
            fold_decisions.push_back(nlohmann::json{
                {"group", entry.group},
                {"kind", entry.kind},
                {"name", entry.name},
                {"field", entry.field},
            });
        }
        if (!fold_decisions.empty())
            result["fold_decisions"] = std::move(fold_decisions);
        // LP-1a: every graph.loop, including one that expanded nothing.
        // Absent when the composed graph has no loop.
        if (!render_pipeline->graph_loops.empty()) {
            auto loops = nlohmann::json::array();
            for (const auto &loop : render_pipeline->graph_loops) {
                auto elements = nlohmann::json::array();
                for (const auto &element : loop.elements) {
                    elements.push_back(nlohmann::json{
                        {"element", element.element},
                        {"index", element.index},
                        {"branch", element.branch},
                    });
                }
                auto entry = nlohmann::json{
                    {"node", loop.node},
                    {"tier", loop.tier},
                    {"count", loop.count},
                    {"elements", std::move(elements)},
                    {"unmatched_branches",
                     loop.unmatched_branches},
                };
                // LP-3a: a draw_loop names its material pass; its elements
                // are the running generation's materials, which
                // applyMaterialDrawLoopElementsToFramePlanJson fills in.
                if (!loop.pass.empty()) entry["pass"] = loop.pass;
                loops.push_back(std::move(entry));
            }
            result["loops"] = std::move(loops);
        }
    }
    return result;
}

void applyMaterialDrawFilterResolutionToFramePlanJson(
    nlohmann::json &plan_json,
    std::string_view pass_name,
    MaterialDrawTagFilterId filter_id,
    std::size_t resolved_draw_count,
    const std::vector<std::string> &unmatched_include,
    const std::vector<std::string> &unmatched_exclude) {
    if (!plan_json.is_object() ||
        !plan_json.contains("nodes") ||
        !plan_json.at("nodes").is_array()) {
        throw std::invalid_argument(
            "frame plan JSON has no node array");
    }
    const auto node = std::find_if(
        plan_json["nodes"].begin(),
        plan_json["nodes"].end(),
        [&](const auto &candidate) {
            return candidate.is_object() &&
                   candidate.value(
                       "name", std::string{}) ==
                       pass_name;
        });
    if (node == plan_json["nodes"].end() ||
        !node->contains("material_filter") ||
        !node->at("material_filter").is_object()) {
        throw std::invalid_argument(
            "frame plan material filter node is unavailable: " +
            std::string{pass_name});
    }
    auto &metadata =
        (*node)["material_filter"];
    const auto expected_id =
        stableFingerprint64String(
            filter_id.value);
    if (metadata.value(
            "filter_id", std::string{}) !=
        expected_id) {
        throw std::invalid_argument(
            "frame plan material filter id mismatch: " +
            std::string{pass_name});
    }
    metadata["resolved_draw_count"] =
        resolved_draw_count;
    metadata["unmatched_include"] =
        unmatched_include;
    metadata["unmatched_exclude"] =
        unmatched_exclude;
    metadata["resolution_state"] =
        "resolved";
}

void appendFramePlanPhysicalNodeReferences(nlohmann::json &plan_json) {
    if (!plan_json.is_object()) return;
    const auto physical = plan_json.find("physical_target_plan");
    if (physical == plan_json.end() || !physical->is_object()) return;
    using Names = std::set<std::string>;
    const auto strings = [](const nlohmann::json &holder, const char *key) {
        std::vector<std::string> result;
        const auto found = holder.find(key);
        if (found == holder.end() || !found->is_array()) return result;
        for (const auto &item : *found)
            if (item.is_string()) result.push_back(item.get<std::string>());
        return result;
    };
    const auto field = [](const nlohmann::json &holder, const char *key) {
        const auto found = holder.find(key);
        return found != holder.end() && found->is_string() ? found->get<std::string>() : std::string{};
    };
    // The frame plan's nodes, in its order, and what each touches.
    std::unordered_map<std::string, std::size_t> node_index;
    std::unordered_map<std::string, Names> resource_nodes;
    if (const auto nodes = plan_json.find("nodes"); nodes != plan_json.end() && nodes->is_array()) {
        for (const auto &node : *nodes) {
            if (!node.is_object()) continue;
            const auto name = field(node, "name");
            if (name.empty()) continue;
            node_index.emplace(name, node_index.size());
            for (const auto *key : {"reads", "writes", "reads_history"})
                for (const auto &resource : strings(node, key)) resource_nodes[resource].insert(name);
        }
    }
    const auto ordered = [&](const Names &names) {
        std::vector<std::string> sorted(names.begin(), names.end());
        std::sort(sorted.begin(), sorted.end(), [&](const std::string &left, const std::string &right) {
            return node_index.at(left) < node_index.at(right);
        });
        return nlohmann::json(sorted);
    };
    // A lowering node is a frame plan node, or stands for the ones it was
    // made from (a subgraph replacement's `sources`).
    std::unordered_map<std::string, Names> lowering_nodes;
    if (const auto lowering = physical->find("lowering_graph");
        lowering != physical->end() && lowering->is_object()) {
        if (const auto nodes = lowering->find("nodes"); nodes != lowering->end() && nodes->is_array()) {
            for (const auto &node : *nodes) {
                if (!node.is_object()) continue;
                const auto name = field(node, "name");
                auto &names = lowering_nodes[name];
                if (node_index.contains(name)) names.insert(name);
                for (const auto &source : strings(node, "sources"))
                    if (node_index.contains(source)) names.insert(source);
            }
        }
    }
    const auto loweringNodes = [&](const std::string &name) {
        if (const auto found = lowering_nodes.find(name); found != lowering_nodes.end()) return found->second;
        return node_index.contains(name) ? Names{name} : Names{};
    };
    // An attachment names its node and resource together.
    if (const auto attachments = physical->find("attachments");
        attachments != physical->end() && attachments->is_array()) {
        for (const auto &attachment : *attachments) {
            if (!attachment.is_object()) continue;
            const auto nodes = loweringNodes(field(attachment, "node"));
            const auto resource = field(attachment, "logical_resource");
            if (!resource.empty()) resource_nodes[resource].insert(nodes.begin(), nodes.end());
        }
    }
    const auto resourceNodes = [&](const std::string &name) {
        const auto found = resource_nodes.find(name);
        return found != resource_nodes.end() ? found->second : Names{};
    };
    if (const auto resources = physical->find("resources"); resources != physical->end() && resources->is_array())
        for (auto &resource : *resources)
            if (resource.is_object())
                resource["from_nodes"] = ordered(resourceNodes(field(resource, "logical_resource")));
    std::unordered_map<std::string, Names> group_nodes;
    if (const auto groups = physical->find("alias_groups"); groups != physical->end() && groups->is_array()) {
        for (auto &group : *groups) {
            if (!group.is_object()) continue;
            Names names;
            for (const auto &member : strings(group, "resources")) {
                const auto nodes = resourceNodes(member);
                names.insert(nodes.begin(), nodes.end());
            }
            group["from_nodes"] = ordered(names);
            group_nodes.emplace(field(group, "id"), std::move(names));
        }
    }
    std::unordered_map<std::string, Names> scope_nodes;
    if (const auto scopes = physical->find("scopes"); scopes != physical->end() && scopes->is_array()) {
        for (const auto &scope : *scopes) {
            if (!scope.is_object()) continue;
            Names names;
            for (const auto &node : strings(scope, "nodes")) {
                const auto mapped = loweringNodes(node);
                names.insert(mapped.begin(), mapped.end());
            }
            scope_nodes.emplace(field(scope, "id"), std::move(names));
        }
    }
    // What a subject names: a node, a resource, an alias group, a scope --
    // or the whole graph (nothing).
    const auto subjectNodes = [&](const std::string &subject) -> Names {
        if (subject.empty()) return {};
        if (const auto nodes = loweringNodes(subject); !nodes.empty()) return nodes;
        if (const auto found = resource_nodes.find(subject); found != resource_nodes.end()) return found->second;
        if (const auto found = group_nodes.find(subject); found != group_nodes.end()) return found->second;
        if (const auto found = scope_nodes.find(subject); found != scope_nodes.end()) return found->second;
        return {};
    };
    const auto annotateDecisions = [&](nlohmann::json &container) {
        if (!container.is_object()) return;
        const auto decisions = container.find("decisions");
        if (decisions == container.end() || !decisions->is_array()) return;
        for (auto &decision : *decisions)
            if (decision.is_object()) decision["from_nodes"] = ordered(subjectNodes(field(decision, "subject")));
    };
    // FG-1: each target_planning entry a fold group added is a decision of
    // the plan's, marked with the group (from_fold).
    std::vector<std::pair<std::size_t, std::string>> fold_marks;
    if (const auto folds = plan_json.find("fold_decisions"); folds != plan_json.end() && folds->is_array()) {
        auto &decisions = (*physical)["decisions"];
        if (decisions.is_null()) decisions = nlohmann::json::array();
        if (decisions.is_array()) {
            for (const auto &entry : *folds) {
                if (!entry.is_object()) continue;
                const auto group = field(entry, "group");
                const auto kind = field(entry, "kind");
                fold_marks.emplace_back(decisions.size(), group);
                decisions.push_back(nlohmann::json{
                    {"id", "pelican.plan.fold_decision_applied@1"},
                    {"subject", field(entry, "name")},
                    {"selected", field(entry, "field")},
                    {"detail", "fold group " + group + " declared it in target_planning " +
                                   (kind == "node" ? "nodes" : "resources")},
                });
            }
        }
    }
    annotateDecisions(*physical);
    for (const auto &[index, group] : fold_marks) (*physical)["decisions"][index]["from_fold"] = group;
    for (const auto *key : {"lowering_graph", "planning_opportunities", "backend_selection"})
        if (const auto container = physical->find(key); container != physical->end())
            annotateDecisions(*container);
    if (const auto selection = physical->find("backend_selection");
        selection != physical->end() && selection->is_object()) {
        if (const auto candidates = selection->find("candidates");
            candidates != selection->end() && candidates->is_array()) {
            for (auto &candidate : *candidates) {
                if (!candidate.is_object()) continue;
                Names names;
                for (const auto *key : {"failures", "diagnostics"}) {
                    const auto entries = candidate.find(key);
                    if (entries == candidate.end() || !entries->is_array()) continue;
                    for (const auto &entry : *entries) {
                        if (!entry.is_object()) continue;
                        const auto nodes = subjectNodes(field(entry, "subject"));
                        names.insert(nodes.begin(), nodes.end());
                    }
                }
                candidate["from_nodes"] = ordered(names);
            }
        }
    }
}

} // namespace Pelican
