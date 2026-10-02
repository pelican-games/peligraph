#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

// CT-3: what each graph op's `parameters` holds, read off the parser that
// reads them. One table: get_graph_catalog lists it per op (`parameters`,
// `parameters_complete`), RS-9's required parameters are its `required`
// entries (renderNodeRequiredParameters), and resolveOperation refuses a value
// of the wrong kind with graph.render_node_parameter_invalid@1 before the op's
// own parser reads it. A parameter the table does not name is left to that
// parser, as before.
enum class GraphParameterKind {
    number,
    integer,
    boolean,
    string,
    enumeration,
    vec2,
    vec3,
    vec4,
    color,
    array,
    object,
    asset,
    shader,
    node_ref,
    expression,
    json,
};

// The catalog's spelling: number, integer, bool, string, enum, vec2, vec3,
// vec4, color, array, object, asset, shader, node_ref, expression, json.
std::string_view graphParameterKindName(GraphParameterKind kind);

struct GraphParameterSchema {
    std::string name;
    GraphParameterKind kind = GraphParameterKind::json;
    // Top level: RS-9 refuses a node without it (or without one of
    // `alternatives`, each a parameter or an input edge of that name).
    // Nested (a field): the op's parser refuses the value without it.
    bool required = false;
    std::vector<std::string> alternatives;
    std::optional<nlohmann::json> default_value;
    std::vector<std::string> enum_values;
    std::optional<double> min;
    std::optional<double> max;
    // An array's elements; an object's members when it is a map (any key).
    std::shared_ptr<const GraphParameterSchema> items;
    // An object's named members.
    std::vector<GraphParameterSchema> fields;
    // LP-1b: a loop body may write {"$expr"} / {"$format"} here.
    bool expression = false;
    std::string description;
    // The op refuses a wrong value by its own name (pose.root_node_unknown@1);
    // the kind is listed but not checked here.
    bool checked_by_op = false;
};

struct GraphOperationParameters {
    std::vector<GraphParameterSchema> parameters;
    // Every key the op's parser reads is listed. False: what is listed is
    // right, and the parser reads more.
    bool complete = false;
};

// nullptr for an op the table does not know.
const GraphOperationParameters *graphOperationParameters(std::string_view op);

// The catalog form of one parameter (CT-3 contract).
nlohmann::ordered_json graphParameterSchemaJson(const GraphParameterSchema &parameter);

// RS-9: the required entries, in table order; each is the name followed by
// its alternatives.
std::vector<std::vector<std::string>> graphOperationRequiredParameters(std::string_view op);

// graph.render_node_parameter_invalid@1 {node, op, parameter, expected,
// reason: type | enum | range[, allowed | min | max]} for the first value the
// table types and the node gets wrong. JSON null, an expression object and a
// parameter the table does not name pass.
void checkGraphNodeParameterTypes(const std::string &op, const nlohmann::json &node);

} // namespace Pelican
