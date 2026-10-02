#include "canvasmodule.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {
using Json = nlohmann::json;

std::string nodeId(const Json &node) {
    const auto found = node.find("id");
    return found == node.end() || !found->is_string() ? std::string{} : found->get<std::string>();
}
} // namespace

std::vector<SemanticTypeSchema> builtinCanvasTypeSchemas() {
    // Symbolic: the lowering removes every edge of these types, so nothing
    // ever evaluates one.
    return {SemanticTypeSchema{parseSemanticTypeId("pelican.canvas.geometry@1"),
                               LogicalTypeConstructor::value, {}, {"symbolic", "geometry"}},
            SemanticTypeSchema{parseSemanticTypeId("pelican.canvas.layer@1"),
                               LogicalTypeConstructor::value, {}, {"symbolic", "layer"}}};
}

const std::vector<std::string> &canvasGraphOperations() {
    static const std::vector<std::string> operations{"canvas.output", "canvas.stack", "shape.ellipse",
                                                     "shape.fill", "shape.rect", "shape.stroke"};
    return operations;
}

bool isCanvasGraphOperation(std::string_view op) {
    const auto &operations = canvasGraphOperations();
    return std::find(operations.begin(), operations.end(), op) != operations.end();
}

std::string canvasStackOpacityPort(const std::string &input) { return input + ".opacity"; }

void registerCanvasGraphModule(GraphModuleRegistry &registry) {
    // The contracts own canonical types, so the factory keeps the registry it
    // canonicalized against alive.
    const auto types = std::make_shared<LogicalTypeRegistry>(makeBuiltinLogicalTypeRegistry());
    for (const auto &schema : builtinCanvasTypeSchemas()) types->registerSchema(schema);
    const auto scalar = types->canonicalize(parseSemanticTypeId("pelican.value.scalar@1"));
    const auto geometry = types->canonicalize(parseSemanticTypeId("pelican.canvas.geometry@1"));
    const auto layer = types->canonicalize(parseSemanticTypeId("pelican.canvas.layer@1"));
    const auto image = sceneLinearHdrV1(*types);
    const GraphInputContract value{exactLogicalTypePattern(*types, scalar), false};
    const GraphInputContract geometry_input{exactLogicalTypePattern(*types, geometry)};
    const GraphInputContract layer_input{exactLogicalTypePattern(*types, layer)};

    const auto declared = [](std::map<std::string, GraphInputContract> inputs,
                             std::map<std::string, LogicalType> outputs, GraphParameterChange change) {
        GraphOperationContract contract;
        contract.inputs = std::move(inputs);
        contract.outputs = std::move(outputs);
        contract.domain = GraphExecutionDomain::rendering;
        contract.evaluation = GraphOperationEvaluation::direct;
        contract.parameter_change = change;
        return GraphOperationRegistration{std::move(contract), {}};
    };
    const auto ports = [&](std::initializer_list<const char *> names) {
        std::map<std::string, GraphInputContract> inputs;
        for (const auto *name : names) inputs.emplace(name, value);
        return inputs;
    };

    auto rect = declared(ports({"size.x", "size.y", "center.x", "center.y", "corner_radius", "rotation"}),
                         {{"geometry", geometry}}, GraphParameterChange::value);
    auto ellipse = declared(ports({"size.x", "size.y", "center.x", "center.y", "rotation"}),
                            {{"geometry", geometry}}, GraphParameterChange::value);
    auto fill_inputs = ports({"color.r", "color.g", "color.b", "color.a"});
    fill_inputs.emplace("geometry", geometry_input);
    auto fill = declared(fill_inputs, {{"layer", layer}}, GraphParameterChange::value);
    auto stroke_inputs = ports({"color.r", "color.g", "color.b", "color.a", "width"});
    stroke_inputs.emplace("geometry", geometry_input);
    auto stroke = declared(stroke_inputs, {{"layer", layer}}, GraphParameterChange::value);
    stroke.contract.parameter_changes = {{"align", GraphParameterChange::rebuild}};

    // A stack's layer ports follow `parameters.layers`: each layer's `input`
    // is a layer port, and `<input>.opacity` the scalar that may drive its
    // opacity. The declared classification stays authoritative.
    auto stack = declared({}, {}, GraphParameterChange::rebuild);
    stack.contract.parameter_changes = {{"opacity", GraphParameterChange::value}};
    stack.factory = [layer_input, value, layer](const Json &node) {
        const auto id = nodeId(node);
        const auto parameters = node.find("parameters");
        if (parameters == node.end() || !parameters->is_object())
            throw std::invalid_argument("canvas.stack parameters must be an object: " + id);
        const auto layers = parameters->find("layers");
        if (layers == parameters->end() || !layers->is_array())
            throw std::invalid_argument("canvas.stack requires a layers array: " + id);
        GraphOperationContract contract;
        for (const auto &entry : *layers) {
            const auto input = entry.is_object() ? entry.find("input") : entry.end();
            if (!entry.is_object() || input == entry.end() || !input->is_string() ||
                input->get_ref<const std::string &>().empty())
                throw std::invalid_argument("canvas.stack layer requires an input name: " + id);
            const auto name = input->get<std::string>();
            if (name.find('.') != std::string::npos)
                throw std::invalid_argument("canvas.stack layer input name holds a dot: " + id + "." + name);
            if (!contract.inputs.emplace(name, layer_input).second)
                throw std::invalid_argument("canvas.stack names a layer input twice: " + id + "." + name);
            contract.inputs.emplace(canvasStackOpacityPort(name), value);
        }
        contract.outputs.emplace("layer", layer);
        return contract;
    };

    auto output = declared({{"layer", layer_input}}, {{"image", image}}, GraphParameterChange::rebuild);

    registry.registerModule(GraphModuleManifest{
        "pelican.canvas", 1,
        "2D shapes, paints and layer stacks, lowered to render nodes and a generated shader.",
        builtinCanvasTypeSchemas(),
        {
            {"shape.rect", std::move(rect)},
            {"shape.ellipse", std::move(ellipse)},
            {"shape.fill", std::move(fill)},
            {"shape.stroke", std::move(stroke)},
            {"canvas.stack", std::move(stack)},
            {"canvas.output", std::move(output)},
        },
        {}});
}

} // namespace Pelican
