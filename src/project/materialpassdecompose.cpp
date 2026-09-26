#include "materialpassdecompose.hpp"

#include "graphcomposition.hpp"
#include "graphmodule.hpp"
#include "renderpipeline.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void refuse(const std::string &pass, const std::string &reason, OrderedJson extra,
                         const std::string &message) {
    OrderedJson fields{{"pass", pass}, {"reason", reason}};
    for (const auto &[key, value] : extra.items()) fields[key] = value;
    throw MaterialPassDecomposeError(std::string{materialPassNotDecomposableCode}, std::move(fields),
                                     "Material pass " + pass + " cannot be decomposed: " + message);
}

// The name an insert looks the neighbor up by: a canonical anchor by its
// anchor, any other pass by its name.
std::string anchorNameOf(const OrderedJson &pass) {
    if (pass.value("type", std::string{}) == "canonical_anchor") return pass.value("anchor", std::string{});
    return pass.value("name", std::string{});
}

OrderedJson targetType(const std::string &target, bool depth, const ComposedRenderTargetLookup &lookup) {
    const Json *declaration = lookup ? lookup(target) : nullptr;
    std::string topology;
    if (declaration != nullptr && declaration->is_object()) {
        if (declaration->value("dimension", std::string{"2d"}) == "cube") topology = "cube";
        else if (const auto layers = declaration->find("layers");
                 layers != declaration->end() && layers->is_number_unsigned() && layers->get<std::uint64_t>() > 1)
            topology = "two_d_array";
    }
    // A color attachment: the one-piece pass states nothing about the image
    // (a G-buffer normal is no scene color), so neither does the reference;
    // its kind and topology are then not compared either.
    if (!depth) return {{"semantic", "pelican.render.legacy_opaque_image@1"}, {"arguments", OrderedJson::object()}};
    OrderedJson type{{"semantic", "pelican.render.depth@1"},
                     {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
    if (!topology.empty()) type["arguments"]["topology"] = topology;
    return type;
}

} // namespace

MaterialPassDecomposition decomposeMaterialPass(OrderedJson &pass_owner, OrderedJson &graph, const std::string &pass,
                                                const ComposedRenderTargetLookup &composed_target) {
    // Find the declaration: its family and its index in the family.
    const auto families = pass_owner.find("rendering_passes");
    OrderedJson *family = nullptr;
    std::size_t index = 0;
    if (families != pass_owner.end() && families->is_array()) {
        for (auto &candidate : *families) {
            const auto passes = candidate.find("passes");
            if (passes == candidate.end() || !passes->is_array()) continue;
            for (std::size_t i = 0; i < passes->size(); ++i) {
                if (passes->at(i).is_object() && passes->at(i).value("name", std::string{}) == pass) {
                    family = &candidate;
                    index = i;
                    break;
                }
            }
            if (family != nullptr) break;
        }
    }
    if (family == nullptr) refuse(pass, "not_found", OrderedJson::object(), "no pass of that name is declared here");
    auto &passes = family->at("passes");
    const auto &declaration = passes.at(index);
    const auto type = declaration.value("type", std::string{});
    if (type != "material") refuse(pass, "not_material", {{"type", type}}, "it is a " + type + " pass");

    // Its contract must be one a draw_loop walks (the legacy one has no queue
    // of its own; graph.loop_contract_unsupported@1).
    const auto contract_name = declaration.value("material_contract", std::string{});
    const auto contract = materialPassContractFromName(contract_name);
    if (!contract || *contract == MaterialPassContract::legacy_gbuffer_v1)
        refuse(pass, "contract_unsupported",
               {{"contract", declaration.contains("material_contract") ? declaration.at("material_contract")
                                                                         : OrderedJson(nullptr)}},
               "a draw_loop does not walk the " + (contract_name.empty() ? std::string{"legacy"} : contract_name) +
                   " contract");

    // Every field but the ones the sockets and the id decide goes to the node.
    OrderedJson parameters = OrderedJson::object();
    for (const auto &[key, value] : declaration.items()) {
        if (key == "name" || key == "type" || key == "output") continue;
        if (key == "pass_set" || key == "insert" || key == "canonical_anchor" || !isRenderMaterialPassParameter(key))
            refuse(pass, "field_unsupported", {{"field", key}}, "render.material_pass does not take " + key);
        parameters[key] = value;
    }

    // Its attachments: one socket per color the contract (or its
    // material_outputs) names, and the depth.
    const auto output = declaration.find("output");
    if (output == declaration.end() || !output->is_object())
        refuse(pass, "attachment_unsupported", {{"socket", nullptr}}, "it has no output");
    std::vector<std::string> sockets;
    try {
        sockets = renderMaterialPassColorPorts(Json::parse(parameters.dump()));
    } catch (const std::exception &error) {
        refuse(pass, "attachment_unsupported", {{"socket", nullptr}}, error.what());
    }
    std::vector<std::string> colors;
    if (const auto color = output->find("color"); color != output->end()) {
        if (color->is_string()) {
            colors.push_back(color->get<std::string>());
        } else if (color->is_array()) {
            for (std::size_t location = 0; location < color->size(); ++location) {
                const auto &entry = color->at(location);
                if (!entry.is_string())
                    refuse(pass, "attachment_unsupported",
                           {{"socket", location < sockets.size() ? OrderedJson(sockets[location]) : OrderedJson(nullptr)}},
                           "a graph attachment names a whole target, not a subresource");
                colors.push_back(entry.get<std::string>());
            }
        }
    }
    if (colors.size() != sockets.size())
        refuse(pass, "attachment_unsupported", {{"socket", nullptr}},
               "it writes " + std::to_string(colors.size()) + " colors where its contract has " +
                   std::to_string(sockets.size()) + " sockets");
    const auto depth = output->find("depth");
    if (depth == output->end() || !depth->is_string() || depth->get_ref<const std::string &>().empty())
        refuse(pass, "attachment_unsupported", {{"socket", "depth"}}, "render.material_pass requires a depth target");

    // The ids it adds must be free.
    const auto found_nodes = graph.find("nodes");
    const auto nodes = found_nodes != graph.end() && found_nodes->is_array() ? *found_nodes : OrderedJson::array();
    const auto found_definitions = graph.find("definitions");
    const auto definitions = found_definitions != graph.end() && found_definitions->is_object()
        ? *found_definitions : OrderedJson::object();
    std::set<std::string> taken;
    for (const auto &node : nodes)
        if (node.is_object() && node.contains("id") && node.at("id").is_string())
            taken.insert(node.at("id").get<std::string>());
    const auto claim = [&](const std::string &id) {
        if (!taken.insert(id).second) throw GraphRegistryError("graph.node_duplicate@1", {{"node", id}, {"scope", "root"}});
    };
    MaterialPassDecomposition result;
    result.loop = pass + "_draws";
    result.definition = pass + "_draw";
    claim(pass);
    claim(result.loop);
    if (definitions.contains(result.definition))
        throw GraphRegistryError("graph.definition_duplicate@1", {{"definition", result.definition}});

    // A render.target_ref per target: one already in the root that names the
    // target (whatever image type it declares: the socket takes any image and
    // composition has checked the type against the target), or a new
    // `<target>_ref`.
    std::vector<OrderedJson> added;
    const auto reference = [&](const std::string &target, bool is_depth) {
        const auto wanted = targetType(target, is_depth, composed_target);
        const auto matches = [&](const OrderedJson &node) {
            if (!node.is_object() || node.value("op", std::string{}) != "render.target_ref") return false;
            const auto found = node.find("parameters");
            return found != node.end() && found->is_object() && found->value("name", std::string{}) == target;
        };
        for (const auto &node : nodes)
            if (matches(node)) return node.at("id").get<std::string>();
        for (const auto &node : added)
            if (matches(node)) return node.at("id").get<std::string>();
        const auto id = target + "_ref";
        claim(id);
        added.push_back({{"id", id}, {"op", "render.target_ref"},
                         {"parameters", {{"name", target}, {"type", wanted}}}, {"outputs", {"resource"}}});
        result.added_target_refs.push_back(id);
        return id;
    };
    OrderedJson inputs = OrderedJson::object();
    for (std::size_t location = 0; location < sockets.size(); ++location) {
        const auto id = reference(colors[location], false);
        result.target_refs.push_back(id);
        inputs[sockets[location]] = {{"node", id}, {"port", "resource"}};
    }
    {
        const auto id = reference(depth->get<std::string>(), true);
        result.target_refs.push_back(id);
        inputs["depth"] = {{"node", id}, {"port", "resource"}};
    }
    inputs["draws"] = {{"node", result.loop}, {"port", "draws"}};

    // Its place: before the pass that followed it, or after the one before
    // it, among the declared passes (insert.declared: the features compose
    // around it as they did around the one-piece pass).
    result.family = family->value("name", std::string{});
    parameters["pass_set"] = result.family;
    if (index + 1 < passes.size()) {
        result.insert = {{"before", anchorNameOf(passes.at(index + 1))}, {"declared", true}};
    } else if (index > 0) {
        result.insert = {{"after", anchorNameOf(passes.at(index - 1))}, {"declared", true}};
    }
    if (!result.insert.is_null()) parameters["insert"] = result.insert;

    // Its draws: the else branch draws each material as the pass did.
    OrderedJson step = parameters.contains("material_variant")
        ? OrderedJson{{"id", "step"}, {"op", "draw.variant"},
                      {"parameters", {{"variant", parameters.at("material_variant")}}}, {"outputs", {"draws"}}}
        : OrderedJson{{"id", "step"}, {"op", "draw.material"}, {"parameters", OrderedJson::object()},
                      {"outputs", {"draws"}}};
    OrderedJson loop_parameters{{"over", {{"draw_items", {{"contract", contract_name}}}}}};
    if (const auto filter = parameters.find("material_filter"); filter != parameters.end()) {
        loop_parameters["where"] = {{"tags", *filter}};
        parameters.erase("material_filter");
    }
    loop_parameters["order"] = {{"by", "phase_default"}};
    loop_parameters["branches"] = OrderedJson::array({{{"body", result.definition}}});

    // Nothing has changed yet; now the documents do.
    auto &graph_definitions = graph["definitions"];
    if (!graph_definitions.is_object()) graph_definitions = OrderedJson::object();
    graph_definitions[result.definition] = {{"nodes", OrderedJson::array({std::move(step)})},
                                            {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}};
    auto &graph_nodes = graph["nodes"];
    if (!graph_nodes.is_array()) graph_nodes = OrderedJson::array();
    for (auto &node : added) graph_nodes.push_back(std::move(node));
    graph_nodes.push_back({{"id", result.loop}, {"op", "graph.loop"}, {"parameters", std::move(loop_parameters)},
                     {"outputs", {"draws"}}});
    graph_nodes.push_back({{"id", pass}, {"op", "render.material_pass"}, {"parameters", std::move(parameters)},
                     {"inputs", std::move(inputs)}, {"outputs", OrderedJson::array()}});
    passes.erase(passes.begin() + static_cast<OrderedJson::difference_type>(index));
    return result;
}

} // namespace Pelican
