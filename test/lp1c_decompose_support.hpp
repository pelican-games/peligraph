#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// LP-1c: the decomposed standard features the engine ships as graph templates
// (engine://graphs/bloom_loop.json, engine://graphs/planar_mip_chain_loop.json;
// docs/design_graph_loops.md 5.3), and how a project takes one in. A template
// is a pelican.graph document whose `definitions` and `nodes` the project
// copies into its own graph document; its `decomposes` says what happens to
// the one-piece feature it stands for:
//
//   - {"feature": <ref>, "remove_feature": true}: the feature instance with
//     that ref leaves `rendering.features` (bloom: the graph replaces it all);
//   - {"feature": <ref>, "pass_replacements": {...}}: the instance keeps its
//     targets, views and surface resources, and those of its passes / tasks
//     are replaced by the graph's nodes (planar: the mip chain).
//   - "shader_defines": defines the feature gave every shader from its
//     parameters (bloom's threshold and knee), added to the rendering
//     section's shader_defines.
//
// Nothing in the engine reads `decomposes`; this is the procedure Studio and
// an agent follow by hand (or through edit_graph_document's add_definition /
// add_node and the rendering section's features list).
namespace Pelican::TestSupport {

inline std::string lp1cFeatureRef(const nlohmann::json &entry) {
    if (entry.is_string()) return entry.get<std::string>();
    if (entry.is_object() && entry.contains("ref") && entry.at("ref").is_string())
        return entry.at("ref").get<std::string>();
    return {};
}

// `document` with `graph_template` taken in. Throws when an id or definition
// name is taken, or when the rendering section has no instance of the feature.
inline nlohmann::json lp1cApplyDecomposition(nlohmann::json document, const nlohmann::json &graph_template) {
    auto &definitions = document["definitions"];
    if (definitions.is_null()) definitions = nlohmann::json::object();
    for (const auto &[name, body] : graph_template.at("definitions").items()) {
        if (definitions.contains(name)) throw std::runtime_error("LP-1c template definition is taken: " + name);
        definitions[name] = body;
    }
    auto &nodes = document["nodes"];
    if (nodes.is_null()) nodes = nlohmann::json::array();
    std::set<std::string> ids;
    for (const auto &node : nodes) ids.insert(node.at("id").get<std::string>());
    for (const auto &node : graph_template.at("nodes")) {
        if (!ids.insert(node.at("id").get<std::string>()).second)
            throw std::runtime_error("LP-1c template node id is taken: " + node.at("id").get<std::string>());
        nodes.push_back(node);
    }
    if (!document.contains("outputs")) document["outputs"] = nlohmann::json::object();
    const auto &decomposes = graph_template.at("decomposes");
    const auto feature = decomposes.at("feature").get<std::string>();
    auto &features = document.at("rendering").at("features");
    bool found = false;
    for (auto entry = features.begin(); entry != features.end();) {
        if (lp1cFeatureRef(*entry) != feature) {
            ++entry;
            continue;
        }
        found = true;
        if (decomposes.value("remove_feature", false)) {
            entry = features.erase(entry);
            continue;
        }
        if (entry->is_string()) *entry = nlohmann::json{{"ref", feature}};
        (*entry)["pass_replacements"] = decomposes.at("pass_replacements");
        ++entry;
    }
    if (!found) throw std::runtime_error("LP-1c the rendering section has no instance of " + feature);
    // The feature's parameters that its shaders read as PELICAN_FEATURE_*
    // defines. A graph pass may not name them (the engine reserves them for
    // enabled features), so they go where the feature puts them: the
    // composed config's shader_defines, through the rendering section's.
    if (const auto defines = decomposes.find("shader_defines"); defines != decomposes.end()) {
        auto &list = document.at("rendering")["shader_defines"];
        if (list.is_null()) list = nlohmann::json::array();
        for (const auto &define : *defines)
            if (std::find(list.begin(), list.end(), define) == list.end()) list.push_back(define);
    }
    return document;
}

// The one-piece bloom feature's names -> the decomposed graph's, for passes and
// for resources (pass and target names of the frame plan).
inline std::map<std::string, std::string> lp1cBloomNames() {
    std::map<std::string, std::string> names{
        {"HighLuminanceExtraction", "bloom_threshold"},
        {"FinalBloomComposite", "bloom_composite"},
        {"Bloom_Threshold_RT", "bloom_threshold_image"},
        {"bloom.intensity", "bloom_intensity"},
    };
    for (int level = 0; level < 4; ++level) {
        const auto element = std::to_string(level);
        names.emplace("HorizontalBlur_" + element, "bloom_down/" + element + "/h_blur");
        names.emplace("VerticalBlur_" + element, "bloom_down/" + element + "/v_blur");
        names.emplace("Bloom_Downsample_H_" + element + "_RT", "bloom_down/" + element + "/h_image");
        names.emplace("Bloom_Upsample_V_" + element + "_RT", "bloom_down/" + element + "/v_image");
    }
    // UpsampleBlend_<e + 1> draws level e + 1 into level e: the up loop's element e.
    for (int element = 0; element < 3; ++element)
        names.emplace("UpsampleBlend_" + std::to_string(element + 1), "bloom_up/" + std::to_string(element) + "/blend");
    return names;
}

// planar_reflection_filter_mip_<e + 1> -> the mip chain loop's element e.
inline std::map<std::string, std::string> lp1cPlanarNames() {
    std::map<std::string, std::string> names;
    for (int element = 0; element < 6; ++element)
        names.emplace("planar_reflection_filter_mip_" + std::to_string(element + 1),
                      "planar_mip_chain/" + std::to_string(element) + "/filter");
    return names;
}

// LP-1c: a name of the one-piece feature, as the decomposed graph spells it
// (the name itself when the table has none).
inline std::string lp1cRenamed(const std::map<std::string, std::string> &names, const std::string &name) {
    const auto found = names.find(name);
    return found != names.end() ? found->second : name;
}

// A main_render frame plan with every name the one-piece feature gave a pass,
// a task or a resource spelled as the decomposed graph spells it, and without
// what says who authored a node (a feature's pass and a graph node's copy
// differ only there). Reads, writes, barriers and levels are compared as sets.
inline nlohmann::json lp1cComparablePlan(const nlohmann::json &plan, const std::map<std::string, std::string> &names) {
    using Json = nlohmann::json;
    const auto renamedList = [&](const Json &list) {
        std::vector<std::string> result;
        for (const auto &name : list) result.push_back(lp1cRenamed(names, name.get<std::string>()));
        std::sort(result.begin(), result.end());
        return Json(result);
    };
    Json result{{"nodes", Json::array()}, {"barriers", Json::array()}, {"levels", Json::array()}};
    for (auto node : plan.at("nodes")) {
        for (const auto *key : {"source", "provider_feature", "provider_ref", "authored_name", "loop_origin"})
            node.erase(key);
        node["name"] = lp1cRenamed(names, node.at("name").get<std::string>());
        for (const auto *key : {"reads", "reads_history", "writes"}) node[key] = renamedList(node.at(key));
        result["nodes"].push_back(std::move(node));
    }
    std::vector<Json> barriers;
    for (auto barrier : plan.at("barriers")) {
        for (const auto *key : {"from", "to", "resource"})
            if (barrier.at(key).is_string()) barrier[key] = lp1cRenamed(names, barrier.at(key).get<std::string>());
        barriers.push_back(std::move(barrier));
    }
    std::sort(barriers.begin(), barriers.end(), [](const Json &lhs, const Json &rhs) { return lhs.dump() < rhs.dump(); });
    result["barriers"] = barriers;
    for (const auto &level : plan.at("levels")) result["levels"].push_back(renamedList(level));
    return result;
}

} // namespace Pelican::TestSupport
