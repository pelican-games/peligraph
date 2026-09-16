#include "graphmodule.hpp"
#include "shaderresourceport.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

namespace Pelican {
namespace {
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

std::string nodeId(const Json &node) {
    const auto found = node.find("id");
    return found == node.end() || !found->is_string() ? std::string{} : found->get<std::string>();
}

LogicalType bufferType(const LogicalTypeRegistry &registry, const Json &declaration) {
    const auto element = declaration.at("element").get<std::string>();
    if (!shaderResourceBufferElementFromName(element))
        throw std::invalid_argument("unknown graph buffer element: " + element);
    if (declaration.contains("domain")) {
        if (declaration.at("domain") == "index" && element != "uint")
            throw std::invalid_argument("index attributes require uint elements");
        return registry.canonicalize(parseSemanticTypeId("pelican.render.attribute@1"),
            {{"element", EnumValueId{element}}, {"domain", EnumValueId{declaration.at("domain").get<std::string>()}}});
    }
    return registry.canonicalize(parseSemanticTypeId("pelican.render.storage_buffer@1"),
                                 {{"element", EnumValueId{element}}});
}

LogicalType resourceType(const LogicalTypeRegistry &registry, const Json &declaration) {
    const auto kind = declaration.value("kind", std::string{"buffer"});
    if (kind == "buffer") return bufferType(registry, declaration);
    if (kind != "image") throw std::invalid_argument("unknown graph resource kind: " + kind);
    if (declaration.contains("domain"))
        throw std::invalid_argument("attribute domains apply to buffer resources, not images");
    const auto &type = declaration.at("type");
    std::vector<LogicalTypeArgumentInput> arguments;
    for (const auto &[name, value] : type.at("arguments").items())
        arguments.push_back({name, EnumValueId{value.get<std::string>()}});
    const auto result = registry.canonicalize(parseSemanticTypeId(type.at("semantic").get<std::string>()),
                                               std::move(arguments));
    if (result.constructor != LogicalTypeConstructor::image)
        throw std::invalid_argument("graph image requires an image logical type");
    return result;
}

// Render resources and passes declare their own ports, so pelican.render
// registers a factory rather than a fixed contract.
GraphOperationContract renderPorts(const LogicalTypeRegistry &registry,
                                   const LogicalType &scalar, const Json &node) {
    const auto op = node.at("op").get<std::string>();
    const auto &parameters = node.at("parameters");
    GraphOperationContract contract;
    if (op == "render.buffer" || op == "render.attribute") {
        if (op == "render.attribute" && !parameters.contains("domain"))
            throw std::invalid_argument("render.attribute requires an attribute domain");
        if (op == "render.buffer" && parameters.contains("domain"))
            throw std::invalid_argument("attribute domain requires render.attribute");
        contract.outputs.emplace("resource", bufferType(registry, parameters));
        contract.inputs.emplace("value", GraphInputContract{exactLogicalTypePattern(registry, scalar), false});
    } else if (op == "render.image") {
        auto declaration = parameters;
        declaration["kind"] = "image";
        contract.outputs.emplace("resource", resourceType(registry, declaration));
    } else if (op == "render.compute" || op == "render.fullscreen" || op == "render.raster") {
        const auto &ports = parameters.at("ports");
        if (!ports.is_object()) throw std::invalid_argument("render ports must be an object");
        for (const auto &[port, declaration] : ports.items()) {
            const auto type = resourceType(registry, declaration);
            contract.inputs.emplace(port, GraphInputContract{exactLogicalTypePattern(registry, type)});
            const auto access = declaration.at("access").get<std::string>();
            if (access != "read" && access != "write" && access != "read_write")
                throw std::invalid_argument("unknown compute port access: " + access);
            if (access != "read") contract.outputs.emplace(port, type);
        }
    } else {
        throw GraphRegistryError("graph.operation_unregistered@1",
                                 {{"node", nodeId(node)}, {"op", op}});
    }
    return contract;
}

OrderedJson schemaJson(const SemanticTypeSchema &schema) {
    OrderedJson result{{"id", semanticTypeIdName(schema.id)},
                       {"constructor", std::string{logicalTypeConstructorName(schema.constructor)}},
                       {"traits", schema.traits},
                       {"parameters", OrderedJson::array()}};
    for (const auto &parameter : schema.parameters) {
        OrderedJson entry{{"name", parameter.name},
                          {"role", std::string{typeArgumentRoleName(parameter.role)}},
                          {"kind", std::string{typeArgumentValueKindName(parameter.value_kind)}}};
        if (parameter.default_value)
            entry["default"] = typeArgumentValueToJson(*parameter.default_value);
        if (!parameter.allowed_enum_values.empty()) {
            auto allowed = OrderedJson::array();
            for (const auto &value : parameter.allowed_enum_values) allowed.push_back(value.value);
            entry["allowed"] = std::move(allowed);
        }
        result["parameters"].push_back(std::move(entry));
    }
    return result;
}

OrderedJson operationJson(const std::string &op, const GraphOperationRegistration &registration,
                          bool sink) {
    const auto &contract = registration.contract;
    OrderedJson change{{"default", std::string{graphParameterChangeName(contract.parameter_change)}}};
    auto parameters = OrderedJson::object();
    for (const auto &[parameter, value] : contract.parameter_changes)
        parameters[parameter] = std::string{graphParameterChangeName(value)};
    change["parameters"] = std::move(parameters);
    OrderedJson result{{"op", op},
                       {"domain", std::string{graphExecutionDomainName(contract.domain)}},
                       {"evaluation", std::string{graphOperationEvaluationName(contract.evaluation)}},
                       {"change", std::move(change)},
                       {"sink", sink}};
    if (registration.factory) {
        // The ports follow the authored parameters; resolving a node yields them.
        result["ports"] = "parameter_dependent";
        return result;
    }
    auto inputs = OrderedJson::array();
    for (const auto &[port, input] : contract.inputs)
        inputs.push_back(OrderedJson{{"port", port}, {"required", input.required},
                                     {"accepted_type", logicalTypePatternToJson(input.accepted_type)}});
    auto outputs = OrderedJson::array();
    for (const auto &[port, type] : contract.outputs)
        outputs.push_back(OrderedJson{{"port", port}, {"type", logicalTypeToJson(type)}});
    result["ports"] = "declared";
    result["inputs"] = std::move(inputs);
    result["outputs"] = std::move(outputs);
    return result;
}
} // namespace

void GraphModuleRegistry::registerModule(GraphModuleManifest manifest) {
    if (manifest.name_space.empty())
        throw GraphRegistryError("graph.module_unnamed@1", {{"version", manifest.version}});
    if (modules_.contains(manifest.name_space))
        throw GraphRegistryError("graph.module_duplicate@1",
            {{"namespace", manifest.name_space}, {"version", manifest.version}});
    for (const auto &[op, registration] : manifest.operations) {
        (void)registration;
        const auto owner = operation_modules_.find(op);
        if (owner != operation_modules_.end())
            throw GraphRegistryError("graph.operation_duplicate@1",
                {{"op", op}, {"first_module", owner->second}, {"second_module", manifest.name_space}});
    }
    std::set<std::string> declared_types;
    for (const auto &schema : manifest.types) {
        const auto name = semanticTypeIdName(schema.id);
        const auto owner = type_modules_.find(name);
        // A manifest listing one type twice is the same mistake as two modules
        // claiming it, and reaches the same name.
        if (owner != type_modules_.end() || !declared_types.insert(name).second)
            throw GraphRegistryError("graph.type_duplicate@1",
                {{"type", name},
                 {"first_module", owner != type_modules_.end() ? owner->second : manifest.name_space},
                 {"second_module", manifest.name_space}});
    }
    for (const auto &sink : manifest.sinks)
        if (!manifest.operations.contains(sink))
            throw GraphRegistryError("graph.sink_unregistered@1",
                {{"module", manifest.name_space}, {"op", sink}});
    // Nothing above changed the registry, so a rejected manifest leaves no
    // half-registered module behind.
    for (const auto &schema : manifest.types) {
        type_modules_.emplace(semanticTypeIdName(schema.id), manifest.name_space);
        types_.registerSchema(schema);
    }
    for (const auto &[op, registration] : manifest.operations) {
        (void)registration;
        operation_modules_.emplace(op, manifest.name_space);
    }
    modules_.emplace(manifest.name_space, std::move(manifest));
}

bool GraphModuleRegistry::contains(std::string_view operation) const {
    return operation_modules_.contains(operation);
}

bool GraphModuleRegistry::isSink(std::string_view operation) const {
    const auto owner = operation_modules_.find(operation);
    if (owner == operation_modules_.end()) return false;
    const auto &sinks = modules_.at(owner->second).sinks;
    return std::find(sinks.begin(), sinks.end(), operation) != sinks.end();
}

GraphOperationContract GraphModuleRegistry::resolveOperation(const nlohmann::json &node) const {
    const auto op = node.at("op").get<std::string>();
    const auto owner = operation_modules_.find(op);
    if (owner == operation_modules_.end())
        throw GraphRegistryError("graph.operation_unregistered@1",
                                 {{"node", nodeId(node)}, {"op", op}});
    const auto &registration = modules_.at(owner->second).operations.at(op);
    auto contract = registration.factory ? registration.factory(node) : registration.contract;
    if (registration.factory) {
        // The module declares the classification once; a factory supplies ports.
        contract.domain = registration.contract.domain;
        contract.evaluation = registration.contract.evaluation;
        contract.parameter_change = registration.contract.parameter_change;
        contract.parameter_changes = registration.contract.parameter_changes;
    }
    for (const auto &[port, type] : contract.outputs) {
        (void)port;
        if (!types_.contains(type.semantic))
            throw GraphRegistryError("graph.type_unregistered@1",
                {{"node", nodeId(node)}, {"type", semanticTypeIdName(type.semantic)}});
    }
    return contract;
}

nlohmann::ordered_json GraphModuleRegistry::catalog() const {
    OrderedJson result{{"schema", "pelican.graph_catalog"}, {"version", 1},
                       {"modules", OrderedJson::array()}};
    for (const auto &[name_space, manifest] : modules_) {
        OrderedJson module{{"namespace", name_space}, {"version", manifest.version},
                           {"description", manifest.description},
                           {"types", OrderedJson::array()},
                           {"operations", OrderedJson::array()},
                           {"sinks", OrderedJson::array()}};
        std::vector<const SemanticTypeSchema *> types;
        types.reserve(manifest.types.size());
        for (const auto &schema : manifest.types) types.push_back(&schema);
        std::sort(types.begin(), types.end(), [](const auto *left, const auto *right) {
            return semanticTypeIdName(left->id) < semanticTypeIdName(right->id);
        });
        for (const auto *schema : types) module["types"].push_back(schemaJson(*schema));
        for (const auto &[op, registration] : manifest.operations)
            module["operations"].push_back(operationJson(op, registration, isSink(op)));
        auto sinks = manifest.sinks;
        std::sort(sinks.begin(), sinks.end());
        for (const auto &sink : sinks) module["sinks"].push_back(sink);
        result["modules"].push_back(std::move(module));
    }
    return result;
}

GraphModuleRegistry makeBuiltinGraphModuleRegistry() {
    // The contracts own canonical types, so the factories keep the registry they
    // canonicalized against alive.
    const auto types = std::make_shared<const LogicalTypeRegistry>(makeBuiltinLogicalTypeRegistry());
    const auto scalar = types->canonicalize(parseSemanticTypeId("pelican.value.scalar@1"));
    const auto event = types->canonicalize(parseSemanticTypeId("pelican.event.playback_control@1"));
    const auto voice = types->canonicalize(parseSemanticTypeId("pelican.audio.voice@1"));
    const GraphInputContract required{exactLogicalTypePattern(*types, scalar)};
    const GraphInputContract optional{required.accepted_type, false};
    const GraphInputContract events{exactLogicalTypePattern(*types, event)};
    const GraphInputContract optional_voice{exactLogicalTypePattern(*types, voice), false};
    const std::map<std::string, LogicalType> value_output{{"value", scalar}};
    const auto declared = [](std::map<std::string, GraphInputContract> inputs,
                             std::map<std::string, LogicalType> outputs,
                             GraphExecutionDomain domain, GraphOperationEvaluation evaluation,
                             GraphParameterChange change) {
        GraphOperationContract contract;
        contract.inputs = std::move(inputs);
        contract.outputs = std::move(outputs);
        contract.domain = domain;
        contract.evaluation = evaluation;
        contract.parameter_change = change;
        return GraphOperationRegistration{std::move(contract), {}};
    };
    const auto value = [&](std::map<std::string, GraphInputContract> inputs,
                           std::map<std::string, LogicalType> outputs,
                           GraphOperationEvaluation evaluation = GraphOperationEvaluation::direct) {
        return declared(std::move(inputs), std::move(outputs), GraphExecutionDomain::values,
                        evaluation, GraphParameterChange::value);
    };

    GraphModuleRegistry registry;
    registry.registerModule(GraphModuleManifest{
        "pelican.value", 1,
        "Scalar values, curves, clocks and typed playback events.",
        builtinValueTypeSchemas(),
        {
            {"value.input", value({}, value_output)},
            {"value.constant", value({}, value_output)},
            {"value.parameter", value({}, value_output)},
            {"value.morph_weight", value({}, value_output)},
            {"value.add", value({{"a", required}, {"b", required}}, value_output)},
            {"value.multiply", value({{"a", required}, {"b", required}}, value_output)},
            {"value.curve", value({{"time", required}}, value_output)},
            {"value.playback", value({{"elapsed", required}, {"source_begin", optional},
                {"source_end", optional}, {"duration", optional}, {"speed", optional},
                {"delay", optional}, {"sample_interval", optional}}, value_output)},
            {"event.playback", value({}, {{"events", event}})},
            // Clocks carry evaluation state between ticks.
            {"value.trigger_clock", value({{"time", required}, {"trigger", required},
                {"paused", optional}, {"reset", optional}}, value_output,
                GraphOperationEvaluation::replay)},
            {"value.event_clock", value({{"time", required}, {"events", events}}, value_output,
                GraphOperationEvaluation::replay)},
        },
        {}});
    registry.registerModule(GraphModuleManifest{
        "pelican.audio", 1,
        "Sound playback voices and the value sinks that drive them.",
        builtinAudioTypeSchemas(),
        {
            {"audio.playback", value({{"time", required}, {"events", events}}, {{"voice", voice}},
                GraphOperationEvaluation::replay)},
            {"audio.volume", value({{"value", required}, {"voice", optional_voice}}, {})},
            {"audio.pitch", value({{"value", required}, {"voice", optional_voice}}, {})},
        },
        {"audio.volume", "audio.pitch"}});
    const auto render = [&](const char *op) {
        auto registration = declared({}, {}, GraphExecutionDomain::rendering,
            GraphOperationEvaluation::direct, GraphParameterChange::rebuild);
        registration.factory = [types, scalar](const nlohmann::json &node) {
            return renderPorts(*types, scalar, node);
        };
        return std::pair<const std::string, GraphOperationRegistration>{op, std::move(registration)};
    };
    registry.registerModule(GraphModuleManifest{
        "pelican.render", 1,
        "GPU buffers, attributes, images, passes and the scalar publication sink.",
        builtinRenderTypeSchemas(),
        {
            // The publication sink runs where the value it uploads is evaluated.
            {"render.scalar", declared({{"value", required}}, {}, GraphExecutionDomain::values,
                GraphOperationEvaluation::direct, GraphParameterChange::rebuild)},
            render("render.buffer"), render("render.attribute"), render("render.image"),
            render("render.compute"), render("render.fullscreen"), render("render.raster"),
        },
        {"render.scalar"}});
    return registry;
}

const GraphModuleRegistry &builtinGraphModuleRegistry() {
    static const GraphModuleRegistry registry = makeBuiltinGraphModuleRegistry();
    return registry;
}

} // namespace Pelican
