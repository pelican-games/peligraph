#include "assetdataformat.hpp"
#include "projectformat.hpp"
#include "projectpathresolver.hpp"
#include "sceneformat.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace {

std::string readTextFile(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        throw std::runtime_error("failed to open " + path.string());
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string requiredReference(const nlohmann::json &basic_config,
                              std::string_view key) {
    const auto found = basic_config.find(std::string{key});
    if (found == basic_config.end() || !found->is_string() ||
        found->get_ref<const std::string &>().empty()) {
        throw std::runtime_error("project basic_config requires " +
                                 std::string{key});
    }
    return found->get<std::string>();
}

} // namespace

int main() {
    try {
        const auto project_root =
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
            "projects" / "example";
        const auto project_json = readTextFile(project_root / "project.json");
        const auto parsed = Pelican::parseProjectEnvelopeText(project_json);

        Pelican::ProjectPathResolver paths;
        paths.setup(project_root, false, parsed.envelope);

        const auto scene_reference = requiredReference(
            parsed.envelope.basic_config, "scene_data_json");
        const auto scene_document = Pelican::normalizeSceneDataJson(
            nlohmann::json::parse(paths.loadText(scene_reference)));

        std::vector<std::string> scene_names;
        scene_names.reserve(scene_document.scenes.size());
        for (const auto &[name, scene] : scene_document.scenes.items()) {
            (void)scene;
            scene_names.push_back(name);
        }

        const auto asset_reference = requiredReference(
            parsed.envelope.basic_config, "asset_data_json");
        const auto asset_document = Pelican::parseAssetDataFormatJson(
            nlohmann::json::parse(paths.loadText(asset_reference)));
        std::vector<std::string> asset_names;
        asset_names.reserve(asset_document.models.size());
        for (const auto &model : asset_document.models) {
            asset_names.push_back(model.name);
        }

        if (scene_names != std::vector<std::string>{
                               "default_scene", "scene_flow_second"}) {
            throw std::runtime_error(
                "pelican_project did not expose the expected scene list");
        }
        if (asset_names != std::vector<std::string>{
                               "alicia", "DamagedHelmet", "sponza",
                               "sotai", "character"}) {
            throw std::runtime_error(
                "pelican_project did not expose the expected asset list");
        }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
