#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

// MG-2b: the authored rendering config is a `pelican.graph` root and nothing
// else; the older root (the config's own keys at the root) is refused by name
// (render_config.legacy_root@1). A test that authors a rendering config inline
// writes its `rendering` section and places it in the graph root with these.
// The root's own members are the empty graph a project without value or pose
// nodes carries, in the order the shipping projects write them.
namespace Pelican::TestSupport {

template <class Json>
Json renderGraphRoot(Json section) {
    Json root = Json::object();
    root["schema"] = "pelican.graph";
    root["version"] = 1;
    root["definitions"] = Json::object();
    root["nodes"] = Json::array();
    root["outputs"] = Json::object();
    root["rendering"] = std::move(section);
    return root;
}

// The text form: parses the section JSON and returns the graph root, dumped
// with two-space indentation like the shipping documents.
inline std::string renderGraphRootText(std::string_view section_json) {
    return renderGraphRoot(nlohmann::ordered_json::parse(section_json)).dump(2);
}

// The older root could declare `buffers` and `compute_tasks` next to its
// passes. A graph root cannot: those declarations belong to the graph, and the
// lowering refuses them in the `rendering` section by name. A test that
// authored them at the root moves them, unchanged, into one project feature
// that the section names first -- the shape the shipping ray tracing and path
// tracing demos use. The feature is `optional` about the runtime shader
// compiler, as the root was, and composes its declarations in their order
// ahead of every other feature's.
//
// Writes `<project_root>/features/<feature_name>.json` when there is anything
// to move and returns the graph root to write as the rendering config.
template <class Json>
Json renderGraphRootWithTaskFeature(
    Json section, const std::filesystem::path &project_root,
    std::string_view feature_name = "authored_tasks") {
    Json feature = Json::object();
    feature["schema"] = "pelican.render_feature";
    feature["version"] = 1;
    feature["name"] = std::string{feature_name};
    feature["runtime_shader_compiler"] = "optional";
    bool moved = false;
    for (const auto *key : {"buffers", "compute_tasks"}) {
        const auto found = section.find(key);
        if (found == section.end()) continue;
        feature[key] = std::move(*found);
        section.erase(key);
        moved = true;
    }
    if (moved) {
        const auto path =
            project_root / "features" / (std::string{feature_name} + ".json");
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output{path, std::ios::binary | std::ios::trunc};
        if (!output) {
            throw std::runtime_error("cannot write test feature " +
                                     path.string());
        }
        output << feature.dump(2);
        auto features = Json::array();
        features.push_back("project://features/" + std::string{feature_name} +
                           ".json");
        if (const auto existing = section.find("features");
            existing != section.end()) {
            for (auto &entry : *existing) features.push_back(std::move(entry));
        }
        section["features"] = std::move(features);
    }
    return renderGraphRoot(std::move(section));
}

} // namespace Pelican::TestSupport
