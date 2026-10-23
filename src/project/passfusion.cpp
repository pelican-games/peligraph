#include "passfusion.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace Pelican {

namespace {

constexpr std::array kBoundaryReasons{
    PassFusionBoundaryReason::not_fullscreen,
    PassFusionBoundaryReason::scheduled_once,
    PassFusionBoundaryReason::fuse_disabled,
    PassFusionBoundaryReason::view_mismatch,
    PassFusionBoundaryReason::read_neighborhood,
    PassFusionBoundaryReason::read_arbitrary,
    PassFusionBoundaryReason::read_temporal,
    PassFusionBoundaryReason::multiple_chain_inputs,
    PassFusionBoundaryReason::in_place_read_outside,
    PassFusionBoundaryReason::resource_facts_missing,
    PassFusionBoundaryReason::extent_mismatch,
    PassFusionBoundaryReason::fan_out,
    PassFusionBoundaryReason::fan_in,
    PassFusionBoundaryReason::format_class_mismatch,
    PassFusionBoundaryReason::reads_other_member_output,
    PassFusionBoundaryReason::outside_dependency,
    PassFusionBoundaryReason::cycle_with_other_chain,
};

struct ReadSite {
    std::size_t node = 0;
    std::size_t use = 0;
};

struct Candidate {
    std::size_t producer = 0;
    std::size_t consumer = 0;
    // The consumer's use that reads `value`.
    std::size_t use = 0;
    LogicalValueId value;
    std::optional<PassFusionBoundaryReason> reason;
    std::string detail;
    bool linked = false;
};

struct Finding {
    PassFusionBoundaryReason reason;
    std::string detail;
};

std::string formatScale(float value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string extentName(const ResourceExtentPlan &extent) {
    if (extent.kind == ResourceExtentKind::fixed) {
        return std::to_string(extent.width) + "x" +
               std::to_string(extent.height);
    }
    if (extent.scale_x == extent.scale_y) {
        return "output*" + formatScale(extent.scale_x);
    }
    return "output*" + formatScale(extent.scale_x) + "x" +
           formatScale(extent.scale_y);
}

std::string footprintName(const LogicalReadFootprint &footprint) {
    std::string result{logicalReadFootprintKindName(footprint.kind)};
    if (footprint.radius) {
        result += " radius " + std::to_string(*footprint.radius);
    }
    return result;
}

std::string viewName(const LogicalGraphNode &node) {
    std::string result = node.view_family;
    for (const auto &[axis, value] : node.view_select) {
        result += " " + axis + "=" + std::to_string(value);
    }
    return result;
}

class GraphPlanner {
  public:
    GraphPlanner(const CompiledLogicalRenderGraph &graph,
                 const PassFusionGraphFacts &facts,
                 const PlanningGraphConstraints *constraints)
        : graph_(graph), count_(graph.nodes.size()) {
        indexNodes();
        readNodeFacts(facts);
        readResourceFacts(facts);
        readConstraints(constraints);
        indexValues();
        buildDependencies();
        orderNodes();
        collectCandidates();
    }

    void plan(PassFusionPlan &result) {
        for (auto &candidate : candidates_) {
            if (auto finding = localFinding(candidate)) {
                candidate.reason = finding->reason;
                candidate.detail = std::move(finding->detail);
            }
        }
        growChains(result);
        for (const auto &candidate : candidates_) {
            if (candidate.linked) continue;
            if (!candidate.reason) {
                throw std::logic_error(
                    "pass fusion left an edge without a decision: " +
                    name(candidate.producer) + " -> " +
                    name(candidate.consumer));
            }
            result.boundaries.push_back(PassFusionBoundary{
                .graph = graph_.name,
                .producer = name(candidate.producer),
                .consumer = name(candidate.consumer),
                .value = candidate.value,
                .reason = *candidate.reason,
                .detail = candidate.detail,
            });
        }
    }

  private:
    const std::string &name(std::size_t node) const {
        return graph_.nodes[node].name;
    }

    void indexNodes() {
        for (std::size_t index = 0; index < count_; ++index) {
            if (!index_.emplace(graph_.nodes[index].name, index).second) {
                throw std::runtime_error(
                    "pass fusion graph '" + graph_.name +
                    "' has a duplicate node: " + graph_.nodes[index].name);
            }
        }
    }

    std::size_t requireNode(std::string_view node,
                            std::string_view context) const {
        const auto found = index_.find(node);
        if (found == index_.end()) {
            throw std::runtime_error(
                std::string{context} + " references unknown node: " +
                graph_.name + "." + std::string{node});
        }
        return found->second;
    }

    void readNodeFacts(const PassFusionGraphFacts &facts) {
        fullscreen_.assign(count_, false);
        once_.assign(count_, false);
        std::vector<bool> seen(count_, false);
        for (const auto &entry : facts.nodes) {
            const auto node = requireNode(entry.node, "pass fusion facts");
            if (seen[node]) {
                throw std::runtime_error(
                    "pass fusion facts repeat node: " + graph_.name + "." +
                    entry.node);
            }
            seen[node] = true;
            fullscreen_[node] = entry.fullscreen;
            once_[node] = entry.once;
        }
        for (std::size_t node = 0; node < count_; ++node) {
            if (!seen[node]) {
                throw std::runtime_error(
                    "pass fusion facts have no entry for node: " +
                    graph_.name + "." + name(node));
            }
        }
    }

    void readResourceFacts(const PassFusionGraphFacts &facts) {
        for (const auto &resource : graph_.resources) {
            resources_.emplace(resource.name, &resource);
        }
        for (const auto &entry : facts.resources) {
            if (!resources_.contains(entry.resource)) {
                throw std::runtime_error(
                    "pass fusion facts reference unknown resource: " +
                    graph_.name + "." + entry.resource);
            }
            if (!resource_facts_.emplace(entry.resource, &entry).second) {
                throw std::runtime_error(
                    "pass fusion facts repeat resource: " + graph_.name +
                    "." + entry.resource);
            }
        }
    }

    void readConstraints(const PlanningGraphConstraints *constraints) {
        fuse_disabled_.assign(count_, false);
        if (constraints == nullptr) return;
        for (const auto &constraint : constraints->nodes) {
            if (constraint.fuse) continue;
            fuse_disabled_[requireNode(
                constraint.node, "pass fusion constraint")] = true;
        }
    }

    void indexValues() {
        outputs_.resize(count_);
        for (const auto &imported : graph_.imports) {
            if (imported.kind == LogicalValueImportKind::previous_epoch) {
                history_resources_.insert(imported.value.resource);
            }
        }
        for (std::size_t node = 0; node < count_; ++node) {
            const auto &uses = graph_.nodes[node].uses;
            for (std::size_t use = 0; use < uses.size(); ++use) {
                if (uses[use].output_value) {
                    const auto &value = *uses[use].output_value;
                    if (!producer_.emplace(value, node).second) {
                        throw std::runtime_error(
                            "pass fusion graph value has two producers: " +
                            graph_.name + "." + logicalValueIdName(value));
                    }
                    outputs_[node].push_back(value);
                    auto &last = last_version_[value.resource];
                    last = std::max(last, value.version);
                }
                if (uses[use].input_value) {
                    readers_[*uses[use].input_value].push_back(
                        ReadSite{node, use});
                }
            }
        }
    }

    void addReach(std::size_t from, std::size_t to) {
        // Everything that reaches `from` (and `from`) now reaches
        // everything `to` reaches (and `to`).
        for (std::size_t source = 0; source < count_; ++source) {
            if (source != from && !reach_[source][from]) continue;
            reach_[source][to] = true;
            for (std::size_t target = 0; target < count_; ++target) {
                if (reach_[to][target]) reach_[source][target] = true;
            }
        }
    }

    void buildDependencies() {
        successors_.assign(count_, {});
        const auto add = [&](std::size_t from, std::size_t to) {
            if (from == to) {
                throw std::runtime_error(
                    "pass fusion graph node depends on itself: " +
                    graph_.name + "." + name(from));
            }
            successors_[from].insert(to);
        };
        for (const auto &[value, sites] : readers_) {
            const auto producer = producer_.find(value);
            if (producer == producer_.end()) continue;
            for (const auto &site : sites) add(producer->second, site.node);
        }
        for (std::size_t node = 0; node < count_; ++node) {
            for (const auto &dependency : graph_.nodes[node].after) {
                add(requireNode(dependency, "pass fusion dependency"), node);
            }
            for (const auto &dependent : graph_.nodes[node].before) {
                add(node, requireNode(dependent, "pass fusion dependency"));
            }
        }
        // An anchor (a canonical phase boundary) only orders: everything
        // before it runs before everything after it. Its predecessors are
        // joined to its successors and the anchor leaves the order, so two
        // passes on either side of one phase boundary are consecutive.
        for (std::size_t anchor = 0; anchor < count_; ++anchor) {
            if (graph_.nodes[anchor].kind != LogicalGraphNodeKind::anchor ||
                !graph_.nodes[anchor].uses.empty()) {
                continue;
            }
            const auto successors = std::move(successors_[anchor]);
            successors_[anchor].clear();
            for (std::size_t node = 0; node < count_; ++node) {
                if (successors_[node].erase(anchor) == 0) continue;
                successors_[node].insert(successors.begin(),
                                         successors.end());
            }
        }

        reach_.assign(count_, std::vector<bool>(count_, false));
        for (std::size_t source = 0; source < count_; ++source) {
            std::vector<std::size_t> stack(successors_[source].begin(),
                                           successors_[source].end());
            while (!stack.empty()) {
                const auto next = stack.back();
                stack.pop_back();
                if (reach_[source][next]) continue;
                reach_[source][next] = true;
                stack.insert(stack.end(), successors_[next].begin(),
                             successors_[next].end());
            }
            if (reach_[source][source]) {
                throw std::runtime_error(
                    "pass fusion graph contains a dependency cycle: " +
                    graph_.name + " (" + name(source) + ")");
            }
        }

        // One image per resource: a node that reads a value runs before the
        // node that writes the resource's next value, and writers keep their
        // order. Each version is bound in declaration order, so these edges
        // agree with the data edges; one that would contradict an explicit
        // after/before edge is left out rather than invented.
        std::map<std::string, std::vector<std::uint32_t>, std::less<>>
            versions;
        for (const auto &[value, node] : producer_) {
            (void)node;
            versions[value.resource].push_back(value.version);
        }
        for (auto &[resource, produced] : versions) {
            std::sort(produced.begin(), produced.end());
            std::vector<std::uint32_t> sequence;
            if (readers_.contains(LogicalValueId{resource, 0})) {
                sequence.push_back(0);
            }
            sequence.insert(sequence.end(), produced.begin(),
                            produced.end());
            for (std::size_t index = 1; index < sequence.size(); ++index) {
                const LogicalValueId earlier{resource, sequence[index - 1]};
                const auto writer =
                    producer_.at(LogicalValueId{resource, sequence[index]});
                const auto history_read =
                    earlier.version == 0 &&
                    history_resources_.contains(resource);
                if (const auto sites = readers_.find(earlier);
                    sites != readers_.end() && !history_read) {
                    for (const auto &site : sites->second) {
                        addHazard(site.node, writer);
                    }
                }
                if (const auto previous = producer_.find(earlier);
                    previous != producer_.end()) {
                    addHazard(previous->second, writer);
                }
            }
        }
    }

    void addHazard(std::size_t from, std::size_t to) {
        if (from == to || reach_[from][to] || reach_[to][from]) return;
        successors_[from].insert(to);
        addReach(from, to);
    }

    void orderNodes() {
        std::vector<std::size_t> indegree(count_, 0);
        for (const auto &next : successors_) {
            for (const auto node : next) ++indegree[node];
        }
        const auto key = [&](std::size_t node) {
            return std::tuple{graph_.nodes[node].declaration_index,
                              std::string_view{name(node)}, node};
        };
        std::set<std::tuple<std::size_t, std::string_view, std::size_t>>
            ready;
        for (std::size_t node = 0; node < count_; ++node) {
            if (indegree[node] == 0) ready.insert(key(node));
        }
        while (!ready.empty()) {
            const auto node = std::get<2>(*ready.begin());
            ready.erase(ready.begin());
            order_.push_back(node);
            for (const auto next : successors_[node]) {
                if (--indegree[next] == 0) ready.insert(key(next));
            }
        }
        position_.assign(count_, 0);
        for (std::size_t index = 0; index < order_.size(); ++index) {
            position_[order_[index]] = index;
        }
    }

    void collectCandidates() {
        for (const auto &[value, sites] : readers_) {
            const auto producer = producer_.find(value);
            if (producer == producer_.end()) continue;
            for (const auto &site : sites) {
                if (!fullscreen_[producer->second] &&
                    !fullscreen_[site.node]) {
                    continue;
                }
                candidates_.push_back(Candidate{
                    .producer = producer->second,
                    .consumer = site.node,
                    .use = site.use,
                    .value = value,
                });
            }
        }
        std::sort(candidates_.begin(), candidates_.end(),
                  [&](const Candidate &left, const Candidate &right) {
                      return std::tuple{position_[left.producer],
                                        position_[left.consumer],
                                        left.value} <
                             std::tuple{position_[right.producer],
                                        position_[right.consumer],
                                        right.value};
                  });
        outgoing_.assign(count_, {});
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            outgoing_[candidates_[index].producer].push_back(index);
        }
    }

    const PassFusionResourceFacts *resourceFacts(
        std::string_view resource) const {
        const auto found = resource_facts_.find(resource);
        return found == resource_facts_.end() ? nullptr : found->second;
    }

    std::optional<Finding> shapeFinding(const Candidate &candidate) const {
        const auto &value = candidate.value;
        const auto *value_facts = resourceFacts(value.resource);
        if (value_facts == nullptr) {
            return Finding{PassFusionBoundaryReason::resource_facts_missing,
                           value.resource +
                               " has no extent or format class"};
        }
        for (const auto node : {candidate.producer, candidate.consumer}) {
            for (const auto &output : outputs_[node]) {
                if (resourceFacts(output.resource) == nullptr) {
                    return Finding{
                        PassFusionBoundaryReason::resource_facts_missing,
                        output.resource +
                            " has no extent or format class"};
                }
            }
        }
        for (const auto node : {candidate.producer, candidate.consumer}) {
            for (const auto &output : outputs_[node]) {
                const auto *facts = resourceFacts(output.resource);
                if (facts->extent != value_facts->extent) {
                    return Finding{
                        PassFusionBoundaryReason::extent_mismatch,
                        logicalValueIdName(value) + " is " +
                            extentName(value_facts->extent) + " but " +
                            name(node) + " writes " + output.resource +
                            " at " + extentName(facts->extent)};
                }
            }
        }
        return std::nullopt;
    }

    std::optional<Finding> localFinding(const Candidate &candidate) const {
        const auto producer = candidate.producer;
        const auto consumer = candidate.consumer;
        for (const auto node : {producer, consumer}) {
            if (!fullscreen_[node]) {
                return Finding{
                    PassFusionBoundaryReason::not_fullscreen,
                    name(node) + " is not a fullscreen pass (" +
                        std::string{logicalGraphNodeKindName(
                            graph_.nodes[node].kind)} +
                        " node)"};
            }
        }
        for (const auto node : {producer, consumer}) {
            if (once_[node]) {
                return Finding{PassFusionBoundaryReason::scheduled_once,
                               name(node) + " runs only on the frames it is "
                                            "due (schedule once)"};
            }
        }
        for (const auto node : {producer, consumer}) {
            if (fuse_disabled_[node]) {
                return Finding{PassFusionBoundaryReason::fuse_disabled,
                               "target_planning.graphs." + graph_.name +
                                   ".nodes." + name(node) +
                                   ".fuse is false"};
            }
        }
        const auto &producer_node = graph_.nodes[producer];
        const auto &consumer_node = graph_.nodes[consumer];
        if (producer_node.view_family != consumer_node.view_family ||
            producer_node.view_select != consumer_node.view_select) {
            return Finding{PassFusionBoundaryReason::view_mismatch,
                           name(producer) + " runs on " +
                               viewName(producer_node) + ", " +
                               name(consumer) + " on " +
                               viewName(consumer_node)};
        }
        const auto &use = consumer_node.uses[candidate.use];
        const auto read = name(consumer) + " reads " +
                          logicalValueIdName(candidate.value) + " as " +
                          footprintName(use.footprint);
        switch (use.footprint.kind) {
        case LogicalReadFootprintKind::same_pixel:
            break;
        case LogicalReadFootprintKind::neighborhood:
            return Finding{PassFusionBoundaryReason::read_neighborhood, read};
        case LogicalReadFootprintKind::temporal:
            return Finding{PassFusionBoundaryReason::read_temporal, read};
        case LogicalReadFootprintKind::none:
        case LogicalReadFootprintKind::arbitrary:
            return Finding{PassFusionBoundaryReason::read_arbitrary, read};
        }
        std::vector<std::string> from_producer;
        for (const auto &other : consumer_node.uses) {
            if (!other.input_value) continue;
            const auto found = producer_.find(*other.input_value);
            if (found != producer_.end() && found->second == producer) {
                from_producer.push_back(
                    logicalValueIdName(*other.input_value));
            }
        }
        if (from_producer.size() > 1) {
            std::string values;
            for (const auto &value : from_producer) {
                values += (values.empty() ? "" : ", ") + value;
            }
            return Finding{PassFusionBoundaryReason::multiple_chain_inputs,
                           name(consumer) + " reads " + values + " of " +
                               name(producer)};
        }
        if (use.access == LogicalAccessMode::read_write) {
            for (const auto &site : readers_.at(candidate.value)) {
                if (site.node == consumer) continue;
                return Finding{
                    PassFusionBoundaryReason::in_place_read_outside,
                    name(consumer) + " loads " +
                        logicalValueIdName(candidate.value) +
                        " in place and " + name(site.node) +
                        " reads it too"};
            }
        }
        return shapeFinding(candidate);
    }

    bool eligible(std::size_t node) const {
        return fullscreen_[node] && !once_[node] && !fuse_disabled_[node];
    }

    // The contraction of the committed chains and `tentative` has no cycle.
    bool contractedAcyclic(const std::vector<std::size_t> &tentative) const {
        std::vector<std::size_t> group(count_);
        for (std::size_t node = 0; node < count_; ++node) {
            group[node] = chain_of_[node] >= 0
                              ? count_ + static_cast<std::size_t>(
                                             chain_of_[node])
                              : node;
        }
        const auto tentative_group = count_ + chains_.size();
        for (const auto node : tentative) group[node] = tentative_group;
        std::map<std::size_t, std::set<std::size_t>> edges;
        std::map<std::size_t, std::size_t> indegree;
        for (std::size_t node = 0; node < count_; ++node) {
            indegree.emplace(group[node], 0);
        }
        for (std::size_t node = 0; node < count_; ++node) {
            for (const auto next : successors_[node]) {
                if (group[node] == group[next]) continue;
                if (edges[group[node]].insert(group[next]).second) {
                    ++indegree[group[next]];
                }
            }
        }
        std::vector<std::size_t> ready;
        for (const auto &[id, degree] : indegree) {
            if (degree == 0) ready.push_back(id);
        }
        std::size_t visited = 0;
        while (!ready.empty()) {
            const auto id = ready.back();
            ready.pop_back();
            ++visited;
            if (const auto found = edges.find(id); found != edges.end()) {
                for (const auto next : found->second) {
                    if (--indegree[next] == 0) ready.push_back(next);
                }
            }
        }
        return visited == indegree.size();
    }

    std::optional<Finding> globalFinding(
        const std::vector<std::size_t> &members,
        const std::vector<std::size_t> &links,
        const Candidate &candidate) const {
        const auto consumer = candidate.consumer;
        const auto tail = members.back();
        if (!links.empty()) {
            // One kind of value flows through the chain: the value handed
            // on keeps the class of the one the tail received.
            const auto &received = candidates_[links.back()].value;
            const auto &class_received =
                resourceFacts(received.resource)->format_class;
            const auto &class_handed =
                resourceFacts(candidate.value.resource)->format_class;
            if (class_handed != class_received) {
                return Finding{
                    PassFusionBoundaryReason::format_class_mismatch,
                    logicalValueIdName(candidate.value) + " is " +
                        class_handed + " but the chain hands " +
                        logicalValueIdName(received) + " as " +
                        class_received};
            }
        }
        for (const auto &use : graph_.nodes[consumer].uses) {
            if (!use.input_value || *use.input_value == candidate.value) {
                continue;
            }
            const auto found = producer_.find(*use.input_value);
            if (found == producer_.end() || found->second == tail) continue;
            if (std::find(members.begin(), members.end(), found->second) !=
                members.end()) {
                return Finding{
                    PassFusionBoundaryReason::reads_other_member_output,
                    name(consumer) + " also reads " +
                        logicalValueIdName(*use.input_value) + " of " +
                        name(found->second) + ", an earlier stage"};
            }
        }
        for (const auto member : members) {
            for (const auto outside : order_) {
                if (outside == consumer ||
                    std::find(members.begin(), members.end(), outside) !=
                        members.end()) {
                    continue;
                }
                if (reach_[member][outside] && reach_[outside][consumer]) {
                    return Finding{
                        PassFusionBoundaryReason::outside_dependency,
                        name(outside) + " runs after " + name(member) +
                            " and before " + name(consumer)};
                }
            }
        }
        auto tentative = members;
        tentative.push_back(consumer);
        if (!contractedAcyclic(tentative)) {
            return Finding{PassFusionBoundaryReason::cycle_with_other_chain,
                           "fusing " + name(consumer) + " after " +
                               name(tail) +
                               " closes a dependency cycle with another "
                               "chain"};
        }
        return std::nullopt;
    }

    void decide(Candidate &candidate, Finding finding) {
        candidate.reason = finding.reason;
        candidate.detail = std::move(finding.detail);
    }

    void growChains(PassFusionPlan &result) {
        chain_of_.assign(count_, -1);
        for (const auto head : order_) {
            if (chain_of_[head] >= 0 || !eligible(head)) continue;
            std::vector<std::size_t> members{head};
            std::vector<std::size_t> links;
            while (true) {
                const auto tail = members.back();
                std::optional<std::size_t> chosen;
                for (const auto index : outgoing_[tail]) {
                    auto &candidate = candidates_[index];
                    if (candidate.reason) continue;
                    if (chosen) {
                        decide(candidate,
                               {PassFusionBoundaryReason::fan_out,
                                name(tail) + " continues into " +
                                    name(candidates_[*chosen].consumer)});
                        continue;
                    }
                    if (const auto other = chain_of_[candidate.consumer];
                        other >= 0) {
                        decide(candidate,
                               {PassFusionBoundaryReason::fan_in,
                                name(candidate.consumer) +
                                    " is already a stage of the chain from " +
                                    name(chains_[static_cast<std::size_t>(
                                                     other)]
                                             .front())});
                        continue;
                    }
                    if (auto finding =
                            globalFinding(members, links, candidate)) {
                        decide(candidate, std::move(*finding));
                        continue;
                    }
                    chosen = index;
                }
                if (!chosen) break;
                candidates_[*chosen].linked = true;
                members.push_back(candidates_[*chosen].consumer);
                links.push_back(*chosen);
            }
            if (members.size() < 2) continue;
            const auto id = static_cast<int>(chains_.size());
            for (const auto member : members) chain_of_[member] = id;
            chains_.push_back(members);
            result.chains.push_back(makeChain(members, links));
        }
    }

    bool liveOut(const LogicalValueId &value) const {
        const auto last = last_version_.find(value.resource);
        if (last == last_version_.end() || last->second != value.version) {
            return false;
        }
        if (history_resources_.contains(value.resource)) return true;
        const auto resource = resources_.find(value.resource);
        return resource != resources_.end() &&
               resource->second->materialization !=
                   LogicalMaterializationRequirement::virtual_resource;
    }

    PassFusionChain makeChain(const std::vector<std::size_t> &members,
                              const std::vector<std::size_t> &links) const {
        PassFusionChain chain{
            .graph = graph_.name,
            .stage_count = members.size(),
        };
        for (const auto member : members) chain.members.push_back(name(member));
        for (std::size_t index = 0; index < links.size(); ++index) {
            const auto &link = candidates_[links[index]];
            chain.chain_resources.push_back(PassFusionChainResource{
                .value = link.value,
                .producer = name(link.producer),
                .consumer = name(link.consumer),
            });
            ++chain.eliminated_reads;
            for (const auto &output : outputs_[members[index]]) {
                std::vector<ReadSite> outside;
                if (const auto sites = readers_.find(output);
                    sites != readers_.end()) {
                    for (const auto &site : sites->second) {
                        if (std::find(members.begin(), members.end(),
                                      site.node) == members.end()) {
                            outside.push_back(site);
                        }
                    }
                }
                std::sort(outside.begin(), outside.end(),
                          [&](const ReadSite &left, const ReadSite &right) {
                              return position_[left.node] <
                                     position_[right.node];
                          });
                const auto live = liveOut(output);
                if (output == link.value && outside.empty() && !live) {
                    ++chain.eliminated_writes;
                    continue;
                }
                PassFusionKeptOutput kept{
                    .value = output,
                    .producer = name(members[index]),
                    .live_out = live,
                    .materialized = live,
                };
                for (const auto &site : outside) {
                    const auto &reader = name(site.node);
                    if (std::find(kept.readers.begin(), kept.readers.end(),
                                  reader) == kept.readers.end()) {
                        kept.readers.push_back(reader);
                    }
                    if (graph_.nodes[site.node].uses[site.use].footprint.kind !=
                        LogicalReadFootprintKind::same_pixel) {
                        kept.materialized = true;
                    }
                }
                chain.kept_outputs.push_back(std::move(kept));
            }
        }
        return chain;
    }

    const CompiledLogicalRenderGraph &graph_;
    std::size_t count_ = 0;
    std::map<std::string, std::size_t, std::less<>> index_;
    std::vector<bool> fullscreen_;
    std::vector<bool> once_;
    std::vector<bool> fuse_disabled_;
    std::map<std::string, const LogicalResourceDesc *, std::less<>>
        resources_;
    std::map<std::string, const PassFusionResourceFacts *, std::less<>>
        resource_facts_;
    std::set<std::string, std::less<>> history_resources_;
    std::map<LogicalValueId, std::size_t> producer_;
    std::map<LogicalValueId, std::vector<ReadSite>> readers_;
    std::vector<std::vector<LogicalValueId>> outputs_;
    std::map<std::string, std::uint32_t, std::less<>> last_version_;
    std::vector<std::set<std::size_t>> successors_;
    std::vector<std::vector<bool>> reach_;
    std::vector<std::size_t> order_;
    std::vector<std::size_t> position_;
    std::vector<Candidate> candidates_;
    std::vector<std::vector<std::size_t>> outgoing_;
    std::vector<int> chain_of_;
    std::vector<std::vector<std::size_t>> chains_;
};

nlohmann::ordered_json valueFields(nlohmann::ordered_json object,
                                   const LogicalValueId &value) {
    object["resource"] = value.resource;
    object["version"] = value.version;
    return object;
}

} // namespace

std::string_view passFusionBoundaryReasonName(
    PassFusionBoundaryReason reason) {
    switch (reason) {
    case PassFusionBoundaryReason::not_fullscreen: return "not_fullscreen";
    case PassFusionBoundaryReason::scheduled_once: return "scheduled_once";
    case PassFusionBoundaryReason::fuse_disabled: return "fuse_disabled";
    case PassFusionBoundaryReason::view_mismatch: return "view_mismatch";
    case PassFusionBoundaryReason::read_neighborhood:
        return "read_neighborhood";
    case PassFusionBoundaryReason::read_arbitrary: return "read_arbitrary";
    case PassFusionBoundaryReason::read_temporal: return "read_temporal";
    case PassFusionBoundaryReason::multiple_chain_inputs:
        return "multiple_chain_inputs";
    case PassFusionBoundaryReason::in_place_read_outside:
        return "in_place_read_outside";
    case PassFusionBoundaryReason::resource_facts_missing:
        return "resource_facts_missing";
    case PassFusionBoundaryReason::extent_mismatch: return "extent_mismatch";
    case PassFusionBoundaryReason::format_class_mismatch:
        return "format_class_mismatch";
    case PassFusionBoundaryReason::reads_other_member_output:
        return "reads_other_member_output";
    case PassFusionBoundaryReason::outside_dependency:
        return "outside_dependency";
    case PassFusionBoundaryReason::cycle_with_other_chain:
        return "cycle_with_other_chain";
    case PassFusionBoundaryReason::fan_out: return "fan_out";
    case PassFusionBoundaryReason::fan_in: return "fan_in";
    }
    throw std::runtime_error("unknown pass fusion boundary reason");
}

std::span<const PassFusionBoundaryReason> passFusionBoundaryReasons() {
    return kBoundaryReasons;
}

PassFusionPlan planPassFusion(
    std::span<const CompiledLogicalRenderGraph> graphs,
    std::span<const PassFusionGraphFacts> facts,
    const TargetPlanningPolicy &target_planning) {
    std::map<std::string, const PassFusionGraphFacts *, std::less<>>
        facts_by_graph;
    for (const auto &entry : facts) {
        if (!facts_by_graph.emplace(entry.graph, &entry).second) {
            throw std::runtime_error(
                "pass fusion facts repeat graph: " + entry.graph);
        }
    }
    std::set<std::string, std::less<>> graph_names;
    for (const auto &graph : graphs) {
        if (!graph_names.insert(graph.name).second) {
            throw std::runtime_error(
                "pass fusion graphs repeat a name: " + graph.name);
        }
        if (!facts_by_graph.contains(graph.name)) {
            throw std::runtime_error(
                "pass fusion facts have no entry for graph: " + graph.name);
        }
    }
    for (const auto &[name, entry] : facts_by_graph) {
        (void)entry;
        if (!graph_names.contains(name)) {
            throw std::runtime_error(
                "pass fusion facts name an unknown graph: " + name);
        }
    }

    PassFusionPlan result;
    for (const auto &graph : graphs) {
        const auto constraints = std::find_if(
            target_planning.graphs.begin(), target_planning.graphs.end(),
            [&](const PlanningGraphConstraints &candidate) {
                return candidate.graph == graph.name;
            });
        GraphPlanner planner{
            graph, *facts_by_graph.at(graph.name),
            constraints == target_planning.graphs.end() ? nullptr
                                                        : &*constraints};
        planner.plan(result);
    }
    return result;
}

PassFusionSummary summarizePassFusionPlan(const PassFusionPlan &plan) {
    PassFusionSummary summary;
    summary.chains = plan.chains.size();
    for (const auto &chain : plan.chains) {
        summary.fused_passes += chain.stage_count;
        summary.eliminated_passes += chain.stage_count - 1;
        summary.eliminated_reads += chain.eliminated_reads;
        summary.eliminated_writes += chain.eliminated_writes;
    }
    summary.boundaries = plan.boundaries.size();
    for (const auto reason : kBoundaryReasons) {
        const auto count = static_cast<std::size_t>(std::count_if(
            plan.boundaries.begin(), plan.boundaries.end(),
            [&](const PassFusionBoundary &boundary) {
                return boundary.reason == reason;
            }));
        if (count != 0) summary.boundary_reasons.emplace_back(reason, count);
    }
    return summary;
}

nlohmann::ordered_json passFusionPlanToJson(const PassFusionPlan &plan) {
    nlohmann::ordered_json result{
        {"schema", "pelican.pass_fusion_plan"},
        {"version", 1},
        {"stage", "structural"},
    };
    auto fused_nodes = nlohmann::ordered_json::array();
    for (const auto &chain : plan.chains) {
        auto chain_resources = nlohmann::ordered_json::array();
        for (const auto &resource : chain.chain_resources) {
            chain_resources.push_back(valueFields(
                nlohmann::ordered_json::object(), resource.value));
            chain_resources.back()["producer"] = resource.producer;
            chain_resources.back()["consumer"] = resource.consumer;
        }
        auto kept_outputs = nlohmann::ordered_json::array();
        for (const auto &kept : chain.kept_outputs) {
            auto entry =
                valueFields(nlohmann::ordered_json::object(), kept.value);
            entry["producer"] = kept.producer;
            entry["readers"] = kept.readers;
            entry["live_out"] = kept.live_out;
            entry["materialized"] = kept.materialized;
            kept_outputs.push_back(std::move(entry));
        }
        fused_nodes.push_back(nlohmann::ordered_json{
            {"graph", chain.graph},
            {"members", chain.members},
            {"stage_count", chain.stage_count},
            {"chain_resources", std::move(chain_resources)},
            {"kept_outputs", std::move(kept_outputs)},
            {"eliminated_reads", chain.eliminated_reads},
            {"eliminated_writes", chain.eliminated_writes},
        });
    }
    result["fused_nodes"] = std::move(fused_nodes);
    auto boundaries = nlohmann::ordered_json::array();
    for (const auto &boundary : plan.boundaries) {
        nlohmann::ordered_json entry{
            {"graph", boundary.graph},
            {"producer", boundary.producer},
            {"consumer", boundary.consumer},
        };
        entry = valueFields(std::move(entry), boundary.value);
        entry["reason"] = passFusionBoundaryReasonName(boundary.reason);
        entry["detail"] = boundary.detail;
        boundaries.push_back(std::move(entry));
    }
    result["boundaries"] = std::move(boundaries);
    const auto summary = summarizePassFusionPlan(plan);
    auto reasons = nlohmann::ordered_json::object();
    for (const auto &[reason, count] : summary.boundary_reasons) {
        reasons[std::string{passFusionBoundaryReasonName(reason)}] = count;
    }
    result["summary"] = nlohmann::ordered_json{
        {"chains", summary.chains},
        {"fused_passes", summary.fused_passes},
        {"eliminated_passes", summary.eliminated_passes},
        {"eliminated_reads", summary.eliminated_reads},
        {"eliminated_writes", summary.eliminated_writes},
        {"boundaries", summary.boundaries},
        {"boundary_reasons", std::move(reasons)},
    };
    return result;
}

} // namespace Pelican
