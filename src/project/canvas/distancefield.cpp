#include "distancefield.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

constexpr double infinity = std::numeric_limits<double>::infinity();

Point2 operator+(Point2 a, Point2 b) { return {a.x + b.x, a.y + b.y}; }
Point2 operator-(Point2 a, Point2 b) { return {a.x - b.x, a.y - b.y}; }
Point2 operator*(double s, Point2 a) { return {s * a.x, s * a.y}; }
double dot(Point2 a, Point2 b) { return a.x * b.x + a.y * b.y; }
double cross(Point2 a, Point2 b) { return a.x * b.y - a.y * b.x; }
double length(Point2 a) { return std::sqrt(dot(a, a)); }
bool samePoint(Point2 a, Point2 b) { return a.x == b.x && a.y == b.y; }

Point2 normalized(Point2 a) {
    const auto l = length(a);
    return l > 0.0 ? Point2{a.x / l, a.y / l} : Point2{0.0, 0.0};
}

// ---------------------------------------------------------------------------
// (a) coverage -> signed distance

void requireImage(std::span<const float> coverage, int width, int height) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("distance field size must be positive");
    if (coverage.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        throw std::invalid_argument("distance field coverage must hold width * height values");
}

float coverageAt(std::span<const float> coverage, int width, int height, int x, int y) {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    const auto value = coverage[static_cast<std::size_t>(y) * width + x];
    return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
}

// One line of the separable transform: out[i] = min_j (i - j)^2 + f[j] and
// the j that attains it (-1 when every f[j] is infinite). The minimum is the
// lower envelope of the parabolas rooted at each finite f[j], built left to
// right in one sweep (Felzenszwalb & Huttenlocher 2012); O(n). Equal
// parabolas keep the earlier root, so the result does not depend on anything
// but the input.
struct EnvelopeScratch {
    std::vector<int> roots;
    std::vector<double> starts;
};

void lowerEnvelope(const double *f, int n, double *out, int *arg, EnvelopeScratch &scratch) {
    scratch.roots.resize(static_cast<std::size_t>(n));
    scratch.starts.resize(static_cast<std::size_t>(n) + 1);
    auto &roots = scratch.roots;
    auto &starts = scratch.starts;
    int top = -1;
    for (int q = 0; q < n; ++q) {
        if (!(f[q] < infinity)) continue;
        if (top < 0) {
            top = 0;
            roots[0] = q;
            starts[0] = -infinity;
            starts[1] = infinity;
            continue;
        }
        // Where the parabola at q starts to undercut the one on top.
        const auto meet = [&](int k) {
            const int v = roots[k];
            return ((f[q] + double(q) * q) - (f[v] + double(v) * v)) / (2.0 * (q - v));
        };
        auto start = meet(top);
        while (start <= starts[top]) start = meet(--top);
        ++top;
        roots[top] = q;
        starts[top] = start;
        starts[top + 1] = infinity;
    }
    if (top < 0) {
        std::fill(out, out + n, infinity);
        std::fill(arg, arg + n, -1);
        return;
    }
    int k = 0;
    for (int q = 0; q < n; ++q) {
        while (starts[k + 1] < q) ++k;
        const int v = roots[k];
        out[q] = double(q - v) * (q - v) + f[v];
        arg[q] = v;
    }
}

// The 2D transform of `f` (rows, then columns): for each pixel, the squared
// distance min over seeds (|p - s|^2 + f(s)) and the seed's pixel index.
struct NearestSeed {
    std::vector<double> squared;
    std::vector<int> seed;
};

NearestSeed transform2d(const std::vector<double> &f, int width, int height) {
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<double> row_out(count);
    std::vector<int> row_arg(count);
    EnvelopeScratch scratch;
    for (int y = 0; y < height; ++y) {
        const auto offset = static_cast<std::size_t>(y) * width;
        lowerEnvelope(f.data() + offset, width, row_out.data() + offset, row_arg.data() + offset, scratch);
    }
    NearestSeed result{std::vector<double>(count), std::vector<int>(count, -1)};
    std::vector<double> column(static_cast<std::size_t>(height));
    std::vector<double> column_out(static_cast<std::size_t>(height));
    std::vector<int> column_arg(static_cast<std::size_t>(height));
    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y) column[y] = row_out[static_cast<std::size_t>(y) * width + x];
        lowerEnvelope(column.data(), height, column_out.data(), column_arg.data(), scratch);
        for (int y = 0; y < height; ++y) {
            const auto index = static_cast<std::size_t>(y) * width + x;
            result.squared[index] = column_out[y];
            const int from_row = column_arg[y];
            if (from_row >= 0)
                result.seed[index] = from_row * width + row_arg[static_cast<std::size_t>(from_row) * width + x];
        }
    }
    return result;
}

// Where a straight edge crosses one pixel, from the pixel's coverage and the
// edge normal. The pixel is the unit square around its centre; `normal`
// points into the shape. Returns the signed distance from the centre to the
// edge (positive when the centre is outside). Covered area as a function of
// that distance is a corner triangle, then a band of slope 1 / |n_major|,
// then the complement of a corner triangle; this inverts it.
double edgeOffsetFromCoverage(double coverage, Point2 normal) {
    auto major = std::abs(normal.x);
    auto minor = std::abs(normal.y);
    if (major < minor) std::swap(major, minor);
    if (minor <= 0.0) return 0.5 - coverage;
    const auto corner = 0.5 * minor / major; // coverage where the corner triangle meets the band
    if (coverage < corner) return 0.5 * (major + minor) - std::sqrt(2.0 * major * minor * coverage);
    if (coverage > 1.0 - corner) return -0.5 * (major + minor) + std::sqrt(2.0 * major * minor * (1.0 - coverage));
    return (0.5 - coverage) * major;
}

// A pixel the edge passes through (or borders): its signed offset to the edge
// and, when the direction is known, the edge point itself.
struct EdgeSeed {
    bool seed = false;
    bool directed = false;
    double offset = 0.0; // signed: positive when the pixel centre is outside
    Point2 point{};      // the subpixel edge point (directed seeds)
};

std::vector<EdgeSeed> edgeSeeds(std::span<const float> coverage, int width, int height) {
    std::vector<EdgeSeed> seeds(static_cast<std::size_t>(width) * height);
    const auto at = [&](int x, int y) { return double(coverageAt(coverage, width, height, x, y)); };
    const double diagonal = std::numbers::sqrt2;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            auto &seed = seeds[static_cast<std::size_t>(y) * width + x];
            const auto value = at(x, y);
            const Point2 centre{x + 0.5, y + 0.5};
            if (value > 0.0 && value < 1.0) {
                // The coverage gradient (an isotropic 3x3 difference) points
                // into the shape and stands in for the edge normal.
                const Point2 gradient{
                    (at(x + 1, y - 1) + diagonal * at(x + 1, y) + at(x + 1, y + 1)) -
                        (at(x - 1, y - 1) + diagonal * at(x - 1, y) + at(x - 1, y + 1)),
                    (at(x - 1, y + 1) + diagonal * at(x, y + 1) + at(x + 1, y + 1)) -
                        (at(x - 1, y - 1) + diagonal * at(x, y - 1) + at(x + 1, y - 1))};
                seed.seed = true;
                if (length(gradient) > 1e-9) {
                    const auto normal = normalized(gradient);
                    seed.offset = edgeOffsetFromCoverage(value, normal);
                    seed.point = centre + seed.offset * normal;
                    seed.directed = true;
                } else {
                    seed.offset = 0.5 - value;
                }
                continue;
            }
            // A full pixel next to a full pixel of the other class: the edge
            // runs along the pixel border, half a pixel away.
            const bool inside = value >= 1.0;
            Point2 towards_other{};
            bool borders = false;
            const int dx[4] = {1, -1, 0, 0};
            const int dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k];
                const int ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const auto other = at(nx, ny);
                if (inside ? other <= 0.0 : other >= 1.0) {
                    borders = true;
                    towards_other = towards_other + Point2{double(dx[k]), double(dy[k])};
                }
            }
            if (!borders) continue;
            seed.seed = true;
            seed.offset = inside ? -0.5 : 0.5;
            if (length(towards_other) > 0.0) {
                const auto step = normalized(towards_other);
                seed.point = centre + 0.5 * step;
                seed.directed = true;
            }
        }
    }
    return seeds;
}

// The distance magnitude from pixel centre `p` to the edge seen by `seed`
// (at pixel `q`); `outside` is the side of p.
double seedDistance(const EdgeSeed &seed, Point2 q, Point2 p, bool outside) {
    if (seed.directed) return length(p - seed.point);
    const auto r = length(p - q);
    return std::max(0.0, outside ? r + seed.offset : r - seed.offset);
}

} // namespace

std::vector<float> signedDistanceFromCoverage(std::span<const float> coverage, int width, int height,
                                              float max_distance, CoverageSeed mode) {
    requireImage(coverage, width, height);
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<float> result(count);
    const auto limit = double(std::max(max_distance, 0.0f));
    const auto store = [&](std::size_t index, double distance) {
        result[index] = static_cast<float>(std::clamp(distance, -limit, limit));
    };

    if (mode == CoverageSeed::linear) {
        // Two transforms, outside and inside; each pixel plants (0.5 - a)
        // on its own side, as a squared height under the parabolas.
        std::vector<double> outer(count), inner(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = double(coverageAt(coverage, width, height, int(i % width), int(i / width)));
            if (value >= 1.0) {
                outer[i] = 0.0;
                inner[i] = infinity;
            } else if (value <= 0.0) {
                outer[i] = infinity;
                inner[i] = 0.0;
            } else {
                outer[i] = std::pow(std::max(0.0, 0.5 - value), 2.0);
                inner[i] = std::pow(std::max(0.0, value - 0.5), 2.0);
            }
        }
        const auto out = transform2d(outer, width, height);
        const auto in = transform2d(inner, width, height);
        for (std::size_t i = 0; i < count; ++i) {
            const auto o = out.squared[i] < infinity ? std::sqrt(out.squared[i]) : limit;
            const auto n = in.squared[i] < infinity ? std::sqrt(in.squared[i]) : limit;
            store(i, o - n);
        }
        return result;
    }

    const auto seeds = edgeSeeds(coverage, width, height);
    std::vector<double> planted(count, infinity);
    for (std::size_t i = 0; i < count; ++i)
        if (seeds[i].seed) planted[i] = 0.0;
    const auto nearest = transform2d(planted, width, height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const auto index = static_cast<std::size_t>(y) * width + x;
            const Point2 p{x + 0.5, y + 0.5};
            const auto &own = seeds[index];
            const bool outside = own.seed ? own.offset > 0.0 : coverageAt(coverage, width, height, x, y) < 0.5;
            const double sign = outside ? 1.0 : -1.0;
            const int found = nearest.seed[index];
            if (found < 0) {
                store(index, sign * limit);
                continue;
            }
            if (mode == CoverageSeed::edge_scalar) {
                if (own.seed) {
                    store(index, own.offset);
                } else {
                    const auto &seed = seeds[static_cast<std::size_t>(found)];
                    const Point2 q{found % width + 0.5, found / width + 0.5};
                    const auto r = length(p - q);
                    store(index, sign * std::max(0.0, outside ? r + seed.offset : r - seed.offset));
                }
                continue;
            }
            const int fx = found % width;
            const int fy = found / width;
            double best = own.seed ? std::abs(own.offset) : infinity;
            if (own.seed && own.directed) best = length(p - own.point);
            if (mode == CoverageSeed::edge_point_single) {
                if (!own.seed) best = seedDistance(seeds[found], Point2{fx + 0.5, fy + 0.5}, p, outside);
                store(index, sign * best);
                continue;
            }
            // edge_point: re-measure against the edge points around the
            // nearest seed (the seed with the nearest centre need not hold
            // the nearest edge point).
            for (int ny = std::max(fy - 1, 0); ny <= std::min(fy + 1, height - 1); ++ny)
                for (int nx = std::max(fx - 1, 0); nx <= std::min(fx + 1, width - 1); ++nx) {
                    const auto &seed = seeds[static_cast<std::size_t>(ny) * width + nx];
                    if (!seed.seed) continue;
                    best = std::min(best, seedDistance(seed, Point2{nx + 0.5, ny + 0.5}, p, outside));
                }
            store(index, sign * best);
        }
    }
    return result;
}

std::vector<float> signedDistanceFromThreshold(std::span<const float> coverage, int width, int height,
                                               float max_distance) {
    requireImage(coverage, width, height);
    const auto count = static_cast<std::size_t>(width) * height;
    std::vector<double> inside(count), outside(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool in = coverageAt(coverage, width, height, int(i % width), int(i / width)) >= 0.5f;
        inside[i] = in ? 0.0 : infinity;
        outside[i] = in ? infinity : 0.0;
    }
    const auto to_inside = transform2d(inside, width, height);
    const auto to_outside = transform2d(outside, width, height);
    const auto limit = double(std::max(max_distance, 0.0f));
    std::vector<float> result(count);
    for (std::size_t i = 0; i < count; ++i) {
        double distance;
        if (inside[i] == 0.0)
            distance = to_outside.squared[i] < infinity ? -(std::sqrt(to_outside.squared[i]) - 0.5) : -limit;
        else
            distance = to_inside.squared[i] < infinity ? std::sqrt(to_inside.squared[i]) - 0.5 : limit;
        result[i] = static_cast<float>(std::clamp(distance, -limit, limit));
    }
    return result;
}

// ---------------------------------------------------------------------------
// (b) outline -> MTSDF

namespace {

enum Channel : std::uint8_t { red = 1, green = 2, blue = 4 };
constexpr std::uint8_t white = red | green | blue;
constexpr std::uint8_t cyan = green | blue;
constexpr std::uint8_t magenta = red | blue;
constexpr std::uint8_t yellow = red | green;

// A line (p0 -> p2; p1 is the midpoint) or a quadratic Bezier.
struct Edge {
    bool quadratic = false;
    Point2 p0, p1, p2;
    std::uint8_t color = white;
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;
};

void computeBounds(Edge &edge) {
    edge.min_x = std::min({edge.p0.x, edge.p1.x, edge.p2.x});
    edge.max_x = std::max({edge.p0.x, edge.p1.x, edge.p2.x});
    edge.min_y = std::min({edge.p0.y, edge.p1.y, edge.p2.y});
    edge.max_y = std::max({edge.p0.y, edge.p1.y, edge.p2.y});
}

Edge makeLine(Point2 a, Point2 b, std::uint8_t color = white) {
    Edge edge{false, a, 0.5 * (a + b), b, color};
    computeBounds(edge);
    return edge;
}

Edge makeQuadratic(Point2 a, Point2 control, Point2 b, std::uint8_t color = white) {
    Edge edge{true, a, control, b, color};
    computeBounds(edge);
    return edge;
}

Point2 pointAt(const Edge &edge, double t) {
    if (t <= 0.0) return edge.p0;
    if (t >= 1.0) return edge.p2;
    if (!edge.quadratic) return edge.p0 + t * (edge.p2 - edge.p0);
    const auto s = 1.0 - t;
    return (s * s) * edge.p0 + (2.0 * s * t) * edge.p1 + (t * t) * edge.p2;
}

Point2 tangentAt(const Edge &edge, double t) {
    if (!edge.quadratic) return edge.p2 - edge.p0;
    const auto d = (1.0 - t) * (edge.p1 - edge.p0) + t * (edge.p2 - edge.p1);
    // A control point on an end point leaves the end tangent zero; the chord
    // is the limit direction there.
    return (d.x == 0.0 && d.y == 0.0) ? edge.p2 - edge.p0 : d;
}

Edge reversed(const Edge &edge) {
    Edge result = edge;
    std::swap(result.p0, result.p2);
    return result;
}

// The part of `edge` between t0 and t1 (the blossom gives the control point).
Edge subEdge(const Edge &edge, double t0, double t1) {
    if (t0 <= 0.0 && t1 >= 1.0) return edge;
    const auto a = pointAt(edge, t0);
    const auto b = pointAt(edge, t1);
    if (!edge.quadratic) return makeLine(a, b, edge.color);
    const auto control = ((1.0 - t0) * (1.0 - t1)) * edge.p0 + ((1.0 - t0) * t1 + t0 * (1.0 - t1)) * edge.p1 +
                         (t0 * t1) * edge.p2;
    return makeQuadratic(a, control, b, edge.color);
}

double boundsDistance(const Edge &edge, Point2 p) {
    const auto dx = std::max({edge.min_x - p.x, 0.0, p.x - edge.max_x});
    const auto dy = std::max({edge.min_y - p.y, 0.0, p.y - edge.max_y});
    return std::sqrt(dx * dx + dy * dy);
}

// ---- winding (nonzero) ----

struct Crossing {
    double x;
    int direction;
};

// Where the horizontal line at `y` crosses `edge`. Each y-monotonic piece
// counts when its end points lie on different sides of y under the rule
// "y_end <= y", so a vertex shared by two pieces is counted once.
void appendCrossings(const Edge &edge, double y, std::vector<Crossing> &out) {
    if (y < edge.min_y || y > edge.max_y) return;
    if (!edge.quadratic) {
        const auto y0 = edge.p0.y;
        const auto y1 = edge.p2.y;
        if ((y0 <= y) == (y1 <= y)) return;
        const auto t = (y - y0) / (y1 - y0);
        out.push_back({edge.p0.x + t * (edge.p2.x - edge.p0.x), y1 > y0 ? 1 : -1});
        return;
    }
    const auto a = edge.p0.y - 2.0 * edge.p1.y + edge.p2.y;
    const auto b = 2.0 * (edge.p1.y - edge.p0.y);
    const auto c = edge.p0.y - y;
    double splits[3] = {0.0, 1.0, 1.0};
    int pieces = 1;
    if (a != 0.0) {
        const auto extremum = -b / (2.0 * a);
        if (extremum > 0.0 && extremum < 1.0) {
            splits[1] = extremum;
            pieces = 2;
        }
    }
    for (int k = 0; k < pieces; ++k) {
        const auto t0 = splits[k];
        const auto t1 = splits[k + 1];
        const auto y0 = pointAt(edge, t0).y;
        const auto y1 = pointAt(edge, t1).y;
        if ((y0 <= y) == (y1 <= y)) continue;
        double t;
        if (std::abs(a) < 1e-12 * (std::abs(b) + std::abs(c) + 1e-300)) {
            t = -c / b;
        } else {
            const auto disc = std::max(0.0, b * b - 4.0 * a * c);
            const auto q = -0.5 * (b + std::copysign(std::sqrt(disc), b));
            const auto r0 = q / a;
            const auto r1 = q != 0.0 ? c / q : r0;
            const auto miss = [&](double r) { return std::max({t0 - r, 0.0, r - t1}); };
            t = miss(r0) <= miss(r1) ? r0 : r1;
        }
        t = std::clamp(t, t0, t1);
        out.push_back({pointAt(edge, t).x, y1 > y0 ? 1 : -1});
    }
}

bool filledAt(const std::vector<Edge> &edges, Point2 p) {
    int winding = 0;
    std::vector<Crossing> crossings;
    for (const auto &edge : edges) {
        if (edge.max_x < p.x) continue;
        crossings.clear();
        appendCrossings(edge, p.y, crossings);
        for (const auto &crossing : crossings)
            if (crossing.x > p.x) winding += crossing.direction;
    }
    return winding != 0;
}

// Crossings of a whole outline with one scanline, sorted by x.
void scanline(const std::vector<Edge> &edges, double y, std::vector<Crossing> &out) {
    out.clear();
    for (const auto &edge : edges) appendCrossings(edge, y, out);
    std::sort(out.begin(), out.end(), [](const Crossing &a, const Crossing &b) {
        return a.x != b.x ? a.x < b.x : a.direction < b.direction;
    });
}

// ---- distance to one edge ----

struct EdgeHit {
    double distance = infinity; // signed: negative on the filled (left) side
    double alignment = 1.0;     // |cos| between the edge tangent and the way to p, at an end point
    double t = 0.0;
};

bool closer(const EdgeHit &a, const EdgeHit &b) {
    const auto da = std::abs(a.distance);
    const auto db = std::abs(b.distance);
    return da < db || (da == db && a.alignment < b.alignment);
}

// Real roots of c3 t^3 + c2 t^2 + c1 t + c0 (at most 3), polished by Newton.
int solveCubic(double c3, double c2, double c1, double c0, double roots[3]) {
    const auto scale = std::abs(c2) + std::abs(c1) + std::abs(c0);
    int count = 0;
    if (std::abs(c3) <= 1e-12 * scale) {
        if (std::abs(c2) <= 1e-12 * (std::abs(c1) + std::abs(c0))) {
            if (c1 != 0.0) roots[count++] = -c0 / c1;
        } else {
            const auto disc = c1 * c1 - 4.0 * c2 * c0;
            if (disc >= 0.0) {
                const auto q = -0.5 * (c1 + std::copysign(std::sqrt(disc), c1));
                roots[count++] = q / c2;
                if (q != 0.0) roots[count++] = c0 / q;
            }
        }
        return count;
    }
    const auto a = c2 / c3, b = c1 / c3, c = c0 / c3;
    const auto q = (a * a - 3.0 * b) / 9.0;
    const auto r = (2.0 * a * a * a - 9.0 * a * b + 27.0 * c) / 54.0;
    const auto q3 = q * q * q;
    if (r * r < q3) {
        const auto theta = std::acos(std::clamp(r / std::sqrt(q3), -1.0, 1.0));
        const auto m = -2.0 * std::sqrt(q);
        roots[count++] = m * std::cos(theta / 3.0) - a / 3.0;
        roots[count++] = m * std::cos((theta + 2.0 * std::numbers::pi) / 3.0) - a / 3.0;
        roots[count++] = m * std::cos((theta - 2.0 * std::numbers::pi) / 3.0) - a / 3.0;
    } else {
        const auto big = -std::copysign(std::cbrt(std::abs(r) + std::sqrt(r * r - q3)), r);
        const auto small = big != 0.0 ? q / big : 0.0;
        roots[count++] = big + small - a / 3.0;
        if (std::abs(big - small) <= 1e-12 * std::abs(big + small)) roots[count++] = -0.5 * (big + small) - a / 3.0;
    }
    for (int k = 0; k < count; ++k)
        for (int step = 0; step < 2; ++step) {
            const auto t = roots[k];
            const auto value = ((c3 * t + c2) * t + c1) * t + c0;
            const auto slope = (3.0 * c3 * t + 2.0 * c2) * t + c1;
            if (slope != 0.0) roots[k] = t - value / slope;
        }
    return count;
}

EdgeHit endHit(const Edge &edge, Point2 p, double t) {
    const auto at = pointAt(edge, t);
    const auto to_p = p - at;
    const auto tangent = tangentAt(edge, t);
    const auto distance = length(to_p);
    EdgeHit hit;
    hit.t = t;
    hit.distance = cross(tangent, to_p) > 0.0 ? -distance : distance;
    hit.alignment = distance > 0.0 ? std::abs(dot(normalized(tangent), normalized(to_p))) : 0.0;
    return hit;
}

EdgeHit hitEdge(const Edge &edge, Point2 p) {
    if (!edge.quadratic) {
        const auto ab = edge.p2 - edge.p0;
        const auto denominator = dot(ab, ab);
        const auto t = denominator > 0.0 ? dot(p - edge.p0, ab) / denominator : 0.0;
        if (t <= 0.0) return endHit(edge, p, 0.0);
        if (t >= 1.0) return endHit(edge, p, 1.0);
        const auto to_p = p - pointAt(edge, t);
        const auto distance = length(to_p);
        return {cross(ab, to_p) > 0.0 ? -distance : distance, 0.0, t};
    }
    // d/dt |B(t) - p|^2 = 0 with B(t) - p = q + 2 t a + t^2 b.
    const auto a = edge.p1 - edge.p0;
    const auto b = edge.p2 - 2.0 * edge.p1 + edge.p0;
    const auto q = edge.p0 - p;
    double roots[3];
    const int count = solveCubic(dot(b, b), 3.0 * dot(a, b), 2.0 * dot(a, a) + dot(q, b), dot(q, a), roots);
    auto best = endHit(edge, p, 0.0);
    if (const auto end = endHit(edge, p, 1.0); closer(end, best)) best = end;
    for (int k = 0; k < count; ++k) {
        const auto t = roots[k];
        if (!(t > 0.0 && t < 1.0)) continue;
        const auto to_p = p - pointAt(edge, t);
        const auto distance = length(to_p);
        if (distance < std::abs(best.distance))
            best = {cross(tangentAt(edge, t), to_p) > 0.0 ? -distance : distance, 0.0, t};
    }
    return best;
}

// Chlumsky's pseudo-distance: past an end point, the distance to the
// tangent line there when that is nearer.
double pseudoDistance(const Edge &edge, const EdgeHit &hit, Point2 p) {
    auto distance = hit.distance;
    if (hit.t <= 0.0 || hit.t >= 1.0) {
        const bool start = hit.t <= 0.0;
        const auto direction = normalized(tangentAt(edge, start ? 0.0 : 1.0));
        const auto to_p = p - (start ? edge.p0 : edge.p2);
        const auto along = dot(to_p, direction);
        if (start ? along < 0.0 : along > 0.0) {
            const auto pseudo = -cross(direction, to_p);
            if (std::abs(pseudo) <= std::abs(distance)) distance = pseudo;
        }
    }
    return distance;
}

// ---- preparing the outline ----

struct Contour {
    std::vector<Edge> edges;
};

void splitCubic(Point2 p0, Point2 p1, Point2 p2, Point2 p3, double tolerance, int depth, std::vector<Edge> &out) {
    // The quadratic sharing both end points and the mid tangent; its distance
    // from the cubic is at most sqrt(3)/36 |p3 - 3 p2 + 3 p1 - p0|.
    const auto error = std::sqrt(3.0) / 36.0 * length(p3 - 3.0 * p2 + 3.0 * p1 - p0);
    if (error <= tolerance || depth >= 16) {
        out.push_back(makeQuadratic(p0, 0.25 * (3.0 * (p1 + p2) - p0 - p3), p3));
        return;
    }
    const auto p01 = 0.5 * (p0 + p1), p12 = 0.5 * (p1 + p2), p23 = 0.5 * (p2 + p3);
    const auto p012 = 0.5 * (p01 + p12), p123 = 0.5 * (p12 + p23);
    const auto mid = 0.5 * (p012 + p123);
    splitCubic(p0, p01, p012, mid, tolerance, depth + 1, out);
    splitCubic(mid, p123, p23, p3, tolerance, depth + 1, out);
}

Point2 applyTransform(const Affine2 &m, Point2 p) {
    return {m.xx * p.x + m.xy * p.y + m.x0, m.yx * p.x + m.yy * p.y + m.y0};
}

std::vector<Contour> contoursOf(const Outline &outline, const Affine2 &transform, double tolerance) {
    std::vector<Contour> contours;
    for (const auto &source : outline.contours) {
        Contour contour;
        for (const auto &segment : source) {
            const auto p = [&](int k) { return applyTransform(transform, segment.points[k]); };
            switch (segment.kind) {
            case SegmentKind::line:
                if (!samePoint(p(0), p(1))) contour.edges.push_back(makeLine(p(0), p(1)));
                break;
            case SegmentKind::quadratic:
                if (!(samePoint(p(0), p(1)) && samePoint(p(1), p(2))))
                    contour.edges.push_back(makeQuadratic(p(0), p(1), p(2)));
                break;
            case SegmentKind::cubic:
                if (!(samePoint(p(0), p(1)) && samePoint(p(1), p(2)) && samePoint(p(2), p(3))))
                    splitCubic(p(0), p(1), p(2), p(3), tolerance, 0, contour.edges);
                break;
            }
        }
        if (contour.edges.empty()) continue;
        // Close the contour when the source left it open.
        if (!samePoint(contour.edges.back().p2, contour.edges.front().p0))
            contour.edges.push_back(makeLine(contour.edges.back().p2, contour.edges.front().p0));
        contours.push_back(std::move(contour));
    }
    return contours;
}

std::vector<Edge> allEdges(const std::vector<Contour> &contours) {
    std::vector<Edge> edges;
    for (const auto &contour : contours) edges.insert(edges.end(), contour.edges.begin(), contour.edges.end());
    return edges;
}

// ---- edge colouring (Chlumsky's Algorithm 1, simple variant) ----

bool isCorner(Point2 incoming, Point2 outgoing, double cross_threshold) {
    const auto a = normalized(incoming);
    const auto b = normalized(outgoing);
    return dot(a, b) <= 0.0 || std::abs(cross(a, b)) > cross_threshold;
}

void colorContour(Contour &contour, double corner_angle) {
    auto &edges = contour.edges;
    const auto threshold = std::sin(corner_angle);
    std::vector<std::size_t> corners;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const auto &previous = edges[(i + edges.size() - 1) % edges.size()];
        if (isCorner(tangentAt(previous, 1.0), tangentAt(edges[i], 0.0), threshold)) corners.push_back(i);
    }
    if (corners.empty()) {
        for (auto &edge : edges) edge.color = white;
        return;
    }
    if (corners.size() == 1) {
        // A teardrop: the two edges at the corner share one channel, the
        // middle of the contour is white. Needs three edges; split if fewer.
        if (edges.size() < 3) {
            std::vector<Edge> split;
            for (const auto &edge : edges)
                for (int k = 0; k < 3; ++k) split.push_back(subEdge(edge, k / 3.0, (k + 1) / 3.0));
            edges = std::move(split);
            corners[0] *= 3;
        }
        const auto m = edges.size();
        for (std::size_t i = 0; i < m; ++i) {
            const auto third = (3 * i) / m; // 0, 1, 2 along the contour from the corner
            edges[(corners[0] + i) % m].color = third == 0 ? cyan : third == 1 ? white : yellow;
        }
        return;
    }
    // Several corners: each run between corners takes the next colour of
    // cyan, magenta, yellow; the last run avoids both its neighbours.
    const std::uint8_t cycle[3] = {cyan, magenta, yellow};
    const auto runs = corners.size();
    std::vector<std::uint8_t> run_color(runs);
    for (std::size_t k = 0; k < runs; ++k) run_color[k] = cycle[k % 3];
    if (runs > 1 && run_color[runs - 1] == run_color[0]) {
        for (const auto candidate : cycle)
            if (candidate != run_color[0] && candidate != run_color[runs - 2]) {
                run_color[runs - 1] = candidate;
                break;
            }
    }
    std::size_t run = 0;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const auto index = (corners[0] + i) % edges.size();
        if (run + 1 < runs && index == corners[run + 1]) ++run;
        edges[index].color = run_color[run];
    }
}

// ---- removing edges inside the fill ----

// +1: filled on the left only; -1: on the right only; 0: both or neither.
int sideStatus(const std::vector<Edge> &all, const Edge &edge, double t) {
    const auto at = pointAt(edge, t);
    const auto normal = normalized(Point2{-tangentAt(edge, t).y, tangentAt(edge, t).x});
    const double step = 1e-5 * std::max(1.0, std::abs(at.x) + std::abs(at.y));
    const bool left = filledAt(all, at + step * normal);
    const bool right = filledAt(all, at - step * normal);
    return left == right ? 0 : (left ? 1 : -1);
}

// The pieces of every edge that separate filled from empty, each turned so
// the fill is on its left. Pieces where both sides (or neither) are filled
// -- overlapping contours, coincident edges -- are dropped.
std::vector<Edge> boundaryEdges(const std::vector<Edge> &all, int &removed) {
    std::vector<Edge> result;
    for (const auto &edge : all) {
        const auto extent = std::max(edge.max_x - edge.min_x, edge.max_y - edge.min_y);
        // One side test per pixel of extent; an overlap shorter than that
        // can be missed (it then shows as a thin seam inside the fill).
        const int samples = std::clamp(int(std::ceil(extent)), 2, 64);
        std::vector<int> status(static_cast<std::size_t>(samples));
        for (int i = 0; i < samples; ++i) status[i] = sideStatus(all, edge, (i + 0.5) / samples);
        double start = 0.0;
        int current = status[0];
        const auto emit = [&](double t0, double t1, int state) {
            if (state == 0) {
                ++removed;
                return;
            }
            const auto piece = subEdge(edge, t0, t1);
            result.push_back(state > 0 ? piece : reversed(piece));
        };
        for (int i = 1; i < samples; ++i) {
            if (status[i] == current) continue;
            double lo = (i - 0.5) / samples, hi = (i + 0.5) / samples;
            for (int step = 0; step < 40; ++step) {
                const auto mid = 0.5 * (lo + hi);
                (sideStatus(all, edge, mid) == current ? lo : hi) = mid;
            }
            const auto cut = 0.5 * (lo + hi);
            emit(start, cut, current);
            start = cut;
            current = status[i];
        }
        emit(start, 1.0, current);
    }
    return result;
}

// Fill of each pixel centre of a row (nonzero winding).
void rowFill(const std::vector<Edge> &edges, int width, double y, std::vector<Crossing> &crossings,
             std::vector<char> &fill) {
    scanline(edges, y, crossings);
    fill.assign(static_cast<std::size_t>(width), 0);
    std::size_t next = 0;
    int winding_left = 0;
    for (int x = 0; x < width; ++x) {
        const auto px = x + 0.5;
        while (next < crossings.size() && crossings[next].x <= px) winding_left += crossings[next++].direction;
        // Every contour is closed, so the winding to the right is minus the
        // winding to the left.
        fill[x] = winding_left != 0;
    }
}

float median3(float a, float b, float c) { return std::max(std::min(a, b), std::min(std::max(a, b), c)); }

struct Prepared {
    std::vector<Edge> original; // every edge, for winding
    std::vector<Edge> boundary; // the pieces that bound the fill, measured for distance
    int contours = 0;
    int removed = 0;
};

Prepared prepare(const Outline &outline, const Affine2 &transform, const MtsdfOptions &options) {
    auto contours = contoursOf(outline, transform, options.cubic_tolerance);
    for (auto &contour : contours) colorContour(contour, options.corner_angle);
    Prepared prepared;
    prepared.contours = int(contours.size());
    prepared.original = allEdges(contours);
    if (options.remove_overlaps) prepared.boundary = boundaryEdges(prepared.original, prepared.removed);
    else prepared.boundary = prepared.original;
    return prepared;
}

double trueDistance(const std::vector<Edge> &boundary, Point2 p) {
    EdgeHit best;
    for (const auto &edge : boundary) {
        if (boundsDistance(edge, p) > std::abs(best.distance)) continue;
        if (const auto hit = hitEdge(edge, p); closer(hit, best)) best = hit;
    }
    return std::abs(best.distance);
}

// Bilinear RGBA at a point given in texel units (texel centres at +0.5).
void sampleBilinear(const std::vector<float> &field, int width, int height, double u, double v, float out[4]) {
    const auto fx = u - 0.5, fy = v - 0.5;
    const auto x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
    const auto tx = float(fx - x0), ty = float(fy - y0);
    const auto texel = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return field.data() + (static_cast<std::size_t>(y) * width + x) * 4;
    };
    const auto *a = texel(x0, y0);
    const auto *b = texel(x0 + 1, y0);
    const auto *c = texel(x0, y0 + 1);
    const auto *d = texel(x0 + 1, y0 + 1);
    for (int k = 0; k < 4; ++k) {
        const auto top = a[k] + (b[k] - a[k]) * tx;
        const auto bottom = c[k] + (d[k] - c[k]) * tx;
        out[k] = top + (bottom - top) * ty;
    }
}

// Error correction. Between texel centres the shader interpolates RGBA and
// takes d = clamp(median, a - t, a + t). A sample point inside a cell (the
// square between four texel centres) is a clash when that d puts it on the
// wrong side of the exact outline by more than `clash_tolerance` texels while
// the interpolated A alone puts it on the right side: the median is at fault
// and replacing RGB by A fixes it. Where A is wrong too, the cell is too
// coarse for the shape (a stroke under about one texel) and nothing is
// replaced; those samples are counted as unrepresentable. In each cell with a
// clash, the texel whose replacement removes the most clashes is replaced,
// until the cell is clean (replacing all four always is).
constexpr int samples_per_side = 4;
constexpr double clash_tolerance = 0.1;

struct CellSample {
    Point2 point;
    bool filled = false;
    bool a_wrong = false;
    double distance = -1.0; // exact |d|, computed on first need
};

bool wrongSide(bool filled, float d) { return filled ? d > 0.0f : d < 0.0f; }

int cellClashes(const std::vector<float> &field, int width, int height, float clamp_width,
                std::vector<CellSample> &samples, const Prepared &prepared) {
    int clashes = 0;
    for (auto &sample : samples) {
        if (sample.a_wrong) continue;
        float rgba[4];
        sampleBilinear(field, width, height, sample.point.x, sample.point.y, rgba);
        if (!wrongSide(sample.filled, reconstructMtsdf(rgba, clamp_width))) continue;
        if (sample.distance < 0.0) sample.distance = trueDistance(prepared.boundary, sample.point);
        if (sample.distance > clash_tolerance) ++clashes;
    }
    return clashes;
}

void correctErrors(std::vector<float> &field, int width, int height, float clamp_width, const Prepared &prepared,
                   bool verify, MtsdfStats &stats) {
    std::vector<char> replaced(static_cast<std::size_t>(width) * height, 0);
    const auto replace = [&](std::size_t index) {
        auto *texel = field.data() + index * 4;
        texel[0] = texel[1] = texel[2] = texel[3];
        replaced[index] = 1;
    };
    const auto cell_may_clash = [&](int cx, int cy) {
        // With all four A on one side and farther than t, the clamp keeps
        // every sample on A's side.
        float nearest = std::numeric_limits<float>::infinity();
        bool positive = false, negative = false;
        for (int k = 0; k < 4; ++k) {
            const auto a = field[(static_cast<std::size_t>(cy + k / 2) * width + cx + k % 2) * 4 + 3];
            nearest = std::min(nearest, std::abs(a));
            (a > 0.0f ? positive : negative) = true;
        }
        return (positive && negative) || nearest <= std::max(clamp_width, 0.0f);
    };
    std::vector<CellSample> samples;
    const auto cell_samples = [&](int cx, int cy) {
        samples.clear();
        for (int sy = 0; sy < samples_per_side; ++sy)
            for (int sx = 0; sx < samples_per_side; ++sx) {
                CellSample sample;
                sample.point = {cx + 0.5 + (sx + 0.5) / samples_per_side, cy + 0.5 + (sy + 0.5) / samples_per_side};
                sample.filled = filledAt(prepared.original, sample.point);
                float rgba[4];
                sampleBilinear(field, width, height, sample.point.x, sample.point.y, rgba);
                if (wrongSide(sample.filled, rgba[3])) {
                    sample.distance = trueDistance(prepared.boundary, sample.point);
                    sample.a_wrong = true;
                }
                samples.push_back(sample);
            }
    };
    for (int cy = 0; cy + 1 < height; ++cy)
        for (int cx = 0; cx + 1 < width; ++cx) {
            if (!cell_may_clash(cx, cy)) continue;
            cell_samples(cx, cy);
            auto clashes = cellClashes(field, width, height, clamp_width, samples, prepared);
            while (clashes > 0) {
                // Fewest clashes after the replacement wins; when none
                // helps alone, the first one still goes (four always do).
                int best_texel = -1;
                int best_clashes = std::numeric_limits<int>::max();
                for (int k = 0; k < 4; ++k) {
                    const auto index = static_cast<std::size_t>(cy + k / 2) * width + cx + k % 2;
                    if (replaced[index]) continue;
                    float saved[3];
                    std::copy_n(field.data() + index * 4, 3, saved);
                    auto *texel = field.data() + index * 4;
                    texel[0] = texel[1] = texel[2] = texel[3];
                    const auto after = cellClashes(field, width, height, clamp_width, samples, prepared);
                    std::copy_n(saved, 3, texel);
                    if (after < best_clashes) {
                        best_clashes = after;
                        best_texel = k;
                    }
                }
                if (best_texel < 0) break;
                replace(static_cast<std::size_t>(cy + best_texel / 2) * width + cx + best_texel % 2);
                ++stats.corrected_texels;
                clashes = best_clashes;
            }
        }
    // A replacement can disturb a cell met earlier; count what is left.
    if (!verify) return;
    for (int cy = 0; cy + 1 < height; ++cy)
        for (int cx = 0; cx + 1 < width; ++cx) {
            if (!cell_may_clash(cx, cy)) continue;
            cell_samples(cx, cy);
            stats.remaining_clashes += cellClashes(field, width, height, clamp_width, samples, prepared);
            for (const auto &sample : samples)
                if (sample.a_wrong && sample.distance > clash_tolerance) ++stats.unrepresentable_samples;
        }
}

} // namespace

float reconstructMtsdf(const float *rgba, float t) {
    const auto median = median3(rgba[0], rgba[1], rgba[2]);
    if (t < 0.0f) return median;
    if (t == 0.0f) return rgba[3];
    return std::clamp(median, rgba[3] - t, rgba[3] + t);
}

float reconstructMtsdfNear(const float *rgba, float t, float near) {
    const auto x = std::clamp((std::abs(rgba[3]) - near) / std::max(near, 1e-6f), 0.0f, 1.0f);
    const auto s = x * x * (3.0f - 2.0f * x);
    return reconstructMtsdf(rgba, t) * (1.0f - s) + rgba[3] * s;
}

std::vector<float> multiChannelDistanceField(const Outline &outline, int width, int height, const Affine2 &transform,
                                             float range, const MtsdfOptions &options, MtsdfStats *stats_out) {
    if (width <= 0 || height <= 0) throw std::invalid_argument("distance field size must be positive");
    if (!(range > 0.0f)) throw std::invalid_argument("distance field range must be positive");
    if (!(options.cubic_tolerance > 0.0)) throw std::invalid_argument("cubic tolerance must be positive");

    const auto prepared = prepare(outline, transform, options);
    const auto &edges = prepared.boundary;
    MtsdfStats stats;
    stats.contours = prepared.contours;
    stats.edges = int(edges.size());
    stats.removed_pieces = prepared.removed;

    const auto limit = double(range);
    std::vector<float> field(static_cast<std::size_t>(width) * height * 4);
    std::vector<Crossing> crossings;
    std::vector<char> fill;
    // The edges that won the previous pixel are measured first; that makes
    // the bounding-box cull below effective. The cull is exact (an edge is
    // skipped only when its box is farther than every current best it could
    // replace), so the order changes only which of two exactly equal hits wins.
    int hint[4] = {-1, -1, -1, -1};
    for (int y = 0; y < height; ++y) {
        rowFill(prepared.original, width, y + 0.5, crossings, fill);
        for (int x = 0; x < width; ++x) {
            const Point2 p{x + 0.5, y + 0.5};
            EdgeHit best_true;
            EdgeHit best[3];
            int best_edge[3] = {-1, -1, -1};
            int true_edge = -1;
            const auto consider = [&](int e) {
                const auto &edge = edges[static_cast<std::size_t>(e)];
                const auto bound = boundsDistance(edge, p);
                bool useful = bound <= std::abs(best_true.distance);
                for (int c = 0; c < 3 && !useful; ++c)
                    if ((edge.color & (1 << c)) && bound <= std::abs(best[c].distance)) useful = true;
                if (!useful) return;
                const auto hit = hitEdge(edge, p);
                if (closer(hit, best_true)) {
                    best_true = hit;
                    true_edge = e;
                }
                for (int c = 0; c < 3; ++c)
                    if ((edge.color & (1 << c)) && closer(hit, best[c])) {
                        best[c] = hit;
                        best_edge[c] = e;
                    }
            };
            for (const int e : hint)
                if (e >= 0) consider(e);
            for (int e = 0; e < int(edges.size()); ++e)
                if (e != hint[0] && e != hint[1] && e != hint[2] && e != hint[3]) consider(e);
            hint[0] = true_edge;
            for (int c = 0; c < 3; ++c) hint[c + 1] = best_edge[c] != true_edge ? best_edge[c] : -1;

            const bool filled = fill[x] != 0;
            auto *texel = field.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            const double inside_sign = filled ? -1.0 : 1.0;
            double channel[3];
            for (int c = 0; c < 3; ++c)
                channel[c] = best_edge[c] < 0
                                 ? inside_sign * limit
                                 : pseudoDistance(edges[static_cast<std::size_t>(best_edge[c])], best[c], p);
            // The winding decides the side; a median that disagrees is
            // flipped as a whole (the correction pass cleans up after it).
            const auto median = std::max(std::min(channel[0], channel[1]),
                                         std::min(std::max(channel[0], channel[1]), channel[2]));
            if (median != 0.0 && (median < 0.0) != filled)
                for (auto &value : channel) value = -value;
            const auto exact = true_edge < 0 ? limit : std::abs(best_true.distance);
            for (int c = 0; c < 3; ++c) texel[c] = static_cast<float>(std::clamp(channel[c], -limit, limit));
            texel[3] = static_cast<float>(std::clamp(inside_sign * exact, -limit, limit));
        }
    }
    if (options.correct_errors)
        correctErrors(field, width, height, options.clamp_width, prepared, options.verify_correction, stats);
    if (stats_out != nullptr) *stats_out = stats;
    return field;
}

Outline transformOutline(const Outline &outline, const Affine2 &transform, double cubic_tolerance) {
    Outline result;
    for (const auto &contour : contoursOf(outline, transform, cubic_tolerance)) {
        std::vector<OutlineSegment> segments;
        for (const auto &edge : contour.edges) {
            OutlineSegment segment;
            segment.kind = edge.quadratic ? SegmentKind::quadratic : SegmentKind::line;
            segment.points[0] = edge.p0;
            segment.points[1] = edge.quadratic ? edge.p1 : edge.p2;
            segment.points[2] = edge.p2;
            segments.push_back(segment);
        }
        result.contours.push_back(std::move(segments));
    }
    return result;
}

std::vector<float> outlineCoverage(const Outline &pixel_outline, int width, int height, int subsamples) {
    if (width <= 0 || height <= 0 || subsamples <= 0) throw std::invalid_argument("coverage size must be positive");
    const auto edges = allEdges(contoursOf(pixel_outline, Affine2{}, 1e-3));
    std::vector<float> coverage(static_cast<std::size_t>(width) * height, 0.0f);
    std::vector<double> row(static_cast<std::size_t>(width));
    std::vector<Crossing> crossings;
    const double weight = 1.0 / subsamples;
    for (int y = 0; y < height; ++y) {
        std::fill(row.begin(), row.end(), 0.0);
        for (int s = 0; s < subsamples; ++s) {
            scanline(edges, y + (s + 0.5) * weight, crossings);
            int winding = 0;
            for (std::size_t k = 0; k + 1 < crossings.size(); ++k) {
                winding += crossings[k].direction;
                if (winding == 0) continue;
                // [x0, x1) is filled; spread its length over the pixels.
                const auto x0 = std::clamp(crossings[k].x, 0.0, double(width));
                const auto x1 = std::clamp(crossings[k + 1].x, 0.0, double(width));
                for (int x = int(std::floor(x0)); x < width && x < x1; ++x) {
                    const auto overlap = std::min(x1, x + 1.0) - std::max(x0, double(x));
                    if (overlap > 0.0) row[x] += overlap * weight;
                }
            }
        }
        for (int x = 0; x < width; ++x)
            coverage[static_cast<std::size_t>(y) * width + x] = static_cast<float>(std::clamp(row[x], 0.0, 1.0));
    }
    return coverage;
}

struct OutlineDistance::State {
    Prepared prepared;
};

OutlineDistance::OutlineDistance(const Outline &pixel_outline) : state(std::make_shared<State>()) {
    MtsdfOptions options;
    options.cubic_tolerance = 1e-3;
    state->prepared = prepare(pixel_outline, Affine2{}, options);
}

double OutlineDistance::signedDistance(Point2 point) const {
    const auto distance = trueDistance(state->prepared.boundary, point);
    return filledAt(state->prepared.original, point) ? -distance : distance;
}

int OutlineDistance::removedPieces() const { return state->prepared.removed; }

} // namespace Pelican
