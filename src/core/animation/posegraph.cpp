#include "posegraph.hpp"

#include "../../project/graphpartition.hpp"
#include "../../project/posegraphmodule.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace Pelican::Animation {
namespace {
using Json = nlohmann::json;

const std::map<std::string, PoseGraphOp, std::less<>> &operationNames() {
    static const std::map<std::string, PoseGraphOp, std::less<>> names{
        {"pose.clip_sample", PoseGraphOp::clip_sample},
        {"pose.blend", PoseGraphOp::blend},
        {"pose.state_machine", PoseGraphOp::state_machine},
        {"pose.retarget", PoseGraphOp::retarget},
        {"pose.skeletal", PoseGraphOp::skeletal_sink},
        {"pose.transform", PoseGraphOp::transform_sink},
    };
    return names;
}

std::string requiredName(const Json &parameters, const char *key, const std::string &node) {
    const auto found = parameters.find(key);
    if (found == parameters.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty())
        throw std::invalid_argument(std::string{"pose operation requires a nonempty "} + key + ": " + node);
    return found->get<std::string>();
}

Json ownerJson(const GraphInstanceOwner &owner) {
    return Json{{"scene", owner.scene}, {"object", owner.object}};
}
} // namespace

void PoseGraph::raiseSinkConflict(const GraphInstanceOwner &owner, const std::string &first,
                                  const std::string &second) {
    throw GraphRegistryError("pose.sink_conflict@1",
        {{"owner", ownerJson(owner)}, {"first", first}, {"second", second}});
}

void PoseGraph::raiseLayoutMismatch(const std::string &node, const std::string &port,
                                    nlohmann::json expected, nlohmann::json observed) {
    throw GraphRegistryError("pose.layout_mismatch@1",
        {{"node", node}, {"port", port}, {"expected", std::move(expected)},
         {"observed", std::move(observed)}});
}

PoseGraph::PoseGraph(const Json &authored) : PoseGraph(authored, builtinGraphModuleRegistry()) {}

PoseGraph::PoseGraph(const Json &authored, const GraphModuleRegistry &modules) {
    compile(authored, modules);
}

void PoseGraph::compile(const Json &authored, const GraphModuleRegistry &modules) {
    const auto partition = partitionGraphDocument(authored, modules).animation;
    if (partition.nodes.empty()) return;
    std::map<std::string, std::size_t, std::less<>> index;
    for (const auto &node : partition.nodes) {
        const auto id = node.at("id").get<std::string>();
        const auto op = node.at("op").get<std::string>();
        const auto found = operationNames().find(op);
        if (found == operationNames().end())
            throw GraphRegistryError("graph.operation_unexecutable@1",
                {{"backend", "pelican.pose"}, {"op", op}, {"node", id}});
        PoseGraphNode compiled;
        compiled.id = id;
        compiled.op = found->second;
        const auto *owner = graphInstanceOwnerOf(partition.instance_owners, id);
        // A pose writes to one object's sink, so a pose node outside an owned
        // instance has nothing to write to. Say so instead of evaluating it.
        if (!owner)
            throw std::invalid_argument("pose operation requires an owned graph instance: " + id);
        compiled.owner = *owner;
        const auto parameters = node.value("parameters", Json::object());
        switch (compiled.op) {
        case PoseGraphOp::clip_sample:
            compiled.rig = requiredName(parameters, "rig", id);
            compiled.clip = requiredName(parameters, "clip", id);
            break;
        case PoseGraphOp::state_machine:
            compiled.rig = requiredName(parameters, "rig", id);
            compiled.state_machine = parameters.at("graph");
            compiled.parameter_ports = poseStateMachineParameterPorts(compiled.state_machine, id);
            break;
        case PoseGraphOp::retarget:
            compiled.profile = requiredName(parameters, "profile", id);
            break;
        default:
            break;
        }
        index.emplace(id, nodes_.size());
        nodes_.push_back(std::move(compiled));
    }
    // Inputs resolve after every node exists: an edge may name a node declared
    // later in the expanded document.
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        auto &compiled = nodes_[i];
        for (const auto &[port, endpoint] : partition.nodes[i].at("inputs").items()) {
            const auto source = endpoint.at("node").get<std::string>();
            const auto found = index.find(source);
            if (found == index.end()) {
                // The values partition publishes scalars for this frame. A typed
                // stream has no such publication yet, so say so instead of
                // reading a value that was never produced.
                if (endpoint.at("port") != "value")
                    throw std::invalid_argument(
                        "pose operations read scalar inputs only: " + compiled.id + "." + port);
                const auto output = graphAnimationScalarOutput(compiled.id, port);
                compiled.scalar_inputs.emplace(port, output);
                scalar_outputs_.push_back(output);
                continue;
            }
            // A pose from another object keeps its own layout; the mismatch is
            // named at evaluation, where the layout generations are known.
            compiled.pose_inputs.emplace(port, found->second);
        }
    }
    // Dependency order with cycle detection. The scalar half is already ordered
    // by the value backend; only pose edges can loop here.
    std::vector<std::uint8_t> mark(nodes_.size(), 0);
    const auto visit = [&](auto &&self, std::size_t node) -> void {
        if (mark[node] == 2) return;
        if (mark[node] == 1) throw std::invalid_argument("pose graph has a cycle: " + nodes_[node].id);
        mark[node] = 1;
        for (const auto &[port, source] : nodes_[node].pose_inputs) {
            (void)port;
            self(self, source);
        }
        mark[node] = 2;
        order_.push_back(node);
    };
    for (std::size_t i = 0; i < nodes_.size(); ++i) visit(visit, i);
    // The layout a pose was produced for follows its chain back to the rig that
    // decided it. Mixing two of them is the mismatch the module names, and it
    // is decidable here -- before any object is live.
    for (const auto node : order_) {
        auto &compiled = nodes_[node];
        if (!compiled.rig.empty()) {
            compiled.resolved_rig = compiled.rig;
            continue;
        }
        for (const auto &[port, source] : compiled.pose_inputs) {
            const auto &upstream = nodes_[source].resolved_rig;
            if (compiled.resolved_rig.empty()) {
                compiled.resolved_rig = upstream;
                continue;
            }
            if (compiled.resolved_rig != upstream)
                raiseLayoutMismatch(compiled.id, port, Json{{"rig", compiled.resolved_rig}},
                                    Json{{"rig", upstream}});
        }
    }
    // One writer per object. A second sink on the same owner is the conflict
    // the module promises by name, whether or not the two chains agree.
    std::map<std::pair<std::string, std::uint64_t>, std::string> writers;
    for (const auto node : order_) {
        const auto &compiled = nodes_[node];
        if (compiled.op != PoseGraphOp::skeletal_sink && compiled.op != PoseGraphOp::transform_sink)
            continue;
        const auto key = std::pair{compiled.owner.scene, compiled.owner.object};
        const auto [found, inserted] = writers.emplace(key, compiled.id);
        if (!inserted) raiseSinkConflict(compiled.owner, found->second, compiled.id);
        sinks_.push_back(node);
    }
    // A wrapped evaluator owns the writer for its object, so its pose can only
    // leave through that object's own sink. Anything else is a second writer.
    std::map<std::size_t, std::string> state_machine_consumers;
    for (const auto &compiled : nodes_)
        for (const auto &[port, source] : compiled.pose_inputs) {
            (void)port;
            if (nodes_[source].op != PoseGraphOp::state_machine) continue;
            const bool first = state_machine_consumers.emplace(source, compiled.id).second;
            if (!first || compiled.op != PoseGraphOp::skeletal_sink)
                raiseSinkConflict(nodes_[source].owner, nodes_[source].id, compiled.id);
        }
    for (std::size_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].op == PoseGraphOp::state_machine && !state_machine_consumers.contains(i))
            throw std::invalid_argument("pose.state_machine requires a pose.skeletal sink: " + nodes_[i].id);
    std::sort(scalar_outputs_.begin(), scalar_outputs_.end());
    scalar_outputs_.erase(std::unique(scalar_outputs_.begin(), scalar_outputs_.end()),
                          scalar_outputs_.end());
}

const PoseGraphNode *PoseGraph::stateMachineFor(std::size_t sink) const {
    const auto &node = nodes_.at(sink);
    const auto pose = node.pose_inputs.find("pose");
    if (pose == node.pose_inputs.end()) return nullptr;
    const auto &source = nodes_.at(pose->second);
    return source.op == PoseGraphOp::state_machine ? &source : nullptr;
}

} // namespace Pelican::Animation
