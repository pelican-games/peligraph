#include "morph_fixture.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {

void write(const std::filesystem::path &path, const std::string &text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error("could not write " + path.string());
    output << text;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: vrm_expression_preview_writer OUTPUT_DIR\n";
        return 2;
    }
    try {
        const std::filesystem::path root{argv[1]};
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "assets");
        std::filesystem::create_directories(root / "passes");
        std::filesystem::create_directories(root / "ui");
        Pelican::TestMorphFixture::writeGlb(
            root / "assets" / "wp123_expression.vrm",
            {.skinned = true, .vrm_expression = true});

        const nlohmann::json project{
            {"schema", "pelican.project"},
            {"version", 1},
            {"name", "WP123 VRM expression preview"},
            {"engine_min_version", "0.1.0"},
            {"basic_config",
             {{"window_title", "WP123 VRM Expression Preview"},
              {"window_size", {{"width", 960}, {"height", 540}}},
              {"fullscreen", false},
              {"framerate", 60},
              {"camera",
               {{"yfov", 0.7853981633974483},
                {"znear", 0.1},
                {"zfar", 100.0},
                {"up", {0.0, 1.0, 0.0}}}},
              {"default_scene_id", "default_scene"},
              {"scene_data_json", "scene.json"},
              {"asset_data_json", "assets.json"},
              {"rendering_config_json", "passes/main.json"},
              {"ui_config_json", "ui/ui.json"},
              {"default_rendering_pass", "main_render"}}},
        };
        write(root / "project.json", project.dump(2) + "\n");
        write(root / "assets.json", R"json({
  "schema": "pelican.asset_data",
  "version": 1,
  "models": [
    {"name":"character","path":"assets/wp123_expression.vrm"}
  ]
})json");
        write(root / "scene.json", R"json({
  "schema":"pelican.scene","version":1,
  "scenes":{"default_scene":{"objects":[
    {"name":"VrmExpressionPreview","components":[
      {"name":"transform","pos":[-0.72,0,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
      {"name":"simplemodelview","model":"character"}
    ]},
    {"name":"VrmExpressionReference","components":[
      {"name":"transform","pos":[0.72,0,0],"rotation":[0,0,0,1],"scale":[0.9,0.9,0.9]},
      {"name":"simplemodelview","model":"character"}
    ]},
    {"name":"Sun","components":[
      {"name":"light","type":"directional","direction":[0.1,-0.2,-1.0],"intensity":4.0,"color":[1.0,0.95,0.9]}
    ]}
  ]}}}
)json");
        write(root / "ui" / "ui.json",
              R"json({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})json");
        std::filesystem::copy_file(
            std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                "projects/example/passes/main_rendering_config.json",
            root / "passes" / "main.json",
            std::filesystem::copy_options::overwrite_existing);
        std::cout << root.string() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
