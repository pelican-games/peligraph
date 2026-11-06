#include "graphincomplete.hpp"

#include "imageformat.hpp"

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

struct ImageFill {
    Json declaration;
    Json consumer_parameters;
    std::string consumer_op;
    std::string format;
    std::string instance;
};

std::optional<std::string> magentaImageFormat(const Json &declaration) {
    std::vector<std::string> candidates;
    if (const auto format = declaration.find("format"); format != declaration.end()) {
        if (!format->is_string()) return std::nullopt;
        candidates.push_back(format->get<std::string>());
    }
    if (const auto formats = declaration.find("format_candidates"); formats != declaration.end()) {
        if (!formats->is_array()) return std::nullopt;
        for (const auto &format : *formats) {
            if (!format.is_string()) return std::nullopt;
            candidates.push_back(format.get<std::string>());
        }
    }
    if (candidates.empty()) candidates.push_back("R8G8B8A8_UNORM");
    if (const auto format_class = declaration.find("format_class"); format_class != declaration.end()) {
        if (!format_class->is_string() ||
            format_class->get<std::string>() != "explicit(" + candidates.front() + ")") return std::nullopt;
    }
    const auto format = findImageFormat(candidates.front());
    if (!format || !imageFormatInfo(*format).authorable) return std::nullopt;
    const auto &info = imageFormatInfo(*format);
    if (info.aspect != ImageAspect::color || info.component_count != 4 ||
        imageShaderNumeric(info.storage) != ImageShaderNumeric::floating) return std::nullopt;
    return candidates.front();
}

bool imageExtentSource(const Json &parameters, const std::string &port) {
    const auto dispatch = parameters.find("dispatch");
    if (dispatch == parameters.end() || !dispatch->is_object()) return false;
    for (const auto *field : {"groups_from", "rays_from"}) {
        const auto found = dispatch->find(field);
        if (found != dispatch->end() && found->is_object() &&
            found->value("port", std::string{}) == port) return true;
    }
    return false;
}

std::optional<ImageFill> imageFill(const Json &node, const std::string &port) {
    const auto op = node.value("op", std::string{});
    if (op != "render.fullscreen" && op != "render.raster" && op != "render.compute") return std::nullopt;
    const auto parameters = node.value("parameters", Json::object());
    const auto ports = parameters.find("ports");
    if (ports == parameters.end() || !ports->is_object()) return std::nullopt;
    const auto found = ports->find(port);
    if (found == ports->end() || !found->is_object()) return std::nullopt;
    const auto &declaration = *found;
    for (const auto &[key, value] : declaration.items()) {
        (void)value;
        if (key != "kind" && key != "access" && key != "type" && key != "binding" && key != "history" &&
            key != "format" && key != "format_class" && key != "format_candidates") return std::nullopt;
    }
    if (declaration.value("kind", std::string{"buffer"}) != "image" ||
        declaration.value("access", std::string{}) != "read" ||
        declaration.value("history", false) || declaration.contains("attachment") ||
        declaration.contains("view") || declaration.contains("sampling") || declaration.contains("subresource") ||
        imageExtentSource(parameters, port)) return std::nullopt;
    const auto format = magentaImageFormat(declaration);
    if (!format) return std::nullopt;
    const auto binding = declaration.value("binding", op == "render.compute" ? std::string{"storage"}
                                                                              : std::string{"sampled"});
    if (binding != "sampled") return std::nullopt;
    const auto type = declaration.find("type");
    if (type == declaration.end() || !type->is_object() ||
        type->value("semantic", std::string{}) != "pelican.render.color_signal@1") return std::nullopt;
    const auto arguments = type->value("arguments", Json::object());
    if (!arguments.is_object() || arguments.value("topology", std::string{"two_d"}) != "two_d") return std::nullopt;
    const auto draw = parameters.find("draw");
    if (draw != parameters.end() && draw->is_object())
        for (const auto *field : {"vertex_source", "index_source", "instance_source"})
            if (draw->value(field, std::string{}) == port) return std::nullopt;
    if (op == "render.raster" && (parameters.contains("gpu_draw_source") || parameters.contains("material_outputs") ||
                                  parameters.contains("material_output_states"))) return std::nullopt;
    return ImageFill{declaration, parameters, op, *format, {}};
}

class Completion {
    struct ImageSource {
        std::string image;
        std::string pass;
    };

    const GraphModuleRegistry &modules_;
    const Json definitions_;
    std::vector<GraphIncompleteInput> inputs_;
    std::map<std::string, ImageSource> images_;

    std::string freshId(std::string base, std::set<std::string> &ids) const {
        std::string result = base;
        for (std::size_t suffix = 1; ids.contains(result); ++suffix)
            result = base + "_" + std::to_string(suffix);
        ids.insert(result);
        return result;
    }

    const ImageSource &imageSource(Json &nodes, std::set<std::string> &ids, const std::string &scope,
                                   const std::string &node, const std::string &port, const ImageFill &fill) {
        Json key{{"type", fill.declaration.at("type")}, {"format", fill.format}, {"binding", "sampled"},
                 {"consumer_domain", fill.consumer_op}, {"instance", fill.instance},
                 {"pass_set", fill.consumer_parameters.value("pass_set", Json{})},
                 {"schedule", fill.consumer_parameters.value("schedule", Json{})},
                 {"view_select", fill.consumer_parameters.value("view_select", Json{})},
                 {"view_family", fill.consumer_parameters.value("view_family", Json{})}};
        const auto encoded = scope + "\n" + key.dump();
        if (const auto found = images_.find(encoded); found != images_.end()) return found->second;
        const auto base = freshId("gc2_unconnected_image_" + std::to_string(images_.size()), ids);
        const auto pass = freshId(base + "_producer", ids);
        Json origin{{"scope", scope}, {"node", node}, {"port", port}};
        Json parameters{{"type", fill.declaration.at("type")}, {"format", fill.format},
                        {"format_class", "explicit(" + fill.format + ")"}, {"width", 1}, {"height", 1},
                        {"extent_scale", 1.0}, {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
        nodes.push_back({{"id", base}, {"op", "render.image"}, {"parameters", std::move(parameters)},
                         {"outputs", Json::array({"resource"})}, {"label", "unconnected image: " + node + "." + port},
                         {"origin", origin}});
        Json producer_parameters{{"shader", {{"vertex", "engine://fullscreen"},
                                               {"fragment", "engine://error_magenta"}}},
            {"ports", {{"color", {{"kind", "image"}, {"access", "write"}, {"attachment", "color"},
                                    {"location", 0}, {"type", fill.declaration.at("type")}}}}}};
        for (const auto *field : {"pass_set", "view_select", "view_family"})
            if (fill.consumer_parameters.contains(field)) producer_parameters[field] = fill.consumer_parameters.at(field);
        if (fill.consumer_op != "render.compute") {
            if (fill.consumer_parameters.contains("schedule"))
                producer_parameters["schedule"] = fill.consumer_parameters.at("schedule");
        }
        nodes.push_back({{"id", pass}, {"op", "render.fullscreen"}, {"parameters", std::move(producer_parameters)},
            {"inputs", {{"color", {{"node", base}, {"port", "resource"}}}}},
            {"outputs", Json::array({"color"})}, {"label", "unconnected: " + node + "." + port}, {"origin", origin}});
        return images_.emplace(encoded, ImageSource{base, pass}).first->second;
    }

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

    std::optional<ImageFill> imageBoundaryInput(const Json &body, const std::string &port,
                                                std::set<std::string> &active) const {
        const auto nodes = body.find("nodes");
        if (nodes == body.end() || !nodes->is_array()) return std::nullopt;
        for (const auto &node : *nodes) {
            if (!node.is_object()) continue;
            const auto inputs = node.find("inputs");
            if (inputs == node.end() || !inputs->is_object()) continue;
            for (const auto &[input, endpoint] : inputs->items()) {
                if (!endpoint.is_object() || endpoint.value("node", std::string{}) != port) continue;
                if (node.value("op", std::string{}) == "graph.instance") {
                    const auto nested = node.find("definition");
                    if (nested != node.end() && nested->is_string() && definitions_.contains(nested->get_ref<const std::string &>())) {
                        const auto name = nested->get<std::string>();
                        if (active.insert(name).second) {
                            auto result = imageBoundaryInput(definitions_.at(name), input, active);
                            active.erase(name);
                            if (result) return result;
                        }
                    }
                    continue;
                }
                if (auto result = imageFill(node, input)) return result;
            }
        }
        return std::nullopt;
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
                const std::string synthetic_id = freshId(base, ids);
                Json origin{{"scope", scope}, {"node", id}, {"port", port}};
                nodes->at(index)["inputs"][port] = Json{{"node", synthetic_id}, {"port", "value"}};
                nodes->push_back({{"id", synthetic_id}, {"op", "value.constant"},
                    {"parameters", {{"value", 0.0}}}, {"outputs", Json::array({"value"})},
                    {"label", "unconnected: " + id + "." + port}, {"origin", origin}});
                inputs_.push_back({scope, id, port, "zero_value"});
            };

            auto fillImage = [&](const ImageFill &image, const std::string &port) {
                if (inputs.contains(port)) return;
                const auto &source = imageSource(*nodes, ids, scope, id, port, image);
                nodes->at(index)["inputs"][port] = Json{{"node", source.image}, {"port", "resource"}};
                inputs_.push_back({scope, id, port, "error_image"});
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
                    if (!has_default && is_scalar) {
                        fill(port);
                    } else if (op == "graph.instance" && !has_default) {
                        std::set<std::string> image_active;
                        for (const auto &name : body_names) {
                            if (!definitions_.contains(name)) continue;
                            if (auto image = imageBoundaryInput(definitions_.at(name), port, image_active)) {
                                image->instance = id;
                                fillImage(*image, port);
                                break;
                            }
                        }
                    }
                }
                continue;
            }
            if (op == "graph.input") continue;
            const auto contract = modules_.resolveOperation(node);
            for (const auto &[port, input] : contract.inputs) {
                (void)input;
                if (requiredScalar(contract, port)) fill(port);
                else if (auto image = imageFill(node, port)) fillImage(*image, port);
            }
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

std::string graphIncompleteDiagnostic(Json authoring_document, std::string_view diagnostic) {
    std::string result{diagnostic};
    try {
        auto completed = completeUnconnectedInputs(authoring_document, builtinGraphModuleRegistry());
        const auto annotate = [&](const Json &body, const std::string &scope, auto &&self) -> void {
            for (const auto &node : body.value("nodes", Json::array())) {
                if (node.value("op", std::string{}) == "render.image" && node.contains("origin") &&
                    result.find("(resource '" + node.value("id", std::string{}) + "')") != std::string::npos) {
                    const auto &origin = node.at("origin");
                    const auto owner = origin.value("scope", std::string{}).empty()
                        ? origin.value("node", std::string{})
                        : origin.at("scope").get<std::string>() + "/" + origin.at("node").get<std::string>();
                    result += "; unconnected image input " + owner + "/" + origin.at("port").get<std::string>() +
                              " uses error_image";
                    return;
                }
            }
            for (const auto &[name, definition] : body.value("definitions", Json::object()).items())
                if (definition.is_object()) self(definition, name, self);
        };
        annotate(completed.document, "", annotate);
    } catch (...) {
    }
    return result;
}

} // namespace Pelican
