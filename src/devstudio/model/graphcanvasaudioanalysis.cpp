// AB-1s: the Graph tab's side of a baked analysis table (engine AB-1) --
// the nodes Use in a shader places in one edit and the nodes that read a
// table. (UX-13a: a Re-bake sends no edit; the reload picks the table up,
// IA-3.)

#include "audioanalysistables.hpp"
#include "graphcanvasmodel.hpp"

#include <algorithm>
#include <utility>

namespace PelicanStudio {
namespace {
using Json = nlohmann::json;

std::string textOf(const Json &value, const char *key) {
    const auto found = value.find(key);
    if (found == value.end() || !found->is_string()) return {};
    return found->get<std::string>();
}

// What a render.image the palette places is (graphcanvasmodel.cpp's
// sceneColorType): the table's numbers are carried, not shown.
Json tableImageType() {
    return Json{{"semantic", "pelican.render.color_signal@1"},
                {"arguments", Json{{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

// The stem the nodes are named by: the table's file name without extension.
std::string tableStem(std::string_view asset) {
    std::string name{asset};
    if (const auto slash = name.find_last_of('/'); slash != std::string::npos) name.erase(0, slash + 1);
    if (const auto dot = name.find('.'); dot != std::string::npos) name.erase(dot);
    std::string stem;
    for (const char c : name)
        stem.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_');
    if (stem.empty() || (stem.front() >= '0' && stem.front() <= '9')) stem.insert(0, "table_");
    return stem;
}

} // namespace

Json GraphCanvasModel::sampleTableShaderOps(std::string_view scope, const std::string &asset,
                                            std::uint32_t rows_per_feature, std::uint32_t sample_count,
                                            double hop_seconds, const std::string &pass,
                                            SampleTableShaderPlacement *placed,
                                            const std::vector<std::string> &shader_ports) const {
    Json ops = Json::array();
    const auto relative = projectRelativeReference(asset);
    if (relative.empty()) return ops;
    if (!scope.empty() && scopeNodes(scope) == nullptr) return ops;
    const auto token = scopeToken(scope);
    SampleTableShaderPlacement placement;
    // Free ids, the ones this edit takes counted as taken.
    std::vector<std::string> taken;
    const auto freeId = [&](const std::string &base) {
        auto candidate = base;
        for (int suffix = 2; findNode(scope, candidate) != nullptr ||
                             std::ranges::find(taken, candidate) != taken.end();
             ++suffix)
            candidate = base + "_" + std::to_string(suffix);
        taken.push_back(candidate);
        return candidate;
    };
    const auto stem = tableStem(relative);
    placement.image = freeId(stem + "_table");
    placement.buffer = freeId(stem + "_layout");
    placement.values = freeId(stem + "_layout_values");
    placement.time = freeId(stem + "_layout_time");
    const auto add = [&](Json node) {
        ops.push_back(Json{{"op", "add_node"}, {"scope", token}, {"node", std::move(node)}});
    };
    // The table, as it is: a persistent image that starts from it (its size
    // is the file's), read and never drawn into.
    add(Json{{"id", placement.image},
             {"op", "render.image"},
             {"parameters", Json{{"type", tableImageType()},
                                 {"format", "R32_SFLOAT"},
                                 {"usage", Json::array({"SAMPLED"})},
                                 {"lifetime", "persistent"},
                                 {"initial", Json{{"asset", relative}}}}},
             {"outputs", Json::array({"resource"})}});
    // Its layout and the time, one vec4: the glsl's own example
    // (rows_per_feature, sample_count, hop_seconds, scene_time). The sinks
    // write it from the CPU each frame (runtime_f32).
    add(Json{{"id", placement.buffer},
             {"op", "render.buffer"},
             {"parameters", Json{{"element", "vec4"}, {"size", 16}, {"host_source", "runtime_f32"}}},
             {"outputs", Json::array({"resource"})}});
    add(Json{{"id", placement.values},
             {"op", "render.scalar"},
             {"parameters", Json{{"target", placement.buffer},
                                 {"value", Json::array({static_cast<double>(rows_per_feature),
                                                        static_cast<double>(sample_count), hop_seconds})},
                                 {"offset", 0}}},
             {"outputs", Json::array()}});
    auto clock = sceneTimeNode(scope);
    if (clock.empty()) {
        clock = freeId("scene_time");
        add(Json{{"id", clock},
                 {"op", "value.input"},
                 {"parameters", Json{{"name", "scene_time"}}},
                 {"outputs", Json::array({"value"})}});
    }
    add(Json{{"id", placement.time},
             {"op", "render.scalar"},
             {"parameters", Json{{"target", placement.buffer}, {"offset", 12}}},
             {"inputs", Json{{"value", Json{{"node", clock}, {"port", "value"}}}}},
             {"outputs", Json::array()}});

    // The pass, when one was picked: the table onto its next image binding
    // its shader declares, the layout onto a buffer port it reads as
    // pelican_resource_<port> (UX-13a: chosen by that name, not by a binding
    // number -- the engine binds the port to the variable of its name).
    const auto *target = pass.empty() ? nullptr : findNode(scope, pass);
    const auto pass_op = target != nullptr ? textOf(*target, "op") : std::string{};
    if (target != nullptr && (pass_op == "render.fullscreen" || pass_op == "render.raster")) {
        const auto *declared = passInputs(scope, pass);
        auto parameters = nodeParameters(scope, pass);
        if (!parameters.is_object()) parameters = Json::object();
        auto &ports = parameters["ports"];
        if (!ports.is_object()) ports = Json::object();
        const auto numbered = numberedPorts(parameters);
        std::size_t next = 0;
        while (numbered.contains(static_cast<std::int64_t>(next))) ++next;
        std::vector<std::string> missing;
        Json lines = Json::array();
        const bool readable = declared != nullptr && declared->declared();
        const auto image_declaration =
            readable ? std::ranges::find_if(declared->declarations,
                                            [&](const FramePlanPassInputDeclaration &entry) {
                                                return !entry.variadic && !entry.buffer() &&
                                                       entry.binding == next;
                                            })
                     : std::vector<FramePlanPassInputDeclaration>::const_iterator{};
        if (readable && image_declaration != declared->declarations.end()) {
            auto port = image_declaration->name.empty() ? placement.image : image_declaration->name;
            for (int suffix = 2; ports.contains(port); ++suffix)
                port = (image_declaration->name.empty() ? placement.image : image_declaration->name) + "_" +
                       std::to_string(suffix);
            ports[port] = Json{{"kind", "image"},    {"type", tableImageType()}, {"access", "read"},
                               {"binding", "input"}, {"input_index", next}};
            if (pass_op == "render.fullscreen") ports[port]["sampling"] = Json{{"address", "clamp_to_edge"}};
            placement.image_port = port;
            lines.push_back(Json{{"op", "set_input"}, {"scope", token}, {"node", pass}, {"port", port},
                                 {"source", Json{{"node", placement.image}, {"port", "resource"}}}});
        } else {
            missing.push_back("image binding " + std::to_string(next));
        }
        // UX-13a, decision 1: the names the shader reads a buffer port by --
        // the pelican_resource_<port> buffers its reflection shows, then the
        // ones its text reads through pelican_resource_ports.glsl, which the
        // generated include declares only once the port is there. The one
        // named as the layout's buffer (<stem>_layout) first, else the first
        // no port takes yet, else the first (its line moves to the layout).
        std::vector<std::string> names;
        const auto offer = [&](std::string name) {
            if (name.empty() || std::ranges::find(names, name) != names.end()) return;
            if (const auto held = ports.find(name);
                held != ports.end() && (!held->is_object() || textOf(*held, "kind") != "buffer"))
                return;
            names.push_back(std::move(name));
        };
        if (readable)
            for (const auto &entry : declared->declarations)
                if (!entry.variadic && entry.buffer() && entry.name.starts_with(kResourcePortVariablePrefix))
                    offer(bufferPortName(entry.name));
        for (const auto &name : shader_ports)
            if (name != placement.image_port) offer(name);
        auto chosen = std::ranges::find(names, placement.buffer);
        if (chosen == names.end())
            chosen = std::ranges::find_if(names, [&](const std::string &name) { return !ports.contains(name); });
        if (chosen == names.end()) chosen = names.begin();
        if (chosen != names.end()) {
            const auto port = *chosen;
            ports[port] = Json{{"kind", "buffer"}, {"access", "read"}, {"element", "vec4"}};
            placement.buffer_port = port;
            lines.push_back(Json{{"op", "set_input"}, {"scope", token}, {"node", pass}, {"port", port},
                                 {"source", Json{{"node", placement.buffer}, {"port", "resource"}}}});
        } else {
            missing.push_back("pelican_resource_<port> buffer");
        }
        if (!lines.empty()) {
            ops.push_back(Json{{"op", "set_node_parameters"}, {"scope", token}, {"node", pass},
                               {"parameters", std::move(parameters)}});
            for (auto &line : lines) ops.push_back(std::move(line));
        }
        if (!missing.empty()) {
            std::string what;
            for (const auto &entry : missing) what += (what.empty() ? "" : " or ") + entry;
            placement.note = pass + "'s shader declares no " + what + "; connect it by hand.";
        }
    } else if (!pass.empty()) {
        placement.note = pass + " is not a pass; the nodes are placed without lines.";
    }
    if (placed != nullptr) *placed = std::move(placement);
    return ops;
}

bool GraphCanvasModel::addSampleTableShader(std::string_view scope, const std::string &asset,
                                            std::uint32_t rows_per_feature, std::uint32_t sample_count,
                                            double hop_seconds, const std::string &pass,
                                            SampleTableShaderPlacement *placed,
                                            const std::vector<std::string> &shader_ports) {
    if (!canEdit()) return false;
    auto ops = sampleTableShaderOps(scope, asset, rows_per_feature, sample_count, hop_seconds, pass,
                                    placed, shader_ports);
    if (ops.empty()) return false;
    return submitOps(std::move(ops));
}

std::string GraphCanvasModel::sampleTableAssetOf(std::string_view scope, std::string_view node) const {
    const auto *found = findNode(scope, node);
    if (found == nullptr) return {};
    const auto op = textOf(*found, "op");
    const auto parameters = found->value("parameters", Json::object());
    if (!parameters.is_object()) return {};
    if (op == "value.sample_table") return projectRelativeReference(textOf(parameters, "asset"));
    if (op == "render.image") {
        const auto initial = parameters.find("initial");
        if (initial != parameters.end() && initial->is_object())
            return projectRelativeReference(textOf(*initial, "asset"));
    }
    return {};
}

std::vector<std::string> GraphCanvasModel::sampleTableReaders(std::string_view asset) const {
    std::vector<std::string> readers;
    const auto relative = projectRelativeReference(asset);
    const auto *nodes = scopeNodes({});
    if (nodes == nullptr || relative.empty()) return readers;
    for (const auto &node : *nodes) {
        if (!node.is_object() || textOf(node, "op") != "value.sample_table") continue;
        const auto id = textOf(node, "id");
        if (sampleTableAssetOf({}, id) == relative) readers.push_back(id);
    }
    return readers;
}

} // namespace PelicanStudio
