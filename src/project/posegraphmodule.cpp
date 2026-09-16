#include "posegraphmodule.hpp"

#include <memory>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {
using Json = nlohmann::json;

std::string requiredName(const Json &parameters, const char *key, const std::string &node) {
    const auto found = parameters.find(key);
    if (found == parameters.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty())
        throw std::invalid_argument(std::string{"pose operation requires a nonempty "} + key + ": " + node);
    return found->get<std::string>();
}

std::string nodeId(const Json &node) {
    const auto found = node.find("id");
    return found == node.end() || !found->is_string() ? std::string{} : found->get<std::string>();
}
} // namespace

std::vector<SemanticTypeSchema> builtinPoseTypeSchemas() {
    // Opaque: the graph moves a staged pose between operations and never reads
    // its joints. The layout generation travels with the handle at evaluation.
    return {SemanticTypeSchema{parseSemanticTypeId("pelican.pose@1"),
                               LogicalTypeConstructor::value, {}, {"opaque", "pose"}},
            SemanticTypeSchema{parseSemanticTypeId("pelican.rig@1"),
                               LogicalTypeConstructor::value, {}, {"opaque", "rig"}}};
}

std::vector<std::string> poseStateMachineParameterPorts(const nlohmann::json &document,
                                                        const std::string &node) {
    if (!document.is_object() || document.value("schema", std::string{}) != "pelican.anim_graph" ||
        document.value("version", 0) != 1)
        throw std::invalid_argument("pose.state_machine requires a pelican.anim_graph v1 document: " + node);
    // Only the declared parameter names are read here; the strict v1 parse (and
    // the v2-reserved key rejection) stays in the animation backend, so this
    // module cannot become a second grammar. pelican.anim_graph declares them
    // as one flat object, so the port set follows in name order.
    std::vector<std::string> ports;
    const auto parameters = document.value("parameters", nlohmann::json::object());
    if (!parameters.is_object())
        throw std::invalid_argument("pose.state_machine parameters must be a flat object: " + node);
    for (const auto &[name, value] : parameters.items()) {
        (void)value;
        if (name.empty() || name.find('.') != std::string::npos)
            throw std::invalid_argument("pose.state_machine parameter name is unusable: " + node);
        if (name == "time" || name == "events")
            throw std::invalid_argument("pose.state_machine parameter shadows a reserved port: " + node);
        ports.push_back(name);
    }
    return ports;
}

void registerPoseGraphModule(GraphModuleRegistry &registry) {
    // The contracts own canonical types, so the factory keeps the registry it
    // canonicalized against alive.
    const auto types = std::make_shared<LogicalTypeRegistry>();
    for (const auto &schema : builtinPoseTypeSchemas()) types->registerSchema(schema);
    auto scalars = makeBuiltinLogicalTypeRegistry();
    const auto scalar = scalars.canonicalize(parseSemanticTypeId("pelican.value.scalar@1"));
    const auto event = scalars.canonicalize(parseSemanticTypeId("pelican.event.playback_control@1"));
    const auto pose = types->canonicalize(parseSemanticTypeId("pelican.pose@1"));
    const GraphInputContract time{exactLogicalTypePattern(scalars, scalar)};
    const GraphInputContract weight{time.accepted_type};
    const GraphInputContract events{exactLogicalTypePattern(scalars, event), false};
    const GraphInputContract pose_input{exactLogicalTypePattern(*types, pose)};
    const std::map<std::string, LogicalType> pose_output{{"pose", pose}};

    const auto animation = [](std::map<std::string, GraphInputContract> inputs,
                              std::map<std::string, LogicalType> outputs,
                              GraphOperationEvaluation evaluation, GraphParameterChange change) {
        GraphOperationContract contract;
        contract.inputs = std::move(inputs);
        contract.outputs = std::move(outputs);
        contract.domain = GraphExecutionDomain::animation;
        contract.evaluation = evaluation;
        contract.parameter_change = change;
        return GraphOperationRegistration{std::move(contract), {}};
    };

    // A state machine's parameter ports follow the document it wraps, so this
    // one operation resolves its ports per node. The declared classification
    // and the two fixed outputs stay authoritative.
    auto state_machine = animation({}, {{"pose", pose}, {"state_events", event}},
        GraphOperationEvaluation::replay, GraphParameterChange::rebuild);
    state_machine.factory = [types, time, events, pose, event](const Json &node) {
        const auto id = nodeId(node);
        const auto &parameters = node.at("parameters");
        if (!parameters.is_object())
            throw std::invalid_argument("pose.state_machine parameters must be an object: " + id);
        (void)requiredName(parameters, "rig", id);
        const auto graph = parameters.find("graph");
        if (graph == parameters.end())
            throw std::invalid_argument("pose.state_machine requires an anim_graph document: " + id);
        GraphOperationContract contract;
        contract.inputs.emplace("time", time);
        contract.inputs.emplace("events", events);
        for (const auto &name : poseStateMachineParameterPorts(*graph, id))
            contract.inputs.emplace(name, GraphInputContract{time.accepted_type, false});
        contract.outputs.emplace("pose", pose);
        contract.outputs.emplace("state_events", event);
        return contract;
    };

    registry.registerModule(GraphModuleManifest{
        "pelican.pose", 1,
        "Skeletal poses on graph ports: clip sampling, blending, state machines and the "
        "single-writer sinks that hand one to an object.",
        builtinPoseTypeSchemas(),
        {
            {"pose.clip_sample", animation({{"time", time}}, pose_output,
                GraphOperationEvaluation::direct, GraphParameterChange::rebuild)},
            {"pose.blend", animation({{"a", pose_input}, {"b", pose_input}, {"weight", weight}},
                pose_output, GraphOperationEvaluation::direct, GraphParameterChange::value)},
            {"pose.state_machine", std::move(state_machine)},
            {"pose.retarget", animation({{"pose", pose_input}}, pose_output,
                GraphOperationEvaluation::direct, GraphParameterChange::rebuild)},
            // One sink per owner. A second writer on the same object is
            // pose.sink_conflict@1 at attach time, never a silent overwrite.
            {"pose.skeletal", animation({{"pose", pose_input}}, {},
                GraphOperationEvaluation::direct, GraphParameterChange::rebuild)},
            {"pose.transform", animation({{"pose", pose_input}}, {},
                GraphOperationEvaluation::direct, GraphParameterChange::rebuild)},
        },
        {"pose.skeletal", "pose.transform"}});
}

} // namespace Pelican
