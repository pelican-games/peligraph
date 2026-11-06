#include "outputlowering.hpp"

#include "../graphcomposition.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace Pelican {
namespace {
using Json = nlohmann::json;

const Json &emptyObject() {
    static const Json empty = Json::object();
    return empty;
}

std::string stringAt(const Json &object, std::string_view key) {
    if (!object.is_object()) return {};
    const auto found = object.find(std::string{key});
    if (found == object.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

const Json *nodeWithId(const std::map<std::string, const Json *> &nodes,
                       std::string_view id) {
    const auto found = nodes.find(std::string{id});
    return found == nodes.end() ? nullptr : found->second;
}

bool isPassOperation(std::string_view op) {
    return op == "render.fullscreen" || op == "render.raster" ||
           op == "render.compute" || op == "render.material_pass" ||
           op == "render.snapshot_copy" || op == "render.shadow_depth";
}

bool isImageKind(const Json &declaration) {
    const auto kind = declaration.find("kind");
    return kind == declaration.end() || (kind->is_string() && kind->get<std::string>() != "buffer");
}

bool writtenPort(const Json &pass, std::string_view port) {
    const auto op = stringAt(pass, "op");
    if (op == "render.shadow_depth") return port == "depth";
    if (op == "render.snapshot_copy") return port == "destination";
    const auto parameters = pass.find("parameters");
    if (parameters != pass.end() && parameters->is_object()) {
        const auto ports = parameters->find("ports");
        if (ports != parameters->end() && ports->is_object()) {
            const auto declaration = ports->find(std::string{port});
            if (declaration != ports->end() && declaration->is_object()) {
                const auto access = stringAt(*declaration, "access");
                const auto binding = stringAt(*declaration, "binding");
                return (access == "write" || access == "read_write") &&
                       binding != "input" && isImageKind(*declaration);
            }
        }
    }
    // A material pass materialises its outputs from the material contract, so
    // its authored parameters need not spell out `ports`; its declared output
    // list and the edge into the socket still identify a written target.
    const auto outputs = pass.find("outputs");
    if (outputs != pass.end() && outputs->is_array() &&
        std::any_of(outputs->begin(), outputs->end(), [&](const nlohmann::json &output) {
            return output.is_string() && output.get<std::string>() == port;
        })) {
        const auto inputs = pass.find("inputs");
        return inputs != pass.end() && inputs->is_object() &&
               inputs->contains(std::string{port});
    }
    return false;
}

struct SourceTarget {
    std::string graph;
    std::string target;
};

std::optional<std::string> concreteTarget(const Json &graph_document,
                                          const Json &pass,
                                          std::string_view port) {
    const auto inputs = pass.find("inputs");
    if (inputs == pass.end() || !inputs->is_object()) return std::nullopt;
    const auto edge = inputs->find(std::string{port});
    if (edge == inputs->end() || !edge->is_object()) return std::nullopt;
    const auto node = stringAt(*edge, "node");
    if (node.empty()) return std::nullopt;
    std::map<std::string, const Json *> nodes;
    const auto root_nodes = graph_document.find("nodes");
    if (root_nodes != graph_document.end() && root_nodes->is_array()) {
        for (const auto &candidate : *root_nodes) {
            const auto id = stringAt(candidate, "id");
            if (!id.empty()) nodes.emplace(id, &candidate);
        }
    }
    const auto found = nodeWithId(nodes, node);
    if (found == nullptr) return std::nullopt;
    const auto op = stringAt(*found, "op");
    if (op == "render.target_ref") {
        const auto parameters = found->find("parameters");
        const auto name = parameters == found->end() || !parameters->is_object()
                              ? std::string{}
                              : stringAt(*parameters, "name");
        if (!name.empty()) return name;
        return std::nullopt;
    }
    if (op == "render.image") return node;
    return std::nullopt;
}

std::string graphFamily(const Json &document, const Json &producer) {
    const auto parameters = producer.find("parameters");
    if (parameters != producer.end() && parameters->is_object()) {
        const auto pass_set = stringAt(*parameters, "pass_set");
        if (!pass_set.empty()) return pass_set;
    }
    if (const auto rendering = document.find("rendering");
        rendering != document.end() && rendering->is_object()) {
        if (const auto passes = rendering->find("rendering_passes");
            passes != rendering->end() && passes->is_array() && !passes->empty()) {
            const auto name = stringAt(passes->front(), "name");
            if (!name.empty()) return name;
        }
    }
    return "main";
}

void addRetain(Json &document, const std::string &graph, const std::string &target) {
    auto &rendering = document["rendering"];
    if (!rendering.is_object()) rendering = Json::object();
    auto &planning = rendering["target_planning"];
    if (!planning.is_object()) planning = Json::object();
    auto &graphs = planning["graphs"];
    if (!graphs.is_object()) graphs = Json::object();
    auto &entry = graphs[graph];
    if (!entry.is_object()) entry = Json::object();
    auto &resources = entry["resources"];
    if (!resources.is_object()) resources = Json::object();
    auto &resource = resources[target];
    if (!resource.is_object()) resource = Json::object();
    resource["retain"] = true;
}

std::string scopePath(const std::string &definition) {
    return "definitions/" + definition;
}

void collectDefinitionSinks(const Json &document) {
    const auto definitions = document.find("definitions");
    if (definitions == document.end() || !definitions->is_object()) return;
    for (const auto &[name, definition] : definitions->items()) {
        if (!definition.is_object()) continue;
        const auto nodes = definition.find("nodes");
        if (nodes == definition.end() || !nodes->is_array()) continue;
        for (const auto &node : *nodes) {
            const auto op = stringAt(node, "op");
            if (op == "output.file" || op.starts_with("output."))
                throw GraphRegistryError("output.scope_unsupported@1",
                                         {{"node", stringAt(node, "id")},
                                          {"scope", scopePath(name)}});
        }
    }
}

bool validName(std::string_view name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') return false;
    return std::all_of(name.begin() + 1, name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// The source socket is exactly pelican.render.color_signal@1, scene, linear,
// extended. A pass can write depth or a data image through the same shape of
// edge, so the target's own logical type decides, not the port name.
bool isSceneColorSignal(const Json &type) {
    if (!type.is_object() || stringAt(type, "semantic") != "pelican.render.color_signal@1")
        return false;
    const auto arguments = type.find("arguments");
    if (arguments == type.end() || !arguments->is_object()) return false;
    return stringAt(*arguments, "reference") == "scene" &&
           stringAt(*arguments, "transfer") == "linear" &&
           stringAt(*arguments, "range") == "extended";
}

struct ParsedSink {
    const Json *node = nullptr;
    std::string id;
    std::string name;
    std::uint64_t every_n = 1;
    bool enabled = true;
    std::string format = "raw";
    std::string when_full = "wait";
    std::string mode = "offline";
    std::string codec = "h264";
    std::uint64_t fps_num = 0;
    std::uint64_t fps_den = 0;
    bool video = false;
    bool spout = false;
    SourceTarget source;
    std::string source_node;
    std::string source_port;
};

} // namespace

OutputLoweringResult lowerOutputNodes(const nlohmann::json &document) {
    OutputLoweringResult result;
    result.document = document;
    result.report = Json{{"sinks", Json::array()}};
    const auto root = result.document.find("nodes");
    if (root == result.document.end() || !root->is_array()) return result;
    collectDefinitionSinks(result.document);

    std::vector<ParsedSink> parsed;
    std::map<std::string, std::string, std::less<>> names;
    std::map<std::string, const Json *> nodes;
    for (const auto &node : *root) {
        const auto id = stringAt(node, "id");
        if (!id.empty()) nodes.emplace(id, &node);
    }
    for (const auto &node : *root) {
        const auto operation = stringAt(node, "op");
        if (operation != "output.file" && operation != "output.video" &&
            operation != "output.spout") continue;
        ParsedSink sink;
        sink.video = operation == "output.video";
        sink.spout = operation == "output.spout";
        sink.node = &node;
        sink.id = stringAt(node, "id");
        const auto parameters = node.find("parameters");
        const auto &values = parameters == node.end() || !parameters->is_object()
                                 ? emptyObject()
                                 : *parameters;
        const auto invalid = [&](std::string_view key, std::string_view reason) {
            throw GraphRegistryError(sink.spout ? "output.spout_parameter_invalid@1"
                                     : sink.video ? "output.video_parameter_invalid@1"
                                                  : "output.parameter_invalid@1",
                                     {{"node", sink.id},
                                      {"parameter", std::string{key}},
                                      {"reason", std::string{reason}}});
        };
        for (const auto &[key, value] : values.items()) {
            (void)value;
            const bool common = key == "name" || key == "enabled";
            const bool file_parameter = key == "every_n" || key == "format" ||
                                        key == "when_full";
            const bool video_parameter = key == "fps_num" || key == "fps_den" ||
                                         key == "codec" || key == "mode";
            if (!common && (sink.spout || (sink.video ? !video_parameter : !file_parameter)))
                invalid(key, "unknown_parameter");
        }
        sink.name = stringAt(values, "name");
        if (!validName(sink.name) ||
            (sink.spout && sink.name.size() > output_spout_name_bytes))
            throw GraphRegistryError("output.name_invalid@1",
                                     {{"node", sink.id}, {"name", sink.name}});
        if (const auto found = names.find(sink.name); found != names.end())
            throw GraphRegistryError("output.name_duplicate@1",
                                     {{"name", sink.name},
                                      {"nodes", Json::array({found->second, sink.id})}});
        names.emplace(sink.name, sink.id);
        if (const auto every = values.find("every_n"); every != values.end()) {
            // A positive JSON integer of either sign's representation. An
            // unsigned value beyond int64 is still a legal every_n; a signed
            // value must be at least one.
            if (!every->is_number_integer() && !every->is_number_unsigned())
                invalid("every_n", "not_a_positive_integer");
            std::uint64_t value = 0;
            if (every->is_number_unsigned()) {
                value = every->get<std::uint64_t>();
            } else {
                const auto signed_value = every->get<std::int64_t>();
                if (signed_value < 1) invalid("every_n", "not_a_positive_integer");
                value = static_cast<std::uint64_t>(signed_value);
            }
            if (value == 0) invalid("every_n", "not_a_positive_integer");
            sink.every_n = value;
        }
        if (const auto enabled = values.find("enabled"); enabled != values.end()) {
            if (!enabled->is_boolean()) invalid("enabled", "not_a_bool");
            sink.enabled = enabled->get<bool>();
        }
        if (const auto format = values.find("format"); format != values.end()) {
            // VO-1b: only the two encodings this writer implements. Anything
            // else, including a non-string, is refused by name rather than
            // silently treated as raw.
            if (!format->is_string() ||
                (format->get<std::string>() != "raw" &&
                 format->get<std::string>() != "png")) {
                throw GraphRegistryError(
                    "output.file_format_unknown@1",
                    {{"node", sink.id}, {"format", *format}});
            }
            sink.format = format->get<std::string>();
        }
        if (const auto when_full = values.find("when_full");
            when_full != values.end()) {
            if (!when_full->is_string() ||
                (when_full->get<std::string>() != "wait" &&
                 when_full->get<std::string>() != "drop")) {
                throw GraphRegistryError(
                    "output.when_full_unknown@1",
                    {{"node", sink.id}, {"when_full", *when_full}});
            }
            sink.when_full = when_full->get<std::string>();
        }
        if (sink.video) {
            const auto positiveInteger = [&](std::string_view key) {
                const auto found = values.find(std::string{key});
                if (found == values.end() ||
                    (!found->is_number_integer() && !found->is_number_unsigned()))
                    invalid(key, "required_positive_integer");
                std::uint64_t value = 0;
                if (found->is_number_unsigned()) {
                    value = found->get<std::uint64_t>();
                } else {
                    const auto signed_value = found->get<std::int64_t>();
                    if (signed_value < 1) invalid(key, "required_positive_integer");
                    value = static_cast<std::uint64_t>(signed_value);
                }
                if (value == 0 || value > (std::numeric_limits<std::uint32_t>::max)())
                    invalid(key, "required_positive_integer");
                return value;
            };
            sink.fps_num = positiveInteger("fps_num");
            sink.fps_den = positiveInteger("fps_den");
            if (const auto codec = values.find("codec"); codec != values.end()) {
                if (!codec->is_string() || codec->get<std::string>() != "h264")
                    invalid("codec", "unknown_codec");
                sink.codec = codec->get<std::string>();
            }
            if (const auto mode = values.find("mode"); mode != values.end()) {
                if (!mode->is_string() ||
                    (mode->get<std::string>() != "live" &&
                     mode->get<std::string>() != "offline"))
                    invalid("mode", "unknown_mode");
                sink.mode = mode->get<std::string>();
            }
            sink.when_full = sink.mode == "live" ? "drop" : "wait";
        }
        if (sink.spout) sink.when_full = "drop";
        parsed.push_back(std::move(sink));
    }
    if (parsed.empty()) return result;

    for (auto &sink : parsed) {
        if (!sink.enabled) continue;
        const auto inputs = sink.node->find("inputs");
        if (inputs == sink.node->end() || !inputs->is_object()) {
            throw GraphRegistryError("output.source_invalid@1",
                                     {{"node", sink.id}, {"reason", "unconnected"}});
        }
        const auto source = inputs->find("source");
        if (source == inputs->end() || !source->is_object() ||
            stringAt(*source, "node").empty() || stringAt(*source, "port").empty()) {
            throw GraphRegistryError("output.source_invalid@1",
                                     {{"node", sink.id}, {"reason", "unconnected"}});
        }
        const auto producer_id = stringAt(*source, "node");
        const auto producer_port = stringAt(*source, "port");
        const auto producer = nodeWithId(nodes, producer_id);
        if (producer == nullptr || !isPassOperation(stringAt(*producer, "op"))) {
            throw GraphRegistryError("output.source_invalid@1",
                                     {{"node", sink.id},
                                      {"reason", "not_a_render_target_output"},
                                      {"source", producer_id}});
        }
        if (!writtenPort(*producer, producer_port))
            throw GraphRegistryError("output.source_invalid@1",
                                     {{"node", sink.id},
                                      {"reason", "port_not_written"},
                                      {"source", producer_id},
                                      {"port", producer_port}});
        const auto target = concreteTarget(result.document, *producer, producer_port);
        if (!target) {
            throw GraphRegistryError("output.source_invalid@1",
                                     {{"node", sink.id},
                                      {"reason", "not_a_render_target"},
                                      {"source", producer_id},
                                      {"port", producer_port}});
        }
        // The edge names the resource node, which for render.target_ref is the
        // reference's own id, not the external target name.
        std::string resource_id;
        const auto producer_inputs = producer->find("inputs");
        if (producer_inputs != producer->end() && producer_inputs->is_object()) {
            const auto edge = producer_inputs->find(std::string{producer_port});
            if (edge != producer_inputs->end() && edge->is_object())
                resource_id = stringAt(*edge, "node");
        }
        const auto target_node = nodeWithId(nodes, resource_id);
        const Json *declared_type = nullptr;
        if (target_node != nullptr) {
            const auto target_parameters = target_node->find("parameters");
            if (target_parameters != target_node->end() && target_parameters->is_object()) {
                const auto type = target_parameters->find("type");
                if (type != target_parameters->end()) declared_type = &*type;
            }
        }
        if (declared_type == nullptr || !isSceneColorSignal(*declared_type)) {
            throw GraphRegistryError(
                "output.source_invalid@1",
                {{"node", sink.id},
                 {"reason", "not_a_scene_color_signal"},
                 {"target", *target},
                 {"type", declared_type != nullptr ? *declared_type : Json{nullptr}}});
        }
        // The same pass writing the same target through two ports has no one
        // concrete source target the sink could name.
        std::vector<std::string> ports;
        const auto parameters = producer->find("parameters");
        if (parameters != producer->end() && parameters->is_object()) {
            if (const auto declared = parameters->find("ports");
                declared != parameters->end() && declared->is_object()) {
                for (const auto &[port, declaration] : declared->items()) {
                    (void)declaration;
                    if (port != producer_port && writtenPort(*producer, port)) ports.push_back(port);
                }
            }
        }
        for (const auto &port : ports) {
            const auto other = concreteTarget(result.document, *producer, port);
            if (other && *other == *target) {
                throw GraphRegistryError("output.source_ambiguous@1",
                                         {{"node", sink.id},
                                          {"target", *target},
                                          {"source", producer_id},
                                          {"ports", Json::array({producer_port, port})}});
            }
        }
        sink.source = {graphFamily(result.document, *producer), *target};
        sink.source_node = producer_id;
        sink.source_port = producer_port;

    }
    // Every sink in the document counts toward the limit, including one whose
    // `enabled` is false: the limit bounds what the document declares.
    if (parsed.size() > 8)
        throw GraphRegistryError("output.too_many_sinks@1",
                                 {{"count", parsed.size()}, {"limit", 8}});

    std::set<std::string, std::less<>> removed;
    for (const auto &sink : parsed) removed.insert(sink.id);
    Json retained_nodes = Json::array();
    for (const auto &node : *root) {
        if (removed.contains(stringAt(node, "id"))) continue;
        retained_nodes.push_back(node);
    }
    for (auto &node : retained_nodes) {
        const auto inputs = node.find("inputs");
        if (inputs == node.end() || !inputs->is_object()) continue;
        for (auto edge = inputs->begin(); edge != inputs->end();) {
            const auto source_node = stringAt(*edge, "node");
            if (removed.contains(source_node))
                edge = inputs->erase(edge);
            else
                ++edge;
        }
    }
    result.document["nodes"] = std::move(retained_nodes);
    result.changed = true;
    for (const auto &sink : parsed) {
        if (!sink.enabled) {
            // A disabled Spout sink stays in the table, with no source or
            // retain, so its status can say it's inactive and not_enabled.
            if (sink.spout) {
                OutputSinkSpec spec;
                spec.name = sink.name;
                spec.kind = "spout";
                spec.enabled = false;
                spec.when_full = sink.when_full;
                result.sinks.sinks.push_back(std::move(spec));
            }
            continue;
        }
        OutputSinkSpec spec;
        spec.name = sink.name;
        spec.kind = sink.spout ? "spout" : sink.video ? "video" : "file";
        spec.every_n = sink.every_n;
        spec.enabled = true;
        spec.format = sink.format;
        spec.when_full = sink.when_full;
        spec.mode = sink.mode;
        spec.codec = sink.codec;
        spec.fps_num = sink.fps_num;
        spec.fps_den = sink.fps_den;
        spec.graph = sink.source.graph;
        spec.target = sink.source.target;
        spec.source_node = sink.source_node;
        spec.source_port = sink.source_port;
        result.sinks.sinks.push_back(std::move(spec));
        addRetain(result.document, sink.source.graph, sink.source.target);
        Json report{{"name", sink.name},
                    {"kind", sink.spout ? "spout" : sink.video ? "video" : "file"},
                    {"graph", sink.source.graph},
                    {"target", sink.source.target}};
        if (sink.video) {
            report["fps_num"] = sink.fps_num;
            report["fps_den"] = sink.fps_den;
            report["codec"] = sink.codec;
            report["mode"] = sink.mode;
        } else if (!sink.spout) {
            report["every_n"] = sink.every_n;
            report["format"] = sink.format;
        }
        result.report["sinks"].push_back(std::move(report));
    }
    return result;
}

} // namespace Pelican
