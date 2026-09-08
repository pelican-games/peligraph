#include "rendergraphcomposition.hpp"
#include "graphpartition.hpp"

#include <algorithm>
#include <limits>

#include <stdexcept>
#include <set>

namespace Pelican {
namespace {
using Json = nlohmann::json;
void forbid(const Json &parameters, std::initializer_list<const char *> fields) {
    if (!parameters.is_object()) throw std::invalid_argument("render node parameters must be an object");
    for (const auto field : fields)
        if (parameters.contains(field))
            throw std::invalid_argument(std::string("render graph derives field from connections: ") + field);
}
}

Json lowerRenderGraphDocument(const Json &document,
    const std::set<std::string> &inactive_scopes) {
    const auto graph = partitionGraphDocument(document).rendering;
    auto config = document.value("rendering", Json::object());
    // Existing global settings, presets and image passes can coexist during
    // migration, but buffer/compute declarations have one authoring owner.
    forbid(config, {"buffers", "compute_tasks"});
    config["buffers"] = Json::array();
    config["compute_tasks"] = Json::array();
    std::map<std::string, const Json *> nodes;
    for (const auto &node : graph.nodes) nodes.emplace(node.at("id").get<std::string>(), &node);
    const auto inactive = [&](const std::string &id) {
        const auto found = graph.node_scopes.find(id);
        return found != graph.node_scopes.end() &&
            std::any_of(found->second.begin(), found->second.end(),
                [&](const auto &scope) { return inactive_scopes.contains(scope); });
    };
    std::set<std::string> resolving;
    std::function<std::string(const Json &)> resourceName = [&](const Json &endpoint) {
        const auto id = endpoint.at("node").get<std::string>();
        const auto &node = *nodes.at(id);
        if (node.at("op") == "render.buffer" || node.at("op") == "render.attribute" ||
            node.at("op") == "render.image") return id;
        if (!resolving.insert(id).second)
            throw std::invalid_argument("cyclic render resource connection: " + id);
        auto resource = resourceName(node.at("inputs").at(endpoint.at("port").get<std::string>()));
        resolving.erase(id);
        return resource;
    };
    for (const auto &node : graph.nodes) {
        const auto id = node.at("id").get<std::string>();
        if (inactive(id)) continue;
        auto parameters = node.at("parameters");
        // Ordering is not a data dependency: an active task need not wait for
        // a task that no longer executes. Unknown names still reach validation.
        for (const auto *key : {"after", "before"}) {
            if (!parameters.contains(key)) continue;
            auto &order = parameters.at(key);
            if (!order.is_array()) throw std::invalid_argument("render ordering must be an array");
            order.erase(std::remove_if(order.begin(), order.end(), [&](const Json &name) {
                return inactive(name.get<std::string>());
            }), order.end());
        }
        if (node.at("op") == "render.buffer" || node.at("op") == "render.attribute") {
            if (parameters.contains("source"))
                throw std::invalid_argument("attribute source must be resolved before render lowering: " + id);
            forbid(parameters, {"name"});
            if (node.at("op") == "render.attribute" && parameters.at("domain") == "index")
                parameters["index_buffer"] = true;
            parameters.erase("element");
            parameters.erase("domain");
            parameters.erase("count");
            parameters["name"] = id;
            config["buffers"].push_back(std::move(parameters));
            continue;
        }
        if (node.at("op") == "render.image") {
            forbid(parameters, {"name", "kind"});
            parameters.erase("type");
            parameters["name"] = id;
            config["render_targets"].push_back(std::move(parameters));
            continue;
        }
        const bool graphics = node.at("op") != "render.compute";
        forbid(parameters, {"name", "reads", "writes", "resource_ports"});
        if (graphics) forbid(parameters, {"type", "input", "output"});
        auto ports = parameters.at("ports");
        parameters.erase("ports");
        if (node.at("op") == "render.raster" && parameters.contains("draw")) {
            auto &draw = parameters.at("draw");
            const auto bind_count = [&](const char *source_key, const char *count_key,
                                        const char *first_key, const char *domain) {
                if (!draw.contains(source_key)) return;
                if (draw.contains(count_key))
                    throw std::invalid_argument(std::string("draw count is derived from attribute: ") + count_key);
                const auto port = draw.at(source_key).get<std::string>();
                const auto &attribute = *nodes.at(resourceName(node.at("inputs").at(port)));
                if (attribute.at("op") != "render.attribute" ||
                    attribute.at("parameters").at("domain") != domain || ports.at(port).at("access") != "read")
                    throw std::invalid_argument(std::string("draw source requires a readable ") + domain + " attribute");
                const auto count = attribute.at("parameters").at("count").get<std::uint64_t>();
                const auto first = draw.value(first_key, std::uint64_t{0});
                if (first > count || count > (std::numeric_limits<std::uint32_t>::max)())
                    throw std::invalid_argument("attribute range exceeds direct draw limits");
                draw[count_key] = count - first;
                draw.erase(source_key);
            };
            if (draw.contains("index_source")) {
                forbid(draw, {"vertex_source", "vertex_count", "first_vertex", "index_buffer", "operation"});
                const auto port = draw.at("index_source").get<std::string>();
                const auto buffer = resourceName(node.at("inputs").at(port));
                bind_count("index_source", "index_count", "first_index", "index");
                draw["operation"] = "indexed";
                draw["index_buffer"] = buffer;
            }
            bind_count("vertex_source", "vertex_count", "first_vertex", "vertex");
            bind_count("instance_source", "instance_count", "first_instance", "instance");
        }
        parameters["name"] = id;
        parameters["reads"] = Json::array();
        parameters["writes"] = Json::array();
        parameters["resource_ports"] = Json::object();
        Json attachments = {{"color", Json::array()}, {"depth", nullptr}};
        std::map<unsigned, std::string> colors;
        std::set<std::string> reads, writes;
        auto after = parameters.value("after", Json::array());
        if (!after.is_array()) throw std::invalid_argument("render after must be an array");
        for (const auto &[port, declaration] : ports.items()) {
            for (const auto &[key, value] : declaration.items())
                if (key != "element" && key != "domain" && key != "access" && key != "kind" && key != "type" &&
                    key != "binding" && key != "view" && key != "sampling" && key != "subresource" &&
                    !(graphics && (key == "attachment" || key == "location")))
                    throw std::invalid_argument("unknown render port setting: " + key);
            const auto &endpoint = node.at("inputs").at(port);
            const auto producer = endpoint.at("node").get<std::string>();
            const auto resource = resourceName(endpoint);
            if (nodes.at(producer)->at("op") != "render.buffer" &&
                nodes.at(producer)->at("op") != "render.attribute" &&
                nodes.at(producer)->at("op") != "render.image" &&
                std::find(after.begin(), after.end(), producer) == after.end())
                after.push_back(producer);
            const auto access = declaration.at("access").get<std::string>();
            if (access != "read" && access != "write" && access != "read_write")
                throw std::invalid_argument("unknown compute port access: " + access);
            if (graphics && declaration.contains("attachment")) {
                if (declaration.value("kind", std::string{"buffer"}) != "image" || access != "write")
                    throw std::invalid_argument("render attachment requires a write image port: " + port);
                for (const auto *key : {"binding", "view", "sampling", "subresource"})
                    if (declaration.contains(key))
                        throw std::invalid_argument("shader resource setting on render attachment: " + port);
                const auto attachment = declaration.at("attachment").get<std::string>();
                if (attachment == "color") {
                    const auto &location = declaration.at("location");
                    if (!location.is_number_integer() || location.get<std::int64_t>() < 0 ||
                        location.get<std::uint64_t>() > (std::numeric_limits<unsigned>::max)())
                        throw std::invalid_argument("invalid color attachment location: " + port);
                    if (!colors.emplace(location.get<unsigned>(), resource).second)
                        throw std::invalid_argument("duplicate color attachment location");
                } else if (attachment == "depth") {
                    if (declaration.contains("location") || !attachments["depth"].is_null())
                        throw std::invalid_argument("duplicate depth attachment or invalid depth location");
                    attachments["depth"] = resource;
                } else throw std::invalid_argument("unknown render attachment: " + attachment);
                continue;
            }
            if (graphics && (access != "read" || declaration.contains("location")))
                throw std::invalid_argument("graphics shader ports require read access; outputs require attachments");
            if (access != "write") reads.insert(resource);
            if (access != "read") writes.insert(resource);
            auto binding = declaration;
            binding.erase("type");
            binding["resource"] = resource;
            binding["kind"] = declaration.value("kind", std::string{"buffer"});
            binding["access"] = declaration.value("binding",
                graphics && binding["kind"] == "image" ? std::string{"sampled"} : std::string{"storage"});
            binding.erase("binding");
            parameters["resource_ports"][port] = std::move(binding);
        }
        parameters["reads"] = reads;
        parameters["writes"] = writes;
        parameters["after"] = std::move(after);
        if (graphics) {
            for (const auto &[location, resource] : colors) {
                if (location != attachments["color"].size())
                    throw std::invalid_argument("color attachment locations must be contiguous from zero");
                attachments["color"].push_back(resource);
            }
            parameters.erase("reads");
            parameters.erase("writes");
            parameters["input"] = reads;
            parameters["output"] = std::move(attachments);
            parameters["type"] = node.at("op") == "render.fullscreen" ? "fullscreen" : "raster";
            const auto family = parameters.value("pass_set", std::string{"main"});
            parameters.erase("pass_set");
            if (!graph.node_scopes.at(id).empty())
                config["target_planning"]["graphs"][family]["nodes"][id]["isolate"] = true;
            auto &families = config["rendering_passes"];
            if (families.is_null()) families = Json::array();
            auto found = std::find_if(families.begin(), families.end(), [&](const Json &candidate) {
                return candidate.at("name") == family;
            });
            if (found == families.end()) {
                families.push_back({{"name", family}, {"passes", Json::array()}});
                found = std::prev(families.end());
            }
            (*found)["passes"].push_back(std::move(parameters));
        } else config["compute_tasks"].push_back(std::move(parameters));
    }
    return config;
}
}
