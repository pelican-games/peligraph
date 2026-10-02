// CC-1a: the mirror check (docs/design_coordinate_convention.md section 5).
//
//   coordinate_mirror_check write <project_dir> <variant>
//   coordinate_mirror_check check <image.png> <variant>
//
// `write` makes a project: a glTF board in the XY plane (normal +Z, front
// faces counter-clockwise from +Z, single-sided) textured with an "F" of
// three rectangles, a red cube on +X and a green cube on +Y, and one camera
// three metres away. The project's basic_config.camera.up is -Y, which the
// engine no longer reads. `check` reads the frame the player rendered and
// requires the world as seen: red on the screen's right, green at the top,
// and the F reading left to right, top to bottom.
//
// Variants: `front` (camera at +Z, no rotation: it looks down -Z with +Y
// up), `side` (camera and scene turned 90 degrees about Y, the same picture)
// and `roll` (the front camera rolled 90 degrees about Z: the picture turns,
// +Y on the screen's right and +X at its bottom).

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

constexpr int textureSize = 64;
constexpr double halfSqrt2 = 0.7071067811865476;

struct Variant {
    std::array<double, 3> camera_pos;
    std::array<double, 4> camera_rotation; // x, y, z, w
    std::array<double, 4> scene_rotation;  // board and markers
    bool rolled = false;
};

bool variantByName(std::string_view name, Variant &variant) {
    if (name == "front") {
        variant = {{0.0, 0.0, 3.0}, {0.0, 0.0, 0.0, 1.0}, {0.0, 0.0, 0.0, 1.0}, false};
        return true;
    }
    if (name == "side") {
        // 90 degrees about Y: the camera at +X looks down -X, the board
        // faces +X, and "+X" of the set is world -Z.
        variant = {{3.0, 0.0, 0.0},
                   {0.0, halfSqrt2, 0.0, halfSqrt2},
                   {0.0, halfSqrt2, 0.0, halfSqrt2},
                   false};
        return true;
    }
    if (name == "roll") {
        variant = {{0.0, 0.0, 3.0},
                   {0.0, 0.0, halfSqrt2, halfSqrt2},
                   {0.0, 0.0, 0.0, 1.0},
                   true};
        return true;
    }
    return false;
}

// Rotates v by the unit quaternion q (x, y, z, w).
std::array<double, 3> rotate(const std::array<double, 4> &q,
                             const std::array<double, 3> &v) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    const std::array<double, 3> u{x, y, z};
    const auto cross = [](const std::array<double, 3> &a,
                          const std::array<double, 3> &b) {
        return std::array<double, 3>{a[1] * b[2] - a[2] * b[1],
                                     a[2] * b[0] - a[0] * b[2],
                                     a[0] * b[1] - a[1] * b[0]};
    };
    const auto t = cross(u, v);
    const std::array<double, 3> t2{2.0 * t[0], 2.0 * t[1], 2.0 * t[2]};
    const auto c = cross(u, t2);
    return {v[0] + w * t2[0] + c[0], v[1] + w * t2[1] + c[1],
            v[2] + w * t2[2] + c[2]};
}

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

// Image rows run top to bottom (glTF uv: v = 0 is the top row).
void writeTexture(const fs::path &path) {
    std::vector<std::uint8_t> pixels(textureSize * textureSize * 4, 235);
    const auto fill = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                auto *pixel = &pixels[(y * textureSize + x) * 4];
                pixel[0] = pixel[1] = pixel[2] = 10;
            }
        }
    };
    fill(16, 10, 24, 54); // the stem, on the left
    fill(16, 10, 50, 18); // the top bar, reaching right
    fill(16, 28, 42, 35); // the middle bar
    for (std::size_t index = 3; index < pixels.size(); index += 4) {
        pixels[index] = 255;
    }
    fs::create_directories(path.parent_path());
    if (stbi_write_png(path.string().c_str(), textureSize, textureSize, 4,
                       pixels.data(), textureSize * 4) == 0) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

template <class T>
void append(std::vector<std::uint8_t> &bytes, const std::vector<T> &values) {
    const auto offset = bytes.size();
    bytes.resize(offset + values.size() * sizeof(T));
    std::memcpy(bytes.data() + offset, values.data(), values.size() * sizeof(T));
    while (bytes.size() % 4 != 0) bytes.push_back(0);
}

struct Mesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs; // empty: untextured
    std::vector<std::uint16_t> indices;
};

void writeGltf(const fs::path &models, const std::string &name, const Mesh &mesh,
               const Json &material) {
    std::vector<std::uint8_t> buffer;
    Json views = Json::array();
    Json accessors = Json::array();
    const auto add = [&](const auto &values, int target, int component,
                         std::size_t count, const char *type) {
        const auto offset = buffer.size();
        append(buffer, values);
        views.push_back({{"buffer", 0},
                         {"byteOffset", offset},
                         {"byteLength", values.size() * sizeof(values[0])},
                         {"target", target}});
        accessors.push_back({{"bufferView", views.size() - 1},
                             {"componentType", component},
                             {"count", count},
                             {"type", type}});
        return accessors.size() - 1;
    };
    const auto vertex_count = mesh.positions.size() / 3;
    Json attributes;
    attributes["POSITION"] = add(mesh.positions, 34962, 5126, vertex_count, "VEC3");
    std::array<float, 3> low{1e9f, 1e9f, 1e9f};
    std::array<float, 3> high{-1e9f, -1e9f, -1e9f};
    for (std::size_t index = 0; index < mesh.positions.size(); ++index) {
        low[index % 3] = std::min(low[index % 3], mesh.positions[index]);
        high[index % 3] = std::max(high[index % 3], mesh.positions[index]);
    }
    accessors[attributes["POSITION"].get<std::size_t>()]["min"] = low;
    accessors[attributes["POSITION"].get<std::size_t>()]["max"] = high;
    attributes["NORMAL"] = add(mesh.normals, 34962, 5126, vertex_count, "VEC3");
    if (!mesh.uvs.empty()) {
        attributes["TEXCOORD_0"] = add(mesh.uvs, 34962, 5126, vertex_count, "VEC2");
    }
    const auto indices =
        add(mesh.indices, 34963, 5123, mesh.indices.size(), "SCALAR");
    writeBytes(models / (name + ".bin"), buffer);
    Json document{
        {"asset", {{"version", "2.0"}, {"generator", "coordinate_mirror_check"}}},
        {"scene", 0},
        {"scenes", Json::array({{{"nodes", Json::array({0})}}})},
        {"nodes", Json::array({{{"mesh", 0}, {"name", name}}})},
        {"meshes", Json::array({{{"name", name},
                                 {"primitives", Json::array({{{"attributes", attributes},
                                                              {"indices", indices},
                                                              {"material", 0}}})}}})},
        {"materials", Json::array({material})},
        {"accessors", accessors},
        {"bufferViews", views},
        {"buffers", Json::array({{{"byteLength", buffer.size()},
                                  {"uri", name + ".bin"}}})},
    };
    if (!mesh.uvs.empty()) {
        document["images"] = Json::array({{{"uri", "f.png"}}});
        document["samplers"] =
            Json::array({{{"magFilter", 9728}, {"minFilter", 9728}}});
        document["textures"] = Json::array({{{"source", 0}, {"sampler", 0}}});
    }
    writeText(models / (name + ".gltf"), document.dump(1));
}

Mesh boardMesh() {
    constexpr float h = 0.8f;
    // 0 bottom-left, 1 bottom-right, 2 top-right, 3 top-left; uv v = 0 at the
    // image's top row. Counter-clockwise seen from +Z (glTF's front).
    return Mesh{
        .positions = {-h, -h, 0, h, -h, 0, h, h, 0, -h, h, 0},
        .normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1},
        .uvs = {0, 1, 1, 1, 1, 0, 0, 0},
        .indices = {0, 1, 2, 0, 2, 3},
    };
}

Mesh cubeMesh(float s) {
    struct Face {
        std::array<float, 3> n, a, b;
    };
    // a x b = n, so each face is counter-clockwise seen from outside.
    constexpr std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
    }};
    Mesh mesh;
    for (const auto &face : faces) {
        const auto base = static_cast<std::uint16_t>(mesh.positions.size() / 3);
        constexpr std::array<std::array<float, 2>, 4> corners{
            {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        for (const auto &[sa, sb] : corners) {
            for (int k = 0; k < 3; ++k) {
                mesh.positions.push_back(s * (face.n[k] + sa * face.a[k] + sb * face.b[k]));
                mesh.normals.push_back(face.n[k]);
            }
        }
        for (const auto offset : {0, 1, 2, 0, 2, 3}) {
            mesh.indices.push_back(static_cast<std::uint16_t>(base + offset));
        }
    }
    return mesh;
}

Json markerMaterial(const std::string &name, const std::array<double, 3> &color) {
    return {{"name", name},
            {"pbrMetallicRoughness",
             {{"baseColorFactor", {color[0], color[1], color[2], 1.0}},
              {"metallicFactor", 0.0},
              {"roughnessFactor", 1.0}}},
            {"emissiveFactor", color},
            {"doubleSided", true}};
}

Json transform(const std::array<double, 3> &pos,
               const std::array<double, 4> &rotation) {
    return {{"name", "transform"},
            {"pos", pos},
            {"rotation", rotation},
            {"scale", {1.0, 1.0, 1.0}}};
}

void writeProject(const fs::path &root, const Variant &variant) {
    fs::remove_all(root);
    const auto models = root / "assets" / "models";
    writeTexture(models / "f.png");
    writeGltf(models, "board", boardMesh(),
              {{"name", "board"},
               {"pbrMetallicRoughness",
                {{"baseColorTexture", {{"index", 0}}},
                 {"metallicFactor", 0.0},
                 {"roughnessFactor", 1.0}}},
               {"emissiveTexture", {{"index", 0}}},
               {"emissiveFactor", {1.0, 1.0, 1.0}},
               {"doubleSided", false}});
    writeGltf(models, "red", cubeMesh(0.12f), markerMaterial("red", {1.0, 0.0, 0.0}));
    writeGltf(models, "green", cubeMesh(0.12f), markerMaterial("green", {0.0, 1.0, 0.0}));

    writeText(root / "assets" / "asset_data.json",
              Json{{"schema", "pelican.asset_data"},
                   {"version", 2},
                   {"models", Json::array({
                                  {{"name", "board"}, {"path", "assets/models/board.gltf"}},
                                  {{"name", "red"}, {"path", "assets/models/red.gltf"}},
                                  {{"name", "green"}, {"path", "assets/models/green.gltf"}},
                              })}}
                  .dump(2));

    Json objects = Json::array();
    objects.push_back(
        {{"name", "Camera"},
         {"components",
          Json::array({transform(variant.camera_pos, variant.camera_rotation),
                       {{"name", "camera"},
                        {"type", "perspective"},
                        {"yfov", 0.7853981633974483},
                        {"znear", 0.1},
                        {"zfar", 100.0}}})}});
    objects.push_back({{"name", "KeyLight"},
                       {"components", Json::array({{{"name", "light"},
                                                    {"type", "directional"},
                                                    {"direction", {-0.5, -1.0, -0.25}},
                                                    {"intensity", 1.0},
                                                    {"color", {1.0, 1.0, 1.0}}}})}});
    objects.push_back(
        {{"name", "Board"},
         {"components", Json::array({transform({0.0, 0.0, 0.0}, variant.scene_rotation),
                                     {{"name", "simplemodelview"}, {"model", "board"}}})}});
    const std::vector<std::pair<std::string, std::array<double, 3>>> markers{
        {"red", {1.0, 0.0, 0.0}}, {"green", {0.0, 1.0, 0.0}}};
    for (const auto &[name, position] : markers) {
        objects.push_back(
            {{"name", name},
             {"components",
              Json::array({transform(rotate(variant.scene_rotation, position),
                                     {0.0, 0.0, 0.0, 1.0}),
                           {{"name", "simplemodelview"}, {"model", name}}})}});
    }
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
  "name": "coordinate-mirror",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Coordinate Mirror",
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

struct Region {
    double count = 0.0;
    double sum_x = 0.0;
    double sum_y = 0.0;
    int min_x = 1 << 30, min_y = 1 << 30, max_x = -1, max_y = -1;

    void add(int x, int y) {
        count += 1.0;
        sum_x += x;
        sum_y += y;
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
    double centerX() const { return sum_x / count; }
    double centerY() const { return sum_y / count; }
};

int failures = 0;

void expect(bool condition, const std::string &what) {
    std::cout << (condition ? "ok   " : "FAIL ") << what << '\n';
    if (!condition) ++failures;
}

int checkImage(const fs::path &png, const Variant &variant) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load(png.string().c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) {
        std::cerr << "cannot decode " << png.string() << '\n';
        return 65;
    }
    const auto at = [&](int x, int y) { return pixels + (y * width + x) * 4; };
    Region red;
    Region green;
    Region board;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto *p = at(x, y);
            const int r = p[0], g = p[1], b = p[2];
            if (r > 150 && g < 90 && b < 90) red.add(x, y);
            if (g > 150 && r < 90 && b < 90) green.add(x, y);
            if (std::min({r, g, b}) > 170) board.add(x, y);
        }
    }
    // The F: dark pixels inside the board (the clear colour outside it is
    // dark too).
    Region letter;
    for (int y = board.min_y + 2; y <= board.max_y - 2; ++y) {
        for (int x = board.min_x + 2; x <= board.max_x - 2; ++x) {
            const auto *p = at(x, y);
            if (std::max({p[0], p[1], p[2]}) < 60) letter.add(x, y);
        }
    }
    double left = 0, right = 0, top = 0, bottom = 0;
    const double letter_x = (letter.min_x + letter.max_x) * 0.5;
    const double letter_y = (letter.min_y + letter.max_y) * 0.5;
    for (int y = letter.min_y; y <= letter.max_y && letter.count > 0; ++y) {
        for (int x = letter.min_x; x <= letter.max_x; ++x) {
            const auto *p = at(x, y);
            if (std::max({p[0], p[1], p[2]}) >= 60) continue;
            (x < letter_x ? left : right) += 1.0;
            (y < letter_y ? top : bottom) += 1.0;
        }
    }
    stbi_image_free(pixels);

    const double cx = width * 0.5;
    const double cy = height * 0.5;
    std::cout << "image " << width << 'x' << height << " red " << red.count << " @ ("
              << red.centerX() << ", " << red.centerY() << ") green " << green.count
              << " @ (" << green.centerX() << ", " << green.centerY() << ") board "
              << board.count << " letter " << letter.count << " left " << left
              << " right " << right << " top " << top << " bottom " << bottom << '\n';
    expect(board.count > width * height * 0.15,
           "the single-sided board faces the camera (front faces are counter-clockwise)");
    expect(red.count >= 8 && green.count >= 8 && letter.count >= 40,
           "the red and green markers and the F are visible");
    if (failures != 0) return 1;
    if (!variant.rolled) {
        expect(red.centerX() > cx + width * 0.2 && std::abs(red.centerY() - cy) < height * 0.1,
               "+X (red) is on the screen's right");
        expect(green.centerY() < cy - height * 0.2 && std::abs(green.centerX() - cx) < width * 0.1,
               "+Y (green) is at the screen's top");
        expect(left > right * 1.3, "the F's stem is on the left (not mirrored left to right)");
        expect(top > bottom * 1.3, "the F's bars are at the top (not upside down)");
    } else {
        expect(green.centerX() > cx + width * 0.2 && std::abs(green.centerY() - cy) < height * 0.1,
               "rolled: +Y (green) is on the screen's right");
        expect(red.centerY() > cy + height * 0.2 && std::abs(red.centerX() - cx) < width * 0.1,
               "rolled: +X (red) is at the screen's bottom");
        expect(top > bottom * 1.3, "rolled: the F's stem lies along the top");
        expect(right > left * 1.3, "rolled: the F's top bar is on the right");
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "usage: coordinate_mirror_check write <project_dir> <variant>\n"
                     "       coordinate_mirror_check check <image.png> <variant>\n";
        return 64;
    }
    Variant variant;
    if (!variantByName(argv[3], variant)) {
        std::cerr << "unknown variant: " << argv[3] << '\n';
        return 64;
    }
    try {
        const std::string_view mode = argv[1];
        if (mode == "write") {
            writeProject(argv[2], variant);
            return 0;
        }
        if (mode == "check") {
            return checkImage(argv[2], variant);
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 70;
    }
    std::cerr << "unknown mode: " << argv[1] << '\n';
    return 64;
}
