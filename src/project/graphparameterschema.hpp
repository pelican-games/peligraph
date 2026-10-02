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
    // CT-4: one of `variants` (ray_query: bool or an object). Listed, not
    // checked here: the op's parser judges it, as it did a json value.
    union_,
};

// The catalog's spelling: number, integer, bool, string, enum, vec2, vec3,
// vec4, color, array, object, asset, shader, node_ref, expression, json,
// union.
std::string_view graphParameterKindName(GraphParameterKind kind);

// CT-4: where a string parameter's value is chosen from. Either an RPC's
// reply -- `rpc` the method, `key` the path to the list in its reply (member
// names joined by '.', an array met on the way walked element by element),
// `field` the member of each entry that holds the name (empty: the entries
// are the names; an entry's field that is an array gives each of its
// strings) -- or the ids of the graph's own nodes of the `graph_ops` ops.
struct GraphParameterChoices {
    std::string rpc;
    std::string key;
    std::string field;
    std::vector<std::string> graph_ops;
};

// CT-4: where a node of the op may stand. owned_instance: only inside a
// graph.instance with an `owner` (graph.op_requires_owned_instance@1
// otherwise).
enum class GraphOperationPlacement {
    anywhere,
    owned_instance,
};

// The catalog's spelling: anywhere, owned_instance.
std::string_view graphOperationPlacementName(GraphOperationPlacement placement);

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
    // CT-4: a slider's range, inside min / max; never checked.
    std::optional<double> soft_min;
    std::optional<double> soft_max;
    // CT-4: an asset's kind (model, image, audio, analysis_table) and the
    // file extensions its reader takes.
    std::string asset_kind;
    std::vector<std::string> extensions;
    std::optional<GraphParameterChoices> choices_from;
    // An array's elements; an object's members when it is a map (any key).
    std::shared_ptr<const GraphParameterSchema> items;
    // An object's named members.
    std::vector<GraphParameterSchema> fields;
    // CT-4: a union's shapes, each without a name.
    std::vector<GraphParameterSchema> variants;
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
    GraphOperationPlacement placement = GraphOperationPlacement::anywhere;
};

// nullptr for an op the table does not know.
const GraphOperationParameters *graphOperationParameters(std::string_view op);

// CT-4: the table's placement; anywhere for an op it does not know.
GraphOperationPlacement graphOperationPlacement(std::string_view op);

// CT-4: graph.op_requires_owned_instance@1 {node, op} for the first node of
// an owned_instance op that no owned graph.instance holds. `owned` tells
// whether an expanded node id lies inside one.
void checkGraphOperationPlacement(const std::string &op, const std::string &node, bool owned);

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
