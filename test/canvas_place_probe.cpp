// CV-6a: holds a frame the player drew with a canvas.place against where the
// placement's arithmetic (canvasplace.hpp) says its items are.
//
//   pelican_test_canvas_place_probe <document.json> <canvas.place id> <frame.png> [--project <dir>]
//       --camera <px> <py> <pz> <qx> <qy> <qz> <qw> --yfov <radians> --znear <n> --zfar <f>
//       [<node>.<port>=<value> ...]
//       [--mark <node> <r> <g> <b> <pixels>] ...  [--pixel <x> <y> <r> <g> <b> <tolerance>] ...
//       [--min-parallax <pixels>]
//
// The camera is the scene camera the player renders with: it looks along its
// rotation's +z with its rotation's +y up (lookAtRH), through
// perspectiveRH_ZO at the frame's aspect, as Camera does.
//
// --mark finds the item whose first authored node is <node>, projects its
// centre, and measures the centroid of the frame's pixels within 48/255 of
// the sRGB colour <r> <g> <b> inside the item's projected quad (grown by 4
// pixels); the two must agree within <pixels>. It also prints where the centre
// would be at depth 0, so a run shows the parallax the depth makes; with
// --min-parallax the two must be at least that far apart (the item is drawn
// off the board's plane, and the pixels are where the depth puts them).
// --pixel asks one pixel to be within <tolerance> of a colour.
// Prints one JSON line per check; exits 1 when any fails.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include "canvas/canvaslowering.hpp"
#include "canvas/canvasplace.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Matrix = Pelican::CanvasMatrix;
using Vec3 = std::array<double, 3>;

Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 normalize(const Vec3 &v) {
    const auto length = std::sqrt(dot(v, v));
    return {v[0] / length, v[1] / length, v[2] / length};
}

// v rotated by the unit quaternion (x, y, z, w).
Vec3 rotate(const std::array<double, 4> &q, const Vec3 &v) {
    const Vec3 u{q[0], q[1], q[2]};
    const auto t = cross(u, v);
    const Vec3 t2{2.0 * t[0], 2.0 * t[1], 2.0 * t[2]};
    const auto c = cross(u, t2);
    return {v[0] + q[3] * t2[0] + c[0], v[1] + q[3] * t2[1] + c[1], v[2] + q[3] * t2[2] + c[2]};
}

Matrix lookAtRH(const Vec3 &eye, const Vec3 &dir, const Vec3 &up) {
    const auto f = normalize(dir);
    const auto s = normalize(cross(f, up));
    const auto u = cross(s, f);
    return {s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0, -dot(s, eye), -dot(u, eye), dot(f, eye), 1};
}

Matrix perspectiveRhZo(double yfov, double aspect, double near, double far) {
    const auto t = std::tan(yfov / 2.0);
    Matrix m{};
    m[0] = 1.0 / (aspect * t);
    m[5] = 1.0 / t;
    m[10] = far / (near - far);
    m[11] = -1.0;
    m[14] = -(far * near) / (far - near);
    return m;
}

std::array<double, 4> transformPoint(const Matrix &m, const std::array<double, 4> &v) {
    std::array<double, 4> result{};
    for (std::size_t row = 0; row < 4; ++row)
        result[row] = m[row] * v[0] + m[4 + row] * v[1] + m[8 + row] * v[2] + m[12 + row] * v[3];
    return result;
}

Matrix multiply(const Matrix &a, const Matrix &b) {
    Matrix result{};
    for (std::size_t column = 0; column < 4; ++column) {
        const auto product = transformPoint(a, {b[column * 4], b[column * 4 + 1], b[column * 4 + 2], b[column * 4 + 3]});
        std::copy(product.begin(), product.end(), result.begin() + static_cast<std::ptrdiff_t>(column * 4));
    }
    return result;
}

} // namespace

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "usage: pelican_test_canvas_place_probe <document.json> <place> <frame.png> --camera ..." << std::endl;
        return 2;
    }
    try {
        std::string project;
        std::array<double, 7> camera{0, 0, 0, 0, 0, 0, 1};
        double yfov = 0.7853981633974483, znear = 0.1, zfar = 100.0;
        Pelican::CanvasValues values;
        struct Mark {
            std::string node;
            std::array<int, 3> rgb;
            double pixels;
        };
        struct Pixel {
            int x, y;
            std::array<int, 3> rgb;
            int tolerance;
        };
        std::vector<Mark> marks;
        std::vector<Pixel> probes;
        double min_parallax = 0.0;
        for (int i = 4; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto next = [&]() -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument("missing a value after " + argument);
                return argv[++i];
            };
            if (argument == "--project") project = next();
            else if (argument == "--camera")
                for (auto &value : camera) value = std::stod(next());
            else if (argument == "--yfov") yfov = std::stod(next());
            else if (argument == "--znear") znear = std::stod(next());
            else if (argument == "--zfar") zfar = std::stod(next());
            else if (argument == "--min-parallax") min_parallax = std::stod(next());
            else if (argument == "--mark") {
                Mark mark;
                mark.node = next();
                for (auto &channel : mark.rgb) channel = std::stoi(next());
                mark.pixels = std::stod(next());
                marks.push_back(mark);
            } else if (argument == "--pixel") {
                Pixel pixel;
                pixel.x = std::stoi(next());
                pixel.y = std::stoi(next());
                for (auto &channel : pixel.rgb) channel = std::stoi(next());
                pixel.tolerance = std::stoi(next());
                probes.push_back(pixel);
            } else {
                const auto equals = argument.find('=');
                const auto dot_at = argument.rfind('.', equals);
                if (equals == std::string::npos || dot_at == std::string::npos)
                    throw std::invalid_argument("expected node.port=value: " + argument);
                values[{argument.substr(0, dot_at), argument.substr(dot_at + 1, equals - dot_at - 1)}] =
                    std::stod(argument.substr(equals + 1));
            }
        }
        int width = 0, height = 0, channels = 0;
        auto *pixels = stbi_load(argv[3], &width, &height, &channels, 4);
        if (pixels == nullptr) throw std::runtime_error(std::string{"cannot decode "} + argv[3]);
        const auto at = [&](int x, int y) {
            const auto *p = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
            return std::array<int, 3>{p[0], p[1], p[2]};
        };

        std::ifstream in(argv[1], std::ios::binary);
        if (!in) throw std::runtime_error(std::string{"cannot read "} + argv[1]);
        const auto document = nlohmann::json::parse(in);
        const Pelican::CanvasAssetReader read_asset =
            [&](std::string_view reference) -> std::shared_ptr<const std::vector<std::byte>> {
            constexpr std::string_view scheme = "project://";
            if (project.empty() || !reference.starts_with(scheme)) return nullptr;
            std::ifstream file(std::filesystem::path{project} / std::string{reference.substr(scheme.size())},
                               std::ios::binary);
            if (!file) return nullptr;
            const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            const auto *begin = reinterpret_cast<const std::byte *>(bytes.data());
            return std::make_shared<const std::vector<std::byte>>(begin, begin + bytes.size());
        };
        const auto ir = Pelican::buildCanvasIr(document, read_asset);
        const auto place = std::find_if(ir.outputs.begin(), ir.outputs.end(), [&](const Pelican::CanvasOutput &output) {
            return output.id == argv[2] && output.placement;
        });
        if (place == ir.outputs.end()) throw std::runtime_error(std::string{"no canvas.place "} + argv[2]);

        const std::array<double, 4> rotation{camera[3], camera[4], camera[5], camera[6]};
        const auto view = lookAtRH({camera[0], camera[1], camera[2]}, rotate(rotation, {0, 0, 1}), rotate(rotation, {0, 1, 0}));
        const auto projection = perspectiveRhZo(yfov, static_cast<double>(width) / height, znear, zfar);
        const auto view_projection = multiply(projection, view);
        const std::array<double, 2> viewport{static_cast<double>(width), static_cast<double>(height)};
        const auto toPixel = [&](const std::array<double, 4> &clip) {
            return std::array<double, 2>{(clip[0] / clip[3] * 0.5 + 0.5) * viewport[0],
                                         (clip[1] / clip[3] * 0.5 + 0.5) * viewport[1]};
        };
        bool passed = true;
        for (const auto &mark : marks) {
            const auto item = std::find_if(place->items.begin(), place->items.end(), [&](const Pelican::CanvasItem &candidate) {
                return candidate.nodes.front() == mark.node;
            });
            if (item == place->items.end()) throw std::runtime_error("no item of " + mark.node);
            const auto index = static_cast<std::size_t>(item - place->items.begin());
            const auto corners = Pelican::canvasPlacedItemCorners(*place, index, view, projection, viewport, values);
            if (!corners.in_front) throw std::runtime_error(mark.node + " is behind the camera");
            Vec3 middle{};
            for (const auto &corner : corners.world)
                for (std::size_t k = 0; k < 3; ++k) middle[k] += corner[k] / 4.0;
            const auto predicted = toPixel(transformPoint(view_projection, {middle[0], middle[1], middle[2], 1.0}));
            // The same centre at depth 0: the board's plane.
            const auto board = Pelican::canvasPlacementMatrix(*place, values);
            const double sx = (corners.stack[0][0] + corners.stack[2][0]) / 2.0;
            const double sy = (corners.stack[0][1] + corners.stack[2][1]) / 2.0;
            const auto flat = toPixel(transformPoint(view_projection, transformPoint(board, {sx, sy, 0.0, 1.0})));
            double x0 = viewport[0], y0 = viewport[1], x1 = 0, y1 = 0;
            for (const auto &corner : corners.pixel) {
                x0 = std::min(x0, corner[0]);
                y0 = std::min(y0, corner[1]);
                x1 = std::max(x1, corner[0]);
                y1 = std::max(y1, corner[1]);
            }
            const int left = std::max(0, static_cast<int>(x0) - 4), top = std::max(0, static_cast<int>(y0) - 4);
            const int right = std::min(width - 1, static_cast<int>(x1) + 4), bottom = std::min(height - 1, static_cast<int>(y1) + 4);
            double count = 0, cx = 0, cy = 0;
            for (int y = top; y <= bottom; ++y)
                for (int x = left; x <= right; ++x) {
                    const auto rgb = at(x, y);
                    if (std::abs(rgb[0] - mark.rgb[0]) <= 48 && std::abs(rgb[1] - mark.rgb[1]) <= 48 &&
                        std::abs(rgb[2] - mark.rgb[2]) <= 48) {
                        count += 1;
                        cx += x + 0.5;
                        cy += y + 0.5;
                    }
                }
            nlohmann::json row{{"mark", mark.node}, {"predicted", predicted}, {"at_depth_0", flat},
                               {"depth", corners.depth}, {"quad", corners.pixel}, {"pixels", count}};
            bool ok = count > 0;
            if (ok) {
                const std::array<double, 2> measured{cx / count, cy / count};
                const auto error = std::hypot(measured[0] - predicted[0], measured[1] - predicted[1]);
                row["measured"] = measured;
                row["error"] = error;
                ok = error <= mark.pixels;
            }
            const auto parallax = std::hypot(predicted[0] - flat[0], predicted[1] - flat[1]);
            row["parallax"] = parallax;
            ok = ok && parallax >= min_parallax;
            row["ok"] = ok;
            passed = passed && ok;
            std::cout << row.dump() << std::endl;
        }
        for (const auto &probe : probes) {
            const auto rgb = at(probe.x, probe.y);
            const bool ok = std::abs(rgb[0] - probe.rgb[0]) <= probe.tolerance &&
                            std::abs(rgb[1] - probe.rgb[1]) <= probe.tolerance &&
                            std::abs(rgb[2] - probe.rgb[2]) <= probe.tolerance;
            passed = passed && ok;
            std::cout << nlohmann::json{{"pixel", {probe.x, probe.y}}, {"rgb", rgb}, {"expected", probe.rgb}, {"ok", ok}}.dump()
                      << std::endl;
        }
        stbi_image_free(pixels);
        return passed ? 0 : 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 2;
    }
}
