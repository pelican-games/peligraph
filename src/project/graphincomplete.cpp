#include "graphincomplete.hpp"

#include <functional>
#include <map>
#include <set>
#include <stdexcept>

namespace Pelican {
namespace {
using Json = nlohmann::json;

bool requiredScalar(const GraphOperationContract &contract, const std::string &port) {
    const auto found = contract.inputs.find(port);
    if (found == contract.inputs.end() || !found->second.required) return false;
    const auto &semantic = found->second.accepted_type.semantic;
    return semantic && semanticTypeIdName(*semantic) == "pelican.value.scalar@1";
}

class Completion {
    const GraphModuleRegistry &modules_;
    const Json definitions_;
    std::vector<GraphIncompleteInput> inputs_;

    bool scalarBoundaryInput(const Json &body, const std::string &port,
                             std::set<std::string> &active) const {
        const auto nodes = body.find("nodes");
        if (nodes == body.end() || !nodes->is_array()) return false;
        for (const auto &node : *nodes) {
            if (!node.is_object()) continue;
            const auto op = node.value("op", std::string{});
            const auto inputs = node.find("inputs");
            if (inputs == node.end() || !inputs->is_object()) continue;
            for (const auto &[input, endpoint] : inputs->items()) {
                if (!endpoint.is_object() || endpoint.value("node", std::string{}) != port) continue;
                if (op == "graph.instance" || op == "graph.loop") {
                    const auto nested = op == "graph.instance" ? node.find("definition") : node.end();
                    if (nested != node.end() && nested->is_string() && definitions_.contains(nested->get_ref<const std::string &>())) {
                        const auto name = nested->get<std::string>();
                        if (active.insert(name).second) {
                            const bool result = scalarBoundaryInput(definitions_.at(name), input, active);
                            active.erase(name);
                            if (result) return true;
                        }
                    }
                    continue;
                }
                if (requiredScalar(modules_.resolveOperation(node), input)) return true;
            }
        }
        return false;
    }

    void completeScope(Json &body, const std::string &scope) {
        auto nodes = body.find("nodes");
        if (nodes == body.end()) return;
        if (!nodes->is_array()) throw std::invalid_argument("graph nodes must be an array");
        std::set<std::string> ids;
        for (const auto &node : *nodes)
            if (node.is_object() && node.contains("id") && node.at("id").is_string())
                ids.insert(node.at("id").get<std::string>());

        const auto original_size = nodes->size();
        for (std::size_t index = 0; index < original_size; ++index) {
            const auto node = nodes->at(index);
            if (!node.is_object()) continue;
            const auto id = node.at("id").get<std::string>();
            const auto op = node.at("op").get<std::string>();
            if (node.contains("inputs") && !node.at("inputs").is_object())
                throw std::invalid_argument("graph inputs must be an object: " + id);
            const auto inputs = node.value("inputs", Json::object());

            auto fill = [&](const std::string &port) {
                if (inputs.contains(port)) return;
                const std::string base = "gc2_unconnected_" + id + "_" + port;
                std::string synthetic_id = base;
                for (std::size_t suffix = 1; ids.contains(synthetic_id); ++suffix)
                    synthetic_id = base + "_" + std::to_string(suffix);
                ids.insert(synthetic_id);
                Json origin{{"scope", scope}, {"node", id}, {"port", port}};
                nodes->at(index)["inputs"][port] = Json{{"node", synthetic_id}, {"port", "value"}};
                nodes->push_back({{"id", synthetic_id}, {"op", "value.constant"},
                    {"parameters", {{"value", 0.0}}}, {"outputs", Json::array({"value"})},
                    {"label", "unconnected: " + id + "." + port}, {"origin", origin}});
                inputs_.push_back({scope, id, port, "zero_value"});
            };

            if (op == "graph.instance" || op == "graph.loop") {
                if (node.contains("parameters") && !node.at("parameters").is_object()) continue;
                std::vector<std::string> body_names;
                if (op == "graph.instance") {
                    const auto definition = node.find("definition");
                    if (definition != node.end() && definition->is_string())
                        body_names.push_back(definition->get<std::string>());
                } else if (node.contains("parameters") && node.at("parameters").contains("branches") &&
                           node.at("parameters").at("branches").is_array()) {
                    for (const auto &branch : node.at("parameters").at("branches"))
                        if (branch.is_object() && branch.contains("body") && branch.at("body").is_string())
                            body_names.push_back(branch.at("body").get<std::string>());
                }
                std::set<std::string> ports;
                for (const auto &name : body_names) {
                    if (!definitions_.contains(name)) continue;
                    const auto &body = definitions_.at(name);
                    const auto nested = body.find("nodes");
                    if (nested == body.end() || !nested->is_array()) continue;
                    for (const auto &inner : *nested)
                        if (inner.is_object() && inner.value("op", std::string{}) == "graph.input" &&
                            inner.contains("id") && inner.at("id").is_string())
                            ports.insert(inner.at("id").get<std::string>());
                }
                std::set<std::string> active;
                for (const auto &port : ports) {
                    if (inputs.contains(port) || (node.contains("parameters") && node.at("parameters").contains(port))) continue;
                    bool has_default = false;
                    bool is_scalar = false;
                    for (const auto &name : body_names) {
                        if (!definitions_.contains(name)) continue;
                        const auto &body = definitions_.at(name);
                        for (const auto &inner : body.at("nodes"))
                            if (inner.is_object() && inner.value("id", std::string{}) == port && inner.contains("default"))
                                has_default = true;
                        if (scalarBoundaryInput(body, port, active)) is_scalar = true;
                    }
                    if (!has_default && is_scalar) fill(port);
                }
                continue;
            }
            if (op == "graph.input") continue;
            const auto contract = modules_.resolveOperation(node);
            for (const auto &[port, input] : contract.inputs)
                if (requiredScalar(contract, port)) fill(port);
        }
    }

  public:
    Completion(const Json &document, const GraphModuleRegistry &modules)
        : modules_(modules), definitions_(document.value("definitions", Json::object())) {}

    GraphIncompleteResult run(Json document) {
        completeScope(document, "");
        if (auto definitions = document.find("definitions"); definitions != document.end() && definitions->is_object())
            for (auto &[name, body] : definitions->items())
                if (body.is_object()) completeScope(body, name);
        return {std::move(document), std::move(inputs_)};
    }
};
} // namespace

GraphIncompleteResult completeUnconnectedInputs(Json authoring_document,
                                                const GraphModuleRegistry &modules) {
    return Completion(authoring_document, modules).run(std::move(authoring_document));
}

Json graphIncompleteInputsJson(const std::vector<GraphIncompleteInput> &inputs) {
    auto result = Json::array();
    for (const auto &input : inputs)
        result.push_back({{"scope", input.scope}, {"node", input.node}, {"port", input.port},
                          {"substitute", input.substitute}});
    return result;
}

} // namespace Pelican
