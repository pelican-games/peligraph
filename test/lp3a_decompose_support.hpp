#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// LP-3a: a rendering root that selects a pipeline preset, rewritten so that
// some of the preset's material passes are written in the graph instead
// (docs/design_graph_loops.md 4.3):
//
//   - the preset is forked into the rendering section (its config, with the
//     root's own features, shader_defines, graph_transforms and the rest laid
//     over it the way a preset is resolved), less the named passes;
//   - each named pass comes back as a render.material_pass with the same id,
//     the same fields, its attachments through render.target_ref nodes and its
//     place through `insert`;
//   - with `with_loop`, its `draws` come from a graph.loop over draw_items of
//     its contract whose one (else) branch is draw.material -- draw.variant
//     when the pass names a material_variant -- and whose `where` is the
//     pass's material_filter; without, `draws` is left unconnected.
namespace Pelican::TestSupport {

struct Lp3aDecomposedPass {
    std::string name;
    // The pass the render.material_pass lands next to.
    nlohmann::json insert;
};

inline nlohmann::json lp3aSceneColorType() {
    return {{"semantic", "pelican.render.color_signal@1"},
            {"arguments", {{"reference", "scene"}, {"transfer", "linear"}, {"range", "extended"}}}};
}

inline nlohmann::json lp3aDepthType() {
    return {{"semantic", "pelican.render.depth@1"},
            {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
}

// The attachment sockets of a material pass (graphmodule.hpp's
// renderMaterialPassColorPorts, restated so a test does not lean on it).
inline std::vector<std::string> lp3aColorSockets(const nlohmann::json &pass) {
    const auto contract = pass.value("material_contract", std::string{});
    if (contract == "forward_opaque_v1" || contract == "forward_transparent_v1") return {"color"};
    return {"albedo", "normal", "material", "worldpos", "emissive"};
}

inline nlohmann::json lp3aForkPreset(const nlohmann::json &rendering, const nlohmann::json &preset_config) {
    auto forked = preset_config;
    for (const auto &[key, value] : rendering.items()) {
        if (key == "pipeline") continue;
        if (key == "features" || key == "shader_defines" || key == "graph_transforms") {
            auto &list = forked[key];
            if (list.is_null()) list = nlohmann::json::array();
            for (const auto &entry : value)
                if (key == "features" || std::find(list.begin(), list.end(), entry) == list.end())
                    list.push_back(entry);
            continue;
        }
        forked[key] = value;
    }
    return forked;
}

inline nlohmann::json lp3aDecomposeMaterialPasses(nlohmann::json document, const nlohmann::json &preset_config,
                                                  const std::vector<Lp3aDecomposedPass> &decomposed,
                                                  bool with_loop = true) {
    using Json = nlohmann::json;
    auto rendering = lp3aForkPreset(document.at("rendering"), preset_config);
    auto &nodes = document["nodes"];
    if (nodes.is_null()) nodes = Json::array();
    auto &definitions = document["definitions"];
    if (definitions.is_null()) definitions = Json::object();
    std::map<std::string, bool> referenced;
    const auto reference = [&](const std::string &target, bool depth) {
        const auto id = "lp3a_ref_" + target;
        if (!referenced.contains(target)) {
            referenced.emplace(target, true);
            nodes.push_back({{"id", id}, {"op", "render.target_ref"},
                             {"parameters", {{"name", target}, {"type", depth ? lp3aDepthType() : lp3aSceneColorType()}}},
                             {"outputs", {"resource"}}});
        }
        return Json{{"node", id}, {"port", "resource"}};
    };
    for (const auto &entry : decomposed) {
        Json pass;
        std::string family;
        for (auto &candidate : rendering.at("rendering_passes")) {
            auto &passes = candidate.at("passes");
            const auto found = std::find_if(passes.begin(), passes.end(), [&](const Json &item) {
                return item.value("name", std::string{}) == entry.name;
            });
            if (found == passes.end()) continue;
            pass = *found;
            family = candidate.at("name").get<std::string>();
            passes.erase(found);
            break;
        }
        if (pass.is_null() || pass.value("type", std::string{}) != "material")
            throw std::runtime_error("no material pass to decompose: " + entry.name);
        Json parameters = Json::object();
        for (const auto &[key, value] : pass.items())
            if (key != "name" && key != "type" && key != "output" && key != "input") parameters[key] = value;
        parameters["pass_set"] = family;
        parameters["insert"] = entry.insert;
        Json inputs = Json::object();
        const auto &output = pass.at("output");
        const auto colors = lp3aColorSockets(pass);
        const auto &color_targets = output.at("color");
        for (std::size_t location = 0; location < colors.size(); ++location)
            inputs[colors[location]] = reference(
                color_targets.is_array() ? color_targets.at(location).get<std::string>()
                                         : color_targets.get<std::string>(), false);
        inputs["depth"] = reference(output.at("depth").get<std::string>(), true);
        if (with_loop) {
            const auto loop = entry.name + "_draws";
            const auto body = entry.name + "_draw";
            Json step = pass.contains("material_variant")
                ? Json{{"id", "step"}, {"op", "draw.variant"},
                       {"parameters", {{"variant", pass.at("material_variant")}}}, {"outputs", {"draws"}}}
                : Json{{"id", "step"}, {"op", "draw.material"}, {"parameters", Json::object()},
                       {"outputs", {"draws"}}};
            definitions[body] = {{"nodes", Json::array({step})},
                                 {"outputs", {{"draws", {{"node", "step"}, {"port", "draws"}}}}}};
            Json loop_parameters{{"over", {{"draw_items", {{"contract", pass.at("material_contract")}}}}},
                                 {"order", {{"by", "phase_default"}}},
                                 {"branches", Json::array({{{"body", body}}})}};
            if (pass.contains("material_filter")) {
                loop_parameters["where"] = {{"tags", pass.at("material_filter")}};
                parameters.erase("material_filter");
            }
            nodes.push_back({{"id", loop}, {"op", "graph.loop"}, {"parameters", std::move(loop_parameters)},
                             {"outputs", {"draws"}}});
            inputs["draws"] = {{"node", loop}, {"port", "draws"}};
        }
        nodes.push_back({{"id", entry.name}, {"op", "render.material_pass"}, {"parameters", std::move(parameters)},
                         {"inputs", std::move(inputs)}, {"outputs", Json::array()}});
    }
    document["rendering"] = std::move(rendering);
    return document;
}

// Where the hybrid preset's three material passes land so that every pass
// keeps the place it has in the preset, with a feature inserted after
// deferred_lighting (sky_ambient) or at the beginning (shadow_directional)
// ahead of it as before.
inline std::vector<Lp3aDecomposedPass> lp3aHybridMaterialPasses() {
    return {
        {"deferred_geometry", {{"before", "ssao_pass"}}},
        {"forward_opaque", {{"before", "__snapshot_opaque_color"}}},
        {"forward_transparent", {{"after", "__snapshot_opaque_depth"}}},
    };
}

} // namespace Pelican::TestSupport
