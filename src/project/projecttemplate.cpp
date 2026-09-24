#include "projecttemplate.hpp"

#include "projectformat.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Pelican {

namespace {

struct TemplateFile {
    std::filesystem::path relative_path;
    std::string_view contents;
};

std::string pathString(const std::filesystem::path &path) {
    return path.string();
}

std::filesystem::path absolutePath(const std::filesystem::path &path) {
    if (path.is_absolute()) {
        return path;
    }
    return std::filesystem::current_path() / path;
}

std::filesystem::path weaklyCanonicalOrThrow(const std::filesystem::path &path, std::string_view context) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        throw std::runtime_error(std::string{context} + " failed to normalize path: " + pathString(path) +
                                 " (" + ec.message() + ")");
    }
    return canonical;
}

bool directoryEmpty(const std::filesystem::path &path) {
    std::error_code ec;
    const auto it = std::filesystem::directory_iterator{path, ec};
    if (ec) {
        throw std::runtime_error("failed to inspect project directory: " + pathString(path) + " (" +
                                 ec.message() + ")");
    }
    return it == std::filesystem::directory_iterator{};
}

void ensureWritableProjectRoot(const std::filesystem::path &project_root) {
    std::error_code ec;
    if (!std::filesystem::exists(project_root, ec)) {
        std::filesystem::create_directories(project_root, ec);
        if (ec) {
            throw std::runtime_error("failed to create project directory: " + pathString(project_root) + " (" +
                                     ec.message() + ")");
        }
        return;
    }
    if (ec) {
        throw std::runtime_error("failed to inspect project directory: " + pathString(project_root) + " (" +
                                 ec.message() + ")");
    }
    if (!std::filesystem::is_directory(project_root, ec) || ec) {
        throw std::runtime_error("project init target is not a directory: " + pathString(project_root));
    }
    if (!directoryEmpty(project_root)) {
        throw std::runtime_error("project init target directory is not empty: " + pathString(project_root));
    }
}

void writeTextFile(const std::filesystem::path &path, std::string_view contents) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file.is_open()) {
        throw std::runtime_error("failed to write file: " + pathString(path));
    }
    file << contents;
}

constexpr std::string_view project_json = R"json({
  "schema": "pelican.project",
  "version": 1,
  "name": "pelican-project",
  "generator": "pelican_cli project init",
  "engine_min_version": "0.1.0",
  "saved_by_engine": "@PELICAN_ENGINE_VERSION@",
  "basic_config": {
    "window_title": "Pelican Project",
    "window_size": {
      "width": 1280,
      "height": 720
    },
    "fullscreen": false,
    "framerate": 60,
    "camera": {
      "yfov": 0.7853981633974483,
      "znear": 0.1,
      "zfar": 1000,
      "up": [0.0, -1.0, 0.0]
    },
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui_overlay.json",
    "input_actions_json": "input/actions.json",
    "input_profiles": {"keyboard": "input/profiles/keyboard.json"},
    "input_profile": "keyboard"
  }
}
)json";

constexpr std::string_view scene_json = R"json({
  "schema": "pelican.scene",
  "version": 2,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "MainCamera",
          "components": [
            {
              "name": "transform",
              "pos": [0.0, 0.0, -3.0],
              "rotation": [0.0, 0.0, 0.0, 1.0],
              "scale": [1.0, 1.0, 1.0]
            },
            {
              "name": "camera"
            }
          ]
        },
        {
          "name": "KeyLight",
          "components": [
            {
              "name": "light",
              "type": "directional",
              "direction": [-0.5, -1.0, 0.25],
              "intensity": 1.0,
              "color": [1.0, 0.95, 0.85]
            }
          ]
        }
      ]
    }
  }
}
)json";

constexpr std::string_view asset_data_json = R"json({
  "schema": "pelican.asset_data",
  "version": 2,
  "models": []
}
)json";

constexpr std::string_view input_actions_json = R"json({
  "schema": "pelican.input_actions",
  "version": 1,
  "action_sets": [
    {
      "name": "gameplay",
      "actions": [
        {
          "name": "move",
          "type": "axis2"
        }
      ]
    }
  ]
}
)json";

constexpr std::string_view input_keyboard_profile_json = R"json({
  "schema": "pelican.input_profile",
  "version": 1,
  "name": "keyboard",
  "bindings": [
    {"action": "move", "binding": "kbd:wasd"}
  ]
}
)json";

// P-2a-4b / MG-2b: a new project is authored as a `pelican.graph` root, the
// same shape every shipping project has. The literal is the whole document,
// written as the graph root directly (the bytes are those the retired
// wrapLegacyRenderingConfig produced, so this file is unchanged for a
// template user).
constexpr std::string_view rendering_config_json = R"json({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "pipeline": {
      "preset": "engine://render_pipelines/hybrid_v1.json"
    },
    "features": [
      "engine://features/shadow_directional.json",
      "engine://features/sky_ambient.json"
    ]
  }
})json"
                                                   "\n";

// F-2: the blank template's rendering config. No `pipeline` key makes it a
// direct config, so the engine runs exactly the passes listed here plus the
// canonical anchors and `output_transform` it always adds. `present` writes
// `display`, the name `output_transform` reads -- which is also what lets the
// HDR path find the scene's last pass.
constexpr std::string_view blank_rendering_config_json = R"json({
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "rendering_passes": [
      {
        "name": "main_render",
        "passes": [
          {
            "name": "present",
            "type": "fullscreen",
            "input": [],
            "output": {
              "color": [
                "display"
              ],
              "depth": null
            },
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "project://shaders/present"
            }
          }
        ]
      }
    ]
  }
})json"
                                                         "\n";

// Reads nothing, so it needs no bindings: a pixel's colour is where it sits on
// the screen, and a first frame that is not one flat colour says the pass ran.
constexpr std::string_view blank_present_frag = R"glsl(#version 460

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(inUV.x, inUV.y, 0.5, 1.0);
}
)glsl";

std::string_view renderingConfigJson(ProjectTemplate kind) {
    return kind == ProjectTemplate::Blank ? blank_rendering_config_json
                                          : rendering_config_json;
}

constexpr std::string_view ui_overlay_json = R"json({
  "images": []
}
)json";

// X-1: pelican_packages() builds every package vendored under packages/ that
// ships code.  It comes after pelican_game_sources so the game logic DLL those
// objects join already exists.
constexpr std::string_view code_cmake =
    "pelican_game_sources(game.cpp)\n"
    "pelican_packages()\n";

constexpr std::string_view game_cpp = R"cpp(#include <gamecontext.hpp>
#include <gamesystem.hpp>

namespace {

class StarterSystem {
  public:
    void update(Pelican::GameContext &) {}
};

} // namespace

PELICAN_REGISTER_SYSTEM(StarterSystem, 100);
)cpp";

constexpr std::string_view gitattributes = R"txt(*.glb -text
*.vrm -text
*.png -text
*.wav -text
*.spv -text

# Uncomment these lines if this project uses Git LFS.
# *.glb filter=lfs diff=lfs merge=lfs -text
# *.vrm filter=lfs diff=lfs merge=lfs -text
# *.png filter=lfs diff=lfs merge=lfs -text
# *.wav filter=lfs diff=lfs merge=lfs -text
# *.spv filter=lfs diff=lfs merge=lfs -text
)txt";

constexpr std::string_view gitignore = R"txt(.pelican/
build/
build-*/
out/
output/
*.user
*.log
)txt";

constexpr std::string_view readme = R"md(# Pelican Project

Generated by `pelican_cli project init`.

## First Steps

Run the project:

```sh
pelican_player --project .
```

Place model files under `assets/models/` and texture or audio files under `assets/textures/` or `assets/audio/`.
After adding a model, register it in `assets/asset_data.json` and reference it from `scenes/main.scene.json`.

Large binary assets should normally use Git LFS. The generated `.gitattributes` already marks common binary asset
extensions as non-text and includes commented Git LFS tracking lines you can enable when the project adopts LFS.
)md";

constexpr std::string_view gitkeep = "\n";

// MG-1: `project init` is one of the two writers of `saved_by_engine` (the
// other is `project migrate`), so the record names the engine that wrote it.
std::string_view projectJson() {
    static const std::string text = [] {
        std::string result{project_json};
        constexpr std::string_view token = "@PELICAN_ENGINE_VERSION@";
        result.replace(result.find(token), token.size(), pelicanEngineVersion);
        return result;
    }();
    return text;
}


std::vector<TemplateFile> templateFiles(ProjectTemplate kind) {
    std::vector<TemplateFile> files{
        {"project.json", projectJson()},
        {"scenes/main.scene.json", scene_json},
        {"assets/asset_data.json", asset_data_json},
        {"assets/.gitkeep", gitkeep},
        {"assets/models/.gitkeep", gitkeep},
        {"assets/textures/.gitkeep", gitkeep},
        {"assets/audio/.gitkeep", gitkeep},
        {"input/actions.json", input_actions_json},
        {"input/profiles/keyboard.json", input_keyboard_profile_json},
        {"passes/main_rendering_config.json", renderingConfigJson(kind)},
        {"ui/ui_overlay.json", ui_overlay_json},
        {"code/CMakeLists.txt", code_cmake},
        {"code/game.cpp", game_cpp},
        {".gitattributes", gitattributes},
        {".gitignore", gitignore},
        {"README.md", readme},
    };
    if (kind == ProjectTemplate::Blank) {
        files.push_back({"shaders/present.frag", blank_present_frag});
    }
    return files;
}

constexpr std::array<ProjectTemplate, 2> all_templates{ProjectTemplate::Standard,
                                                       ProjectTemplate::Blank};

} // namespace

std::string_view projectTemplateName(ProjectTemplate kind) noexcept {
    switch (kind) {
    case ProjectTemplate::Standard:
        return "standard";
    case ProjectTemplate::Blank:
        return "blank";
    }
    return {};
}

std::optional<ProjectTemplate> projectTemplateFromName(std::string_view name) noexcept {
    for (const ProjectTemplate kind : all_templates) {
        if (projectTemplateName(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

std::span<const ProjectTemplate> projectTemplates() noexcept {
    return all_templates;
}

ProjectInitResult initializeProjectTemplate(const std::filesystem::path &project_dir, ProjectTemplate kind) {
    auto project_root = weaklyCanonicalOrThrow(absolutePath(project_dir), "project init target");
    ensureWritableProjectRoot(project_root);

    ProjectInitResult result{project_root, 0};
    for (const auto &file : templateFiles(kind)) {
        writeTextFile(project_root / file.relative_path, file.contents);
        ++result.files_written;
    }
    return result;
}

} // namespace Pelican
