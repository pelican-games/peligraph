// MV-1: a morph model after a large static model still deforms.
//
//   morph_capacity_check write <project_dir> <on|off>
//   morph_capacity_check check <image.png> <on|off>
//
// `write` makes a project whose asset list holds a static model of 300,000
// vertices (more than the 262,144 the per-vertex morph metadata once had, all
// of them at one point, so it draws nothing) and, after it, a quad with one
// morph target that moves its top edge right and up. The morph quad's
// vertices land in the shared static vertex buffer after the large model's.
// The morph metadata used to be a fixed array indexed by that position, so
// the start stopped with "glTF morph vertex metadata exceeds renderer
// capacity". `on` gives the mesh weight 1, `off` gives it 0. `check` reads
// the frame the player rendered: with the weight on, the quad reaches the
// screen's top right corner; with it off, it stays the centred square.

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

constexpr std::size_t bulkVertexCount = 300000;

void writeText(const fs::path &path, const std::string &text) {
    fs::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file << text;
    if (!file) throw std::runtime_error("cannot write " + path.string());
}

void writeBytes(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary};
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("cannot write " + path.string());
}

template <class T>
std::size_t append(std::vector<std::uint8_t> &bytes, const std::vector<T> &values) {
    while (bytes.size() % 4 != 0) bytes.push_back(0);
    const auto offset = bytes.size();
    bytes.resize(offset + values.size() * sizeof(T));
    std::memcpy(bytes.data() + offset, values.data(), values.size() * sizeof(T));
    return offset;
}

struct Writer {
    std::vector<std::uint8_t> buffer;
    Json views = Json::array();
    Json accessors = Json::array();

    template <class T>
    std::size_t add(const std::vector<T> &values, std::size_t count, int component,
                    const char *type, int target) {
        const auto offset = append(buffer, values);
        Json view{{"buffer", 0},
                  {"byteOffset", offset},
                  {"byteLength", values.size() * sizeof(T)}};
        if (target != 0) view["target"] = target;
        views.push_back(view);
        accessors.push_back({{"bufferView", views.size() - 1},
                             {"componentType", component},
                             {"count", count},
                             {"type", type}});
        return accessors.size() - 1;
    }

    std::size_t addVec3(const std::vector<float> &values, bool bounds) {
        const auto index = add(values, values.size() / 3, 5126, "VEC3", bounds ? 34962 : 0);
        std::array<float, 3> low{1e9f, 1e9f, 1e9f};
        std::array<float, 3> high{-1e9f, -1e9f, -1e9f};
        for (std::size_t i = 0; i < values.size(); ++i) {
            low[i % 3] = std::min(low[i % 3], values[i]);
            high[i % 3] = std::max(high[i % 3], values[i]);
        }
        accessors[index]["min"] = low;
        accessors[index]["max"] = high;
        return index;
    }

    void write(const fs::path &models, const std::string &name, Json mesh, Json material) {
        writeBytes(models / (name + ".bin"), buffer);
        const Json document{
            {"asset", {{"version", "2.0"}, {"generator", "morph_capacity_check"}}},
            {"scene", 0},
            {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
            {"nodes", Json::array({{{"mesh", 0}, {"name", name}}})},
            {"meshes", Json::array({std::move(mesh)})},
            {"materials", Json::array({std::move(material)})},
            {"accessors", accessors},
            {"bufferViews", views},
            {"buffers", Json::array({{{"byteLength", buffer.size()},
                                      {"uri", name + ".bin"}}})},
        };
        writeText(models / (name + ".gltf"), document.dump(1));
    }
};

Json emissiveMaterial(const std::string &name, const std::array<double, 3> &color) {
    return {{"name", name},
            {"pbrMetallicRoughness",
             {{"baseColorFactor", {color[0], color[1], color[2], 1.0}},
              {"metallicFactor", 0.0},
              {"roughnessFactor", 1.0}}},
            {"emissiveFactor", color},
            {"doubleSided", true}};
}

// 300,000 vertices at one point: 100,000 triangles of no area, no index
// stream. Only the vertex count matters.
void writeBulk(const fs::path &models) {
    Writer writer;
    std::vector<float> positions(bulkVertexCount * 3);
    for (std::size_t vertex = 0; vertex < bulkVertexCount; ++vertex) {
        positions[vertex * 3 + 0] = 0.0f;
        positions[vertex * 3 + 1] = 0.0f;
        positions[vertex * 3 + 2] = -5.0f;
    }
    const auto position = writer.addVec3(positions, true);
    writer.write(models, "bulk",
                 {{"name", "bulk"},
                  {"primitives", Json::array({{{"attributes", {{"POSITION", position}}},
                                               {"material", 0}}})}},
                 emissiveMaterial("bulk", {1.0, 0.0, 0.0}));
}

// A quad of half size 0.6 in the XY plane facing +Z. The one target moves
// the top two vertices by (+0.55, +0.35).
void writeMorphQuad(const fs::path &models, double weight) {
    Writer writer;
    const std::vector<float> positions{-0.6f, -0.6f, 0.0f, 0.6f, -0.6f, 0.0f,
                                       -0.6f, 0.6f, 0.0f, 0.6f, 0.6f, 0.0f};
    const std::vector<float> normals{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const std::vector<std::uint16_t> indices{0, 1, 2, 2, 1, 3};
    const std::vector<float> delta{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                   0.55f, 0.35f, 0.0f, 0.55f, 0.35f, 0.0f};
    const auto position = writer.addVec3(positions, true);
    const auto normal = writer.add(normals, 4, 5126, "VEC3", 34962);
    const auto index = writer.add(indices, indices.size(), 5123, "SCALAR", 34963);
    const auto target = writer.addVec3(delta, false);
    writer.write(models, "morph",
                 {{"name", "morph"},
                  {"weights", Json::array({weight})},
                  {"primitives",
                   Json::array({{{"attributes", {{"POSITION", position}, {"NORMAL", normal}}},
                                 {"indices", index},
                                 {"material", 0},
                                 {"targets", Json::array({{{"POSITION", target}}})}}})}},
                 emissiveMaterial("morph", {0.1, 0.65, 0.95}));
}

Json transform(const std::array<double, 3> &pos) {
    return {{"name", "transform"},
            {"pos", pos},
            {"rotation", {0.0, 0.0, 0.0, 1.0}},
            {"scale", {1.0, 1.0, 1.0}}};
}

void writeProject(const fs::path &root, bool on) {
    fs::remove_all(root);
    const auto models = root / "assets" / "models";
    writeBulk(models);
    writeMorphQuad(models, on ? 1.0 : 0.0);
    // The large model first: its vertices take the start of the shared
    // static vertex buffer.
    writeText(root / "assets" / "asset_data.json",
              Json{{"schema", "pelican.asset_data"},
                   {"version", 2},
                   {"models", Json::array({
                                  {{"name", "bulk"}, {"path", "assets/models/bulk.gltf"}},
                                  {{"name", "morph"}, {"path", "assets/models/morph.gltf"}},
                              })}}
                  .dump(2));

    Json objects = Json::array();
    objects.push_back({{"name", "Camera"},
                       {"components", Json::array({transform({0.0, 0.0, 3.0}),
                                                   {{"name", "camera"},
                                                    {"type", "perspective"},
                                                    {"yfov", 0.7853981633974483},
                                                    {"znear", 0.1},
                                                    {"zfar", 100.0}}})}});
    objects.push_back({{"name", "Bulk"},
                       {"components", Json::array({transform({0.0, 0.0, 0.0}),
                                                   {{"name", "simplemodelview"}, {"model", "bulk"}}})}});
    objects.push_back({{"name", "Morph"},
                       {"components", Json::array({transform({0.0, 0.0, 0.0}),
                                                   {{"name", "simplemodelview"}, {"model", "morph"}}})}});
    writeText(root / "scenes" / "main.scene.json",
              Json{{"schema", "pelican.scene"},
                   {"version", 2},
                   {"scenes", {{"default_scene", {{"objects", objects}}}}}}
                  .dump(2));

    writeText(root / "passes" / "main.json", R"json({
  "schema": "pelican.graph", "version": 1, "definitions": {},
  "nodes": [
    {"id": "scene_color", "op": "render.image",
     "parameters": {"type": {"semantic": "pelican.render.color_signal@1",
                             "arguments": {"reference": "scene", "transfer": "linear", "range": "extended"}},
                    "format": "R16G16B16A16_SFLOAT", "extent_scale": 1.0, "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
     "inputs": {}, "outputs": ["resource"]},
    {"id": "scene_depth", "op": "render.image",
     "parameters": {"type": {"semantic": "pelican.render.depth@1",
                             "arguments": {"representation": "device", "space": "projection"}},
                    "format": "D32_SFLOAT", "extent_scale": 1.0, "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"]},
     "inputs": {}, "outputs": ["resource"]},
    {"id": "fwd", "op": "render.material_pass",
     "parameters": {"material_contract": "forward_opaque_v1", "depth_store_op": "store",
                    "clear_color": [0.07, 0.07, 0.09, 1.0]},
     "inputs": {"color": {"node": "scene_color", "port": "resource"},
                "depth": {"node": "scene_depth", "port": "resource"}},
     "outputs": ["color", "depth"]}
  ],
  "outputs": {},
  "rendering": {
    "material_route_default": "forward",
    "rendering_passes": [{"name": "main_render", "passes": [{
      "name": "present", "type": "fullscreen", "input": ["scene_color"],
      "output": {"color": ["display"], "depth": null},
      "shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"}}]}]
  }
}
)json");
    writeText(root / "ui" / "empty.json",
              R"({"schema":"pelican.ui","version":1,"key":"empty","root":{"id":"root","type":"panel"}})");
    writeText(root / "input" / "actions.json",
              R"({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"gameplay","actions":[{"name":"move","type":"axis2"}]}]})");
    writeText(root / "input" / "profiles" / "keyboard.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"keyboard","bindings":[{"action":"move","binding":"kbd:wasd"}]})");
    writeText(root / "project.json", R"json({
  "schema": "pelican.project",
  "version": 1,
  "name": "morph-capacity",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Morph Capacity",
    "window_size": {"width": 128, "height": 128},
    "fullscreen": false,
    "framerate": 30,
    "camera": {"yfov": 0.7853981633974483, "znear": 0.1, "zfar": 100.0},
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/empty.json",
    "input_actions_json": "input/actions.json",
    "input_profiles": {"keyboard": "input/profiles/keyboard.json"},
    "input_profile": "keyboard"
  }
}
)json");
}

int failures = 0;

void expect(bool condition, const std::string &what) {
    std::cout << (condition ? "ok   " : "FAIL ") << what << '\n';
    if (!condition) ++failures;
}

int checkImage(const fs::path &png, bool on) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(png.string().c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        std::cerr << "cannot decode " << png.string() << '\n';
        return 65;
    }
    double count = 0.0;
    int min_x = width, min_y = height, max_x = -1, max_y = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto *p = pixels + (y * width + x) * 4;
            const int r = p[0], g = p[1], b = p[2];
            if (b > 120 && b > r + 60 && g > r + 30) {
                count += 1.0;
                min_x = std::min(min_x, x);
                min_y = std::min(min_y, y);
                max_x = std::max(max_x, x);
                max_y = std::max(max_y, y);
            }
        }
    }
    stbi_image_free(pixels);
    // With a 45 degree field of view three metres away, the half height of
    // the z = 0 plane is 1.24: the quad's right edge at x = 0.6 falls at
    // 0.74 of the width, the morphed one reaches 1.15, 0.96 of the width.
    std::cout << "image " << width << 'x' << height << " quad " << count << " x ["
              << min_x << ", " << max_x << "] y [" << min_y << ", " << max_y << "]\n";
    expect(count > width * height * 0.1, "the quad is drawn");
    if (failures != 0) return 1;
    if (on) {
        expect(max_x > width * 0.88, "weight 1: the morphed top edge reaches the right");
        expect(min_y < height * 0.18, "weight 1: the morphed top edge rises");
    } else {
        expect(max_x < width * 0.80, "weight 0: the quad keeps its right edge");
        expect(min_y > height * 0.20, "weight 0: the quad keeps its top edge");
    }
    return failures == 0 ? 0 : 1;
}

bool variantByName(std::string_view name, bool &on) {
    if (name == "on") {
        on = true;
        return true;
    }
    if (name == "off") {
        on = false;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: morph_capacity_check write <project_dir> <on|off>\n"
                     "       morph_capacity_check check <image.png> <on|off>\n";
        return 64;
    }
    bool on = false;
    if (!variantByName(argv[3], on)) {
        std::cerr << "unknown variant: " << argv[3] << '\n';
        return 64;
    }
    try {
        const std::string_view mode = argv[1];
        if (mode == "write") {
            writeProject(argv[2], on);
            return 0;
        }
        if (mode == "check") {
            return checkImage(argv[2], on);
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 70;
    }
    std::cerr << "unknown mode: " << argv[1] << '\n';
    return 64;
}
