#include "frameexecutionadapter.hpp"

#include "../../project/targetrenderplanning.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace Pelican {
namespace {

inline constexpr std::string_view
    kGraphicsCapability =
        "pelican.execution.graphics@1";
inline constexpr std::string_view
    kComputeCapability =
        "pelican.execution.compute@1";
inline constexpr std::string_view
    kTransferCapability =
        "pelican.execution.transfer@1";
inline constexpr std::string_view
    kFrameOutputCapability =
        "pelican.execution.frame_output@1";

void appendUnique(
    std::vector<std::string> &values,
    std::string_view value) {
    if (std::find(
            values.begin(), values.end(),
            value) == values.end()) {
        values.emplace_back(value);
    }
}

bool contains(
    const std::vector<std::string> &values,
    std::string_view value) {
    return std::find(
               values.begin(), values.end(),
               value) != values.end();
}

std::string_view semanticDialect(
    FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render:
    case FramePlanNodeKind::anchor:
    case FramePlanNodeKind::output_transform:
        return "pelican.logical.render@1";
    case FramePlanNodeKind::compute:
        return "pelican.logical.compute@1";
    case FramePlanNodeKind::snapshot_copy:
        return "pelican.logical.transfer@1";
    }
    throw std::runtime_error(
        "unknown frame-plan node kind for execution dialect");
}

std::string_view selectedImplementation(
    FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render:
        return "pelican.execution.frame_render@1";
    case FramePlanNodeKind::compute:
        return "pelican.execution.frame_compute@1";
    case FramePlanNodeKind::anchor:
        return "pelican.execution.frame_anchor@1";
    case FramePlanNodeKind::snapshot_copy:
        return "pelican.execution.snapshot_copy@1";
    case FramePlanNodeKind::output_transform:
        return "pelican.execution.output_transform@1";
    }
    throw std::runtime_error(
        "unknown frame-plan node kind for execution implementation");
}

std::vector<std::string> requiredCapabilities(
    FramePlanNodeKind kind) {
    switch (kind) {
    case FramePlanNodeKind::render:
    case FramePlanNodeKind::anchor:
        return {std::string{kGraphicsCapability}};
    case FramePlanNodeKind::compute:
        return {std::string{kComputeCapability}};
    case FramePlanNodeKind::snapshot_copy:
        return {std::string{kTransferCapability}};
    case FramePlanNodeKind::output_transform:
        return {
            std::string{kGraphicsCapability},
            std::string{kFrameOutputCapability},
        };
    }
    throw std::runtime_error(
        "unknown frame-plan node kind for execution capabilities");
}

std::string_view dependencyReason(
    std::string_view barrier_kind) {
    if (barrier_kind ==
        "read_after_write") {
        return "pelican.dependency.read_after_write@1";
    }
    if (barrier_kind ==
        "write_after_write") {
        return "pelican.dependency.write_after_write@1";
    }
    throw std::runtime_error(
        "unknown frame-plan barrier kind for execution dependency: " +
        std::string{barrier_kind});
}

struct SourceNodeFacts {
    std::map<std::string, LogicalReadFootprint,
             std::less<>>
        footprints;
    std::map<std::string, LogicalAccessIntent,
             std::less<>>
        accesses;
    std::set<std::string, std::less<>>
        attachments;
};

SourceNodeFacts sourceFacts(
    const FrameGraphNodeDefinition &source) {
    SourceNodeFacts result;
    for (const auto &entry :
         source.read_footprints) {
        if (!result.footprints.emplace(
                 entry.resource,
                 entry.footprint)
                 .second) {
            throw std::runtime_error(
                "Execution adapter found duplicate read footprint in node '" +
                source.name + "': " +
                entry.resource);
        }
    }
    for (const auto &entry :
         source.resource_accesses) {
        if (!result.accesses.emplace(
                 entry.resource,
                 entry.intent)
                 .second) {
            throw std::runtime_error(
                "Execution adapter found duplicate resource access in node '" +
                source.name + "': " +
                entry.resource);
        }
    }
    for (const auto &attachment :
         source.attachments) {
        result.attachments.insert(
            attachment.resource);
    }
    return result;
}

LogicalAccessIntent accessIntent(
    const FrameGraphNodeDefinition *source,
    const SourceNodeFacts *facts,
    std::string_view resource,
    bool history) {
    if (source == nullptr || facts == nullptr) {
        return LogicalAccessIntent::automatic;
    }
    auto key = std::string{resource};
    if (history) key += "@history";
    if (const auto found =
            facts->accesses.find(key);
        found != facts->accesses.end()) {
        return found->second;
    }
    if (!history &&
        facts->attachments.contains(
            std::string{resource})) {
        return LogicalAccessIntent::attachment;
    }
    if (source->kind ==
        FramePlanNodeKind::snapshot_copy) {
        return LogicalAccessIntent::transfer;
    }
    return LogicalAccessIntent::automatic;
}

LogicalReadFootprint readFootprint(
    const SourceNodeFacts *facts,
    std::string_view resource,
    bool history) {
    if (history) {
        return {
            LogicalReadFootprintKind::temporal,
            std::nullopt,
        };
    }
    if (facts != nullptr) {
        if (const auto found =
                facts->footprints.find(
                    std::string{resource});
            found != facts->footprints.end()) {
            return found->second;
        }
        if (facts->attachments.contains(
                std::string{resource})) {
            return {
                LogicalReadFootprintKind::same_pixel,
                std::nullopt,
            };
        }
    }
    return {
        LogicalReadFootprintKind::arbitrary,
        std::nullopt,
    };
}

std::vector<ExecutionResourceUse>
resourceUses(
    const FramePlanNode &node,
    const FrameGraphNodeDefinition *source) {
    const auto facts =
        source != nullptr
            ? std::optional{sourceFacts(*source)}
            : std::nullopt;
    std::set<std::string, std::less<>>
        current_resources(
            node.reads.begin(), node.reads.end());
    current_resources.insert(
        node.writes.begin(), node.writes.end());

    std::vector<ExecutionResourceUse> result;
    result.reserve(
        current_resources.size() +
        node.reads_history.size());
    for (const auto &resource :
         current_resources) {
        const auto is_read =
            contains(node.reads, resource);
        const auto is_write =
            contains(node.writes, resource);
        const auto access =
            is_read && is_write
                ? LogicalAccessMode::read_write
            : is_read
                ? LogicalAccessMode::read
                : LogicalAccessMode::write;
        result.push_back(
            ExecutionResourceUse{
                .resource = resource,
                .epoch =
                    ExecutionResourceEpoch::current,
                .access = access,
                .intent = accessIntent(
                    source,
                    facts ? &*facts : nullptr,
                    resource, false),
                .footprint =
                    is_read
                        ? readFootprint(
                              facts
                                  ? &*facts
                                  : nullptr,
                              resource, false)
                        : LogicalReadFootprint{
                              LogicalReadFootprintKind::none,
                              std::nullopt},
            });
    }
    for (const auto &resource :
         node.reads_history) {
        result.push_back(
            ExecutionResourceUse{
                .resource = resource,
                .epoch =
                    ExecutionResourceEpoch::previous,
                .access =
                    LogicalAccessMode::read,
                .intent = accessIntent(
                    source,
                    facts ? &*facts : nullptr,
                    resource, true),
                .footprint =
                    readFootprint(
                        facts ? &*facts : nullptr,
                        resource, true),
            });
    }
    return result;
}

FrameExecutionNode executionNode(
    const FramePlanNode &node,
    const FrameGraphNodeDefinition *source,
    std::string_view endpoint) {
    auto result = FrameExecutionNode{
        .name = node.name,
        .declaration_index =
            node.declaration_index,
        .order = node.order,
        .level = node.level,
        .semantic_dialect =
            source != nullptr &&
                    !source->semantic_dialect.empty()
                ? source->semantic_dialect
                : std::string{
                      semanticDialect(node.kind)},
        .selected_implementation =
            source != nullptr &&
                    !source
                         ->execution_implementation
                         .empty()
                ? source
                      ->execution_implementation
                : std::string{
                      selectedImplementation(
                          node.kind)},
        .selected_endpoint =
            std::string{endpoint},
        .required_capabilities =
            requiredCapabilities(node.kind),
        .resource_uses =
            resourceUses(node, source),
        .view_family = node.view_family,
    };
    if (node.kind ==
        FramePlanNodeKind::output_transform) {
        result.effects.push_back(
            ExecutionEffect{
                .id =
                    "pelican.effect.frame_output@1",
                .subject =
                    node.writes.empty()
                        ? std::string{}
                        : node.writes.front(),
            });
    }
    return result;
}

void appendBarrierDependencies(
    const FramePlan &frame_plan,
    FrameExecutionPlan &execution_plan) {
    for (const auto &barrier :
         frame_plan.barriers) {
        execution_plan.dependencies.push_back(
            ExecutionDependency{
                .from = barrier.from,
                .to = barrier.to,
                .reason =
                    std::string{
                        dependencyReason(
                            barrier.kind)},
                .resource = barrier.resource,
            });
    }
}

FrameExecutionPlan buildPlan(
    const FramePlan &frame_plan,
    const FrameGraphDefinition *definition,
    ExecutionEndpoint endpoint) {
    if (frame_plan.name.empty()) {
        throw std::runtime_error(
            "Execution adapter requires a named FramePlan");
    }
    if (!frame_plan.nodes.empty() &&
        (endpoint.id.empty() ||
         endpoint.backend.empty())) {
        throw std::runtime_error(
            "Execution adapter requires a selected endpoint");
    }

    std::map<
        std::string,
        const FrameGraphNodeDefinition *,
        std::less<>>
        source_nodes;
    if (definition != nullptr) {
        if (definition->name !=
            frame_plan.name) {
            throw std::runtime_error(
                "Execution adapter graph name does not match FramePlan");
        }
        for (const auto &source :
             definition->nodes) {
            if (!source_nodes.emplace(
                     source.name, &source)
                     .second) {
                throw std::runtime_error(
                    "Execution adapter source has duplicate node: " +
                    source.name);
            }
        }
    }

    FrameExecutionPlan result;
    result.graph = frame_plan.name;
    if (!frame_plan.nodes.empty()) {
        result.endpoints.push_back(
            std::move(endpoint));
    }
    result.nodes.reserve(
        frame_plan.nodes.size());
    for (const auto &node :
         frame_plan.nodes) {
        const FrameGraphNodeDefinition *source =
            nullptr;
        if (definition != nullptr) {
            const auto found =
                source_nodes.find(node.name);
            if (found == source_nodes.end()) {
                throw std::runtime_error(
                    "Execution adapter cannot find source node: " +
                    node.name);
            }
            source = found->second;
            if (source->kind != node.kind) {
                throw std::runtime_error(
                    "Execution adapter node kind does not match FramePlan: " +
                    node.name);
            }
        }
        result.nodes.push_back(
            executionNode(
                node, source,
                result.endpoints.front().id));
        for (const auto &capability :
             result.nodes.back()
                 .required_capabilities) {
            appendUnique(
                result.endpoints.front()
                    .capabilities,
                capability);
        }
    }
    appendBarrierDependencies(
        frame_plan, result);

    if (definition != nullptr) {
        for (const auto &source :
             definition->nodes) {
            for (const auto &after :
                 source.after) {
                result.dependencies.push_back(
                    ExecutionDependency{
                        .from = after,
                        .to = source.name,
                        .reason =
                            "pelican.dependency.explicit_after@1",
                    });
            }
            for (const auto &before :
                 source.before) {
                result.dependencies.push_back(
                    ExecutionDependency{
                        .from = source.name,
                        .to = before,
                        .reason =
                            "pelican.dependency.explicit_before@1",
                    });
            }
            if (!source.snapshot_after.empty()) {
                result.dependencies.push_back(
                    ExecutionDependency{
                        .from =
                            source.snapshot_after,
                        .to = source.name,
                        .reason =
                            "pelican.dependency.snapshot_after@1",
                    });
            }
        }
    }

    result =
        canonicalizeFrameExecutionPlan(
            std::move(result));
    validateFrameExecutionPlanCompatibility(
        result, frame_plan);
    return result;
}

bool hasUse(
    const FrameExecutionNode &node,
    std::string_view resource,
    ExecutionResourceEpoch epoch,
    bool require_read,
    bool require_write) {
    const auto found = std::find_if(
        node.resource_uses.begin(),
        node.resource_uses.end(),
        [&](const auto &use) {
            return use.resource == resource &&
                   use.epoch == epoch;
        });
    if (found == node.resource_uses.end()) {
        return false;
    }
    const auto reads =
        found->access ==
            LogicalAccessMode::read ||
        found->access ==
            LogicalAccessMode::read_write;
    const auto writes =
        found->access ==
            LogicalAccessMode::write ||
        found->access ==
            LogicalAccessMode::read_write;
    return (!require_read || reads) &&
           (!require_write || writes);
}

} // namespace

ExecutionEndpoint selectedVulkanExecutionEndpoint(
    const VulkanTargetPlan &target_plan) {
    if (target_plan.backend_selection
            .selected_candidate.empty()) {
        throw std::runtime_error(
            "Vulkan target plan lacks a selected backend candidate: " +
            target_plan.graph);
    }
    const auto selected = std::find_if(
        target_plan.backend_selection
            .candidates.begin(),
        target_plan.backend_selection
            .candidates.end(),
        [&](const auto &candidate) {
            return candidate.candidate ==
                   target_plan.backend_selection
                       .selected_candidate;
        });
    if (selected ==
            target_plan.backend_selection
                .candidates.end() ||
        !selected->feasible ||
        selected->endpoint.empty()) {
        throw std::runtime_error(
            "Vulkan target plan selected candidate has no feasible endpoint: " +
            target_plan.graph);
    }
    return ExecutionEndpoint{
        .id = selected->endpoint,
        .endpoint_class =
            ExecutionEndpointClass::device,
        .backend = "vulkan",
        .capabilities =
            target_plan.required_physical_features,
    };
}

FrameExecutionPlan compileFrameExecutionPlan(
    const FrameGraphDefinition &definition,
    const FramePlan &frame_plan,
    ExecutionEndpoint endpoint) {
    return buildPlan(
        frame_plan, &definition,
        std::move(endpoint));
}

FrameExecutionPlan makeCompatibilityFrameExecutionPlan(
    const FramePlan &frame_plan,
    ExecutionEndpoint endpoint) {
    return buildPlan(
        frame_plan, nullptr,
        std::move(endpoint));
}

void validateFrameExecutionPlanCompatibility(
    const FrameExecutionPlan &execution_plan,
    const FramePlan &frame_plan) {
    validateFrameExecutionPlan(execution_plan);
    if (execution_plan.graph !=
        frame_plan.name) {
        throw std::runtime_error(
            "Frame execution plan graph does not match FramePlan");
    }
    if (execution_plan.nodes.size() !=
        frame_plan.nodes.size()) {
        throw std::runtime_error(
            "Frame execution plan node count does not match FramePlan: " +
            frame_plan.name);
    }
    for (std::size_t index = 0;
         index < frame_plan.nodes.size();
         ++index) {
        const auto &legacy =
            frame_plan.nodes[index];
        const auto &execution =
            execution_plan.nodes[index];
        if (execution.name != legacy.name ||
            execution.declaration_index !=
                legacy.declaration_index ||
            execution.order != legacy.order ||
            execution.level != legacy.level ||
            execution.view_family !=
                legacy.view_family) {
            throw std::runtime_error(
                "Frame execution node does not match FramePlan node at order " +
                std::to_string(index) + ": " +
                legacy.name);
        }
        for (const auto &resource :
             legacy.reads) {
            if (!hasUse(
                    execution, resource,
                    ExecutionResourceEpoch::current,
                    true, false)) {
                throw std::runtime_error(
                    "Frame execution node is missing current read: " +
                    legacy.name + "/" + resource);
            }
        }
        for (const auto &resource :
             legacy.reads_history) {
            if (!hasUse(
                    execution, resource,
                    ExecutionResourceEpoch::previous,
                    true, false)) {
                throw std::runtime_error(
                    "Frame execution node is missing history read: " +
                    legacy.name + "/" + resource);
            }
        }
        for (const auto &resource :
             legacy.writes) {
            if (!hasUse(
                    execution, resource,
                    ExecutionResourceEpoch::current,
                    false, true)) {
                throw std::runtime_error(
                    "Frame execution node is missing current write: " +
                    legacy.name + "/" + resource);
            }
        }
    }
    for (const auto &barrier :
         frame_plan.barriers) {
        const auto expected_reason =
            dependencyReason(barrier.kind);
        const auto found = std::find_if(
            execution_plan.dependencies.begin(),
            execution_plan.dependencies.end(),
            [&](const auto &dependency) {
                return dependency.from ==
                           barrier.from &&
                       dependency.to ==
                           barrier.to &&
                       dependency.resource ==
                           barrier.resource &&
                       dependency.reason ==
                           expected_reason;
            });
        if (found ==
            execution_plan.dependencies.end()) {
            throw std::runtime_error(
                "Frame execution plan is missing FramePlan barrier: " +
                barrier.from + " -> " +
                barrier.to);
        }
    }
}

} // namespace Pelican
