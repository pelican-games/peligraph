#include "graphmodule.hpp"
#include "featurecompose.hpp"
#include "materialoutput.hpp"
#include "renderpipeline.hpp"
#include "shaderresourceport.hpp"
#if PELICAN_WITH_ANIMATION
#include "posegraphmodule.hpp"
#endif

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

// LP-0e: render.shadow_depth and render.snapshot_copy are the rendering
// section's pass types of the same names. Their images are fixed sockets --
// shadow_depth's `depth` attachment, snapshot_copy's `source` and
// `destination` -- that take any image: a shadow map is a depth array, and a
// copy's two images may differ in topology (cube capture copies a cube into a
// 2D array). The pass checks formats, usage and extents when it is compiled,
// as it does for the JSON form. Nothing comes out: whoever reads what the pass
// wrote refers to the target (render.target_ref), as for render.material_pass.
std::vector<std::string> renderPassKindSockets(const std::string &op) {
    if (op == "render.shadow_depth") return {"depth"};
    return {"source", "destination"};
}

// The parameters are the pass's own fields, less the ones the sockets and the
// id decide (name, type, output, source, destination).
GraphOperationContract renderPassKindPorts(const Json &node) {
    const auto op = node.at("op").get<std::string>();
    const auto id = nodeId(node);
    const auto parameters = node.find("parameters");
    if (parameters == node.end() || !parameters->is_object())
        throw std::invalid_argument("render node parameters must be an object");
    static const std::set<std::string, std::less<>> common{
        "view_family", "view_select", "region_tags", "pass_set", "after", "before", "insert", "canonical_anchor"};
    static const std::set<std::string, std::less<>> shadow_depth{
        "shader", "ray_query", "depth_load_op", "depth_store_op", "resolution_domain"};
    for (const auto &[key, value] : parameters->items()) {
        (void)value;
        const bool own = op == "render.shadow_depth" ? shadow_depth.contains(key) : key == "snapshot_after";
        if (!own && !common.contains(key)) throw std::invalid_argument(op + " does not take " + key + ": " + id);
    }
    if (op == "render.snapshot_copy") {
        // The frame planner orders the copy after this pass, as it does for
        // the JSON form, which requires it too.
        const auto after = parameters->find("snapshot_after");
        if (after == parameters->end() || !after->is_string() || after->get_ref<const std::string &>().empty())
            throw std::invalid_argument("render.snapshot_copy requires snapshot_after, the pass it copies after: " +
                                        id);
    }
    // Where the pass lands, as for render.fullscreen / render.raster (LP-0b).
    if (parameters->contains("insert") && parameters->contains("canonical_anchor"))
        throw std::invalid_argument("render insert and canonical_anchor exclude each other: " + id);
    if (const auto insert = parameters->find("insert"); insert != parameters->end()) {
        const auto side = insert->is_object() && insert->size() == 1 ? insert->begin() : insert->end();
        if (side == insert->end() || (side.key() != "after" && side.key() != "before") || !side->is_string() ||
            side->get_ref<const std::string &>().empty())
            throw std::invalid_argument("render insert must be {\"after\" | \"before\": \"<pass>\"}: " + id);
    }
    if (const auto anchor = parameters->find("canonical_anchor"); anchor != parameters->end()) {
        if (!anchor->is_string()) throw std::invalid_argument("render canonical_anchor must be a string: " + id);
        if (!isCanonicalRenderPassAnchor(anchor->get<std::string>()))
            throw GraphRegistryError("graph.insert_anchor_unknown@1",
                                     {{"node", id}, {"anchor", anchor->get<std::string>()}});
    }
    GraphOperationContract contract;
    LogicalTypePattern image;
    image.constructor = LogicalTypeConstructor::image;
    for (const auto &socket : renderPassKindSockets(op)) contract.inputs.emplace(socket, GraphInputContract{image});
    return contract;
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

// LP-0b: where a graph pass lands among the preset's and the features' passes.
// Which pass an insert names is known only after composition; the shape is
// checked here.
void checkRenderPlacement(const std::string &op, const Json &parameters, const Json &node) {
    const bool placed = parameters.contains("insert") || parameters.contains("canonical_anchor");
    // A compute task is always declared after every pass; it has no place
    // among them to take.
    if (placed && op == "render.compute")
        throw GraphRegistryError("graph.insert_on_compute@1", {{"node", nodeId(node)}});
    if (parameters.contains("insert") && parameters.contains("canonical_anchor"))
        throw std::invalid_argument("render insert and canonical_anchor exclude each other: " + nodeId(node));
    if (const auto insert = parameters.find("insert"); insert != parameters.end()) {
        // LP-3b: `declared: true` beside the side places the pass among the
        // declared passes, before any feature (RenderGraphPassRun::declared).
        const auto declared = insert->is_object() ? insert->find("declared") : insert->end();
        const bool has_declared = declared != insert->end();
        if (has_declared && !declared->is_boolean())
            throw std::invalid_argument("render insert declared must be a boolean: " + nodeId(node));
        auto side = insert->end();
        if (insert->is_object() && insert->size() == (has_declared ? 2u : 1u))
            for (auto entry = insert->begin(); entry != insert->end(); ++entry)
                if (entry.key() != "declared") side = entry;
        if (side == insert->end() || (side.key() != "after" && side.key() != "before") ||
            !side->is_string() || side->get_ref<const std::string &>().empty())
            throw std::invalid_argument(
                "render insert must be {\"after\" | \"before\": \"<pass>\", \"declared\"?: true}: " +
                nodeId(node));
    }
    if (const auto anchor = parameters.find("canonical_anchor"); anchor != parameters.end()) {
        if (!anchor->is_string())
            throw std::invalid_argument("render canonical_anchor must be a string: " + nodeId(node));
        if (!isCanonicalRenderPassAnchor(anchor->get<std::string>()))
            throw GraphRegistryError("graph.insert_anchor_unknown@1",
                                     {{"node", nodeId(node)}, {"anchor", anchor->get<std::string>()}});
    }
}

// LP-3a: one draw.* node's parameters (a step of a draw_loop body).
void checkDrawStepParameters(const std::string &op, const Json &parameters, const Json &node) {
    const auto id = nodeId(node);
    if (!parameters.is_object()) throw std::invalid_argument("draw node parameters must be an object: " + id);
    const auto only = [&](std::initializer_list<const char *> keys) {
        for (const auto &[key, value] : parameters.items()) {
            (void)value;
            if (std::none_of(keys.begin(), keys.end(), [&](const char *known) { return key == known; }))
                throw std::invalid_argument(op + " does not take " + key + ": " + id);
        }
    };
    if (op == "draw.material") {
        only({});
        return;
    }
    if (op == "draw.variant") {
        only({"variant"});
        const auto variant = parameters.find("variant");
        if (variant == parameters.end() || !variant->is_string() || variant->get_ref<const std::string &>().empty())
            throw std::invalid_argument("draw.variant requires a variant name: " + id);
        return;
    }
    only({"render_state", "defines"});
    bool any = false;
    if (const auto state = parameters.find("render_state"); state != parameters.end()) {
        if (!state->is_object()) throw std::invalid_argument("draw.override render_state must be an object: " + id);
        const auto oneOf = [&](const std::string &key, std::initializer_list<const char *> names) {
            const auto &value = state->at(key);
            if (!value.is_string() || std::none_of(names.begin(), names.end(),
                                                    [&](const char *name) { return value == name; }))
                throw std::invalid_argument("draw.override render_state " + key + " is not known: " + id);
        };
        for (const auto &[key, value] : state->items()) {
            if (key == "blend") oneOf(key, {"opaque", "blend", "additive"});
            else if (key == "cull") oneOf(key, {"none", "front", "back"});
            else if (key == "depth_compare")
                oneOf(key, {"never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal", "always"});
            else if (key == "depth_test" || key == "depth_write") {
                if (!value.is_boolean())
                    throw std::invalid_argument("draw.override render_state " + key + " must be a boolean: " + id);
            } else throw std::invalid_argument("draw.override render_state does not take " + key + ": " + id);
            any = true;
        }
        if (state->value("depth_test", true) == false && state->value("depth_write", false) == true)
            throw std::invalid_argument("draw.override writes depth with depth_test off: " + id);
    }
    if (const auto defines = parameters.find("defines"); defines != parameters.end()) {
        if (!defines->is_array()) throw std::invalid_argument("draw.override defines must be an array: " + id);
        for (const auto &define : *defines) {
            if (!define.is_string() || define.get_ref<const std::string &>().empty())
                throw std::invalid_argument("draw.override defines are nonempty strings: " + id);
            any = true;
        }
    }
    if (!any) throw std::invalid_argument("draw.override replaces a render state or defines: " + id);
}

// Render resources and passes declare their own ports, so pelican.render
// registers a factory rather than a fixed contract.
GraphOperationContract renderPorts(const LogicalTypeRegistry &registry,
                                   const LogicalType &scalar, const Json &node) {
    const auto op = node.at("op").get<std::string>();
    // LP-3a: a draw.material has nothing to say, so its parameters may be absent.
    static const Json no_parameters = Json::object();
    const auto found_parameters = node.find("parameters");
    const auto &parameters = found_parameters != node.end() ? *found_parameters : no_parameters;
    GraphOperationContract contract;
    const auto draw_stream = registry.canonicalize(parseSemanticTypeId("pelican.render.draw_stream@1"));
    if (op == "draw.material" || op == "draw.variant" || op == "draw.override") {
        checkDrawStepParameters(op, parameters, node);
        // Each step follows the one on its `after` edge; the body's `draws`
        // output is the last.
        contract.inputs.emplace("after", GraphInputContract{exactLogicalTypePattern(registry, draw_stream), false});
        contract.outputs.emplace("draws", draw_stream);
        return contract;
    }
    if (op == "render.material_pass") {
        const auto id = nodeId(node);
        if (!parameters.is_object()) throw std::invalid_argument("render node parameters must be an object");
        for (const auto &[key, value] : parameters.items()) {
            (void)value;
            if (!isRenderMaterialPassParameter(key))
                throw std::invalid_argument("render.material_pass does not take " + key + ": " + id);
        }
        const auto material_contract = parameters.find("material_contract");
        if (material_contract == parameters.end() || !material_contract->is_string() ||
            !materialPassContractFromName(material_contract->get<std::string>()))
            throw std::invalid_argument("render.material_pass requires a known material_contract: " + id);
        checkRenderPlacement(op, parameters, node);
        // An attachment takes any image; the pass checks formats and counts
        // against the contract when it is compiled, as for the JSON form.
        LogicalTypePattern image;
        image.constructor = LogicalTypeConstructor::image;
        for (const auto &port : renderMaterialPassColorPorts(parameters)) {
            if (port == "depth" || port == "draws" || !contract.inputs.emplace(port, GraphInputContract{image}).second)
                throw std::invalid_argument("render.material_pass color socket is named twice: " + port);
        }
        contract.inputs.emplace("depth", GraphInputContract{image});
        // Unconnected: today's drawing (one draw.material per material, or
        // the pass's material_variant).
        contract.inputs.emplace("draws", GraphInputContract{exactLogicalTypePattern(registry, draw_stream), false});
        return contract;
    }
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
    } else if (op == "render.target_ref") {
        // LP-0a: a target declared outside the graph. Only its shape is known
        // here; whether the rendering section, the preset or a feature declares
        // it is decided after composition (graph.target_ref_unknown@1), so an
        // edit may name a target a feature added later will supply.
        if (!parameters.is_object()) throw std::invalid_argument("render node parameters must be an object");
        for (const auto &[key, value] : parameters.items()) {
            (void)value;
            if (key != "name" && key != "type")
                throw std::invalid_argument("render.target_ref accepts only name and type: " + key);
        }
        const auto name = parameters.find("name");
        if (name == parameters.end() || !name->is_string() || name->get_ref<const std::string &>().empty())
            throw std::invalid_argument("render.target_ref requires a target name: " + nodeId(node));
        // The previous frame is read through a port's `history`, never by name.
        if (name->get_ref<const std::string &>().find('@') != std::string::npos)
            throw std::invalid_argument("render.target_ref name is a target name; read its history with a "
                                        "port's history: " + nodeId(node));
        if (!parameters.contains("type"))
            throw std::invalid_argument("render.target_ref requires an image type: " + nodeId(node));
        auto declaration = parameters;
        declaration.erase("name");
        declaration["kind"] = "image";
        contract.outputs.emplace("resource", resourceType(registry, declaration));
    } else if (op == "render.shadow_depth" || op == "render.snapshot_copy") {
        contract = renderPassKindPorts(node);
    } else if (op == "render.compute" || op == "render.fullscreen" || op == "render.raster") {
        const auto &ports = parameters.at("ports");
        if (!ports.is_object()) throw std::invalid_argument("render ports must be an object");
        // LP-1d: ports read as the pass's numbered inputs, by input_index.
        std::map<std::int64_t, std::vector<std::string>> input_indices;
        for (const auto &[port, declaration] : ports.items()) {
            if (declaration.is_object() && declaration.value("binding", Json{}) == "input") {
                if (op == "render.compute")
                    throw std::invalid_argument("render.compute ports bind as resource ports, not inputs: " + port);
                if (declaration.value("kind", std::string{"buffer"}) != "image" ||
                    declaration.value("access", std::string{}) != "read" || declaration.contains("attachment"))
                    throw std::invalid_argument("an input port reads an image: " + port);
                for (const auto *key : {"view", "sampling", "subresource"})
                    if (declaration.contains(key))
                        throw std::invalid_argument(std::string("an input port takes no ") + key + ": " + port);
                const auto index = declaration.find("input_index");
                if (index == declaration.end() || !index->is_number_integer() || index->get<std::int64_t>() < 0)
                    throw std::invalid_argument("an input port requires a non-negative integer input_index: " + port);
                input_indices[index->get<std::int64_t>()].push_back(port);
            } else if (declaration.is_object() && declaration.contains("input_index")) {
                throw std::invalid_argument("input_index requires binding \"input\": " + port);
            }
        }
        // The numbers are the pass's `input` positions: each once, from 0.
        std::int64_t expected = 0;
        for (const auto &[index, holders] : input_indices) {
            if (index != expected)
                throw GraphRegistryError("graph.input_index_invalid@1",
                    {{"node", nodeId(node)}, {"input_index", expected}, {"reason", "missing"},
                     {"ports", Json::array()}});
            if (holders.size() > 1)
                throw GraphRegistryError("graph.input_index_invalid@1",
                    {{"node", nodeId(node)}, {"input_index", index}, {"reason", "duplicate"}, {"ports", holders}});
            ++expected;
        }
        for (const auto &[port, declaration] : ports.items()) {
            const auto type = resourceType(registry, declaration);
            contract.inputs.emplace(port, GraphInputContract{exactLogicalTypePattern(registry, type)});
            const auto access = declaration.at("access").get<std::string>();
            if (access != "read" && access != "write" && access != "read_write")
                throw std::invalid_argument("unknown compute port access: " + access);
            // LP-0a: `history` reads the image as the previous frame left it.
            // Nothing writes a previous frame, so a writing port is refused.
            if (const auto history = declaration.find("history"); history != declaration.end()) {
                if (!history->is_boolean())
                    throw std::invalid_argument("render port history must be a boolean: " + port);
                if (history->get<bool>() && declaration.value("kind", std::string{"buffer"}) != "image")
                    throw std::invalid_argument("render port history applies to images: " + port);
                if (history->get<bool>() && (access != "read" || declaration.contains("attachment")))
                    throw GraphRegistryError("graph.history_write@1", {{"node", nodeId(node)}, {"port", port}});
            }
            if (access != "read") contract.outputs.emplace(port, type);
        }
        checkRenderPlacement(op, parameters, node);
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
    if (registration.factory && contract.inputs.empty() && contract.outputs.empty()) {
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
        // AG-2: one node of an operation can cost more than the operation does
        // -- a pose.clip_sample that extracts root motion keeps the previous
        // frame's displacement and is `replay` where the plain sampler is
        // `direct`. So the resolved classification is the stronger of the two
        // (direct < replay < baked < recorded): a factory may raise it and can
        // never relax it. A factory that leaves `evaluation` alone gets the
        // registration's value back, which is what every factory but that one
        // does today.
        contract.evaluation = std::max(registration.contract.evaluation, contract.evaluation);
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
    [[maybe_unused]] const GraphInputContract optional_voice{exactLogicalTypePattern(*types, voice), false};
    [[maybe_unused]] const GraphInputContract optional_events{events.accepted_type, false};
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
#if PELICAN_WITH_AUDIO
    // Registered only where something can play the voice, the same way the
    // pose module is: a build without audio never registers these operations,
    // so a document naming one is graph.operation_unregistered@1 before any
    // sound is prepared.
    // AU-1: audio.playback is one voice in either of two forms. The
    // integrated one carries value.playback's time map (wrap, source_begin,
    // source_end, speed, delay; the numbers literal or an edge); the
    // decomposed one follows `position`. In both, events are optional (AU-3):
    // without them the owner's first frame starts the voice, so a node placed
    // with only its `time` plays. Which asset and bus the voice plays, and how
    // it wraps, are fixed when the voice starts, so changing them rebuilds.
    auto playback = value({{"time", required}, {"events", optional_events}, {"position", optional},
            {"source_begin", optional}, {"source_end", optional}, {"speed", optional},
            {"delay", optional}}, {{"voice", voice}}, GraphOperationEvaluation::replay);
    playback.contract.parameter_changes = {{"asset", GraphParameterChange::rebuild},
        {"bus", GraphParameterChange::rebuild}, {"wrap", GraphParameterChange::rebuild}};
    registry.registerModule(GraphModuleManifest{
        "pelican.audio", 1,
        "Sound playback voices and the value sinks that drive them.",
        builtinAudioTypeSchemas(),
        {
            {"audio.playback", std::move(playback)},
            {"audio.volume", value({{"value", required}, {"voice", optional_voice}}, {})},
            {"audio.pitch", value({{"value", required}, {"voice", optional_voice}}, {})},
        },
        {"audio.volume", "audio.pitch"}});
#endif
    // LP-0e: a pass type whose sockets are fixed, so the catalog declares
    // them; the factory still checks the parameters.
    const auto pass_kind = [&](const char *op) {
        auto registration = declared({}, {}, GraphExecutionDomain::rendering,
            GraphOperationEvaluation::direct, GraphParameterChange::rebuild);
        registration.factory = [types, scalar](const nlohmann::json &node) {
            return renderPorts(*types, scalar, node);
        };
        LogicalTypePattern image;
        image.constructor = LogicalTypeConstructor::image;
        for (const auto &socket : renderPassKindSockets(op))
            registration.contract.inputs.emplace(socket, GraphInputContract{image});
        return std::pair<const std::string, GraphOperationRegistration>{op, std::move(registration)};
    };
    const auto render = [&](const char *op) {
        auto registration = declared({}, {}, GraphExecutionDomain::rendering,
            GraphOperationEvaluation::direct, GraphParameterChange::rebuild);
        registration.factory = [types, scalar](const nlohmann::json &node) {
            return renderPorts(*types, scalar, node);
        };
        return std::pair<const std::string, GraphOperationRegistration>{op, std::move(registration)};
    };
    // LP-3a: a draw step's ports are fixed, so the catalog declares them; the
    // factory still checks the parameters.
    const auto draw_stream = types->canonicalize(parseSemanticTypeId("pelican.render.draw_stream@1"));
    const auto draw = [&](const char *op) {
        auto entry = render(op);
        entry.second.contract.inputs.emplace(
            "after", GraphInputContract{exactLogicalTypePattern(*types, draw_stream), false});
        entry.second.contract.outputs.emplace("draws", draw_stream);
        return entry;
    };
    registry.registerModule(GraphModuleManifest{
        "pelican.render", 1,
        "GPU buffers, attributes, images, passes and the scalar publication sink.",
        builtinRenderTypeSchemas(),
        {
            // The publication sink runs where the value it uploads is evaluated.
            {"render.scalar", declared({{"value", required}}, {}, GraphExecutionDomain::values,
                GraphOperationEvaluation::direct, GraphParameterChange::rebuild)},
            pass_kind("render.shadow_depth"), pass_kind("render.snapshot_copy"),
            render("render.buffer"), render("render.attribute"), render("render.image"),
            render("render.target_ref"),
            render("render.compute"), render("render.fullscreen"), render("render.raster"),
            // LP-3a: the material pass, and the steps a draw_loop draws one
            // material with.
            render("render.material_pass"),
            draw("draw.material"), draw("draw.variant"), draw("draw.override"),
        },
        {"render.scalar"}});
    // GP-3. The UI has no build without it, so the module always registers;
    // whether a project has a UI is the render config's reference to the ui
    // feature, checked when the config is prepared (ui.feature_unreferenced@1).
    // Changing which node a sink writes rebuilds, as a render.scalar target does.
    const auto ui_sink = [&] {
        return declared({{"value", required}}, {}, GraphExecutionDomain::values,
                        GraphOperationEvaluation::direct, GraphParameterChange::rebuild);
    };
    registry.registerModule(GraphModuleManifest{
        "pelican.ui", 1,
        "Sinks that write a UI node's value and visibility from the graph.",
        {},
        {
            {"ui.value", ui_sink()},
            {"ui.visible", ui_sink()},
        },
        {"ui.value", "ui.visible"}});
#if PELICAN_WITH_ANIMATION
    // A module registers the same way whether the engine brings it or not; a
    // build without the animation backend simply never calls this, and a
    // document naming a pose operation is rejected as graph.operation_unregistered@1.
    registerPoseGraphModule(registry);
#endif
    return registry;
}

const GraphModuleRegistry &builtinGraphModuleRegistry() {
    static const GraphModuleRegistry registry = makeBuiltinGraphModuleRegistry();
    return registry;
}

std::vector<std::string> renderMaterialPassColorPorts(const nlohmann::json &parameters) {
    if (const auto outputs = parameters.find("material_outputs"); outputs != parameters.end()) {
        std::vector<std::string> ports;
        try {
            for (const auto &output : parseMaterialOutputSchema(*outputs, "render.material_pass material_outputs").outputs)
                ports.push_back(output.name);
        } catch (const std::runtime_error &error) {
            throw std::invalid_argument(error.what());
        }
        return ports;
    }
    const auto contract = materialPassContractFromName(parameters.value("material_contract", std::string{}));
    if (!contract) throw std::invalid_argument("render.material_pass requires a known material_contract");
    if (materialPassShaderContract(*contract) == MaterialShaderContract::forward_scene_color_v1) return {"color"};
    return {"albedo", "normal", "material", "worldpos", "emissive"};
}

bool isRenderMaterialPassParameter(std::string_view key) {
    static const std::set<std::string, std::less<>> keys{
        // The material pass's own fields (passfieldownership.cpp).
        "material_contract", "material_filter", "material_variant", "material_range", "gpu_draw_source",
        "material_outputs", "material_output_states", "screen_inputs", "surface_resources", "material_resources",
        "ray_query",
        // Its attachments' load, store and clear.
        "color_load_op", "color_store_op", "depth_load_op", "depth_store_op", "clear_color", "clear_colors",
        // Any pass's views and region tags. LP-3b: `regions` is the key the
        // pass JSON reads them from (parseOptionalRegionTags), so a
        // decomposed pass keeps its tags.
        "view_family", "view_select", "region_tags", "regions",
        // LP-Vb: the extent domain it renders in (a cube capture's passes
        // are `independent` of the scene's), and the pass whose surface
        // resources, material resources and screen inputs it takes when it
        // names none of its own (resolveInheritedPassBindings).
        "resolution_domain", "inherit_bindings_from",
        // Where it lands.
        "pass_set", "after", "before", "insert", "canonical_anchor"};
    return keys.contains(key);
}

} // namespace Pelican
