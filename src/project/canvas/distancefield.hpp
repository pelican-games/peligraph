#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Pelican {

// DF-1: distance fields for pelican.canvas, built in house (no external
// library); CV-4 bakes shape.text's glyph atlas with (b). Two producers share one sign convention: a distance is in pixels
// of the output image, positive outside the shape and negative inside, and
// pixel (x, y) is sampled at its centre (x + 0.5, y + 0.5). Row 0 is the first
// row of the image. Both producers are deterministic: the same input gives the
// same bytes, and nothing depends on thread count or timing.
//
// (a) signedDistanceFromCoverage: an anti-aliased coverage image (0 = empty,
//     1 = full) to a signed distance field. Edge pixels get a subpixel
//     estimate of where the edge crosses them (from the coverage value and the
//     local gradient, as in Gustavson & Strand's anti-aliased EDT), and the
//     lower envelope of parabolas (Felzenszwalb & Huttenlocher) carries the
//     nearest edge pixel to every other pixel. CoverageSeed picks how the edge
//     estimate is planted; `edge_point` is the default and the most accurate.
//
// (b) multiChannelDistanceField: an outline to an MTSDF (RGB = the
//     multi-channel pseudo-distances of Chlumsky's MSDF, A = the true signed
//     distance). Inside/outside comes from the nonzero winding number, not
//     from edge orientation, so overlapping contours (CJK glyphs built from
//     overlapping strokes) give one clean silhouette: edge pieces that lie
//     inside the filled area are removed before any distance is measured.
//     Reconstruct with reconstructMtsdf (the shader does the same).

enum class CoverageSeed : std::uint8_t {
    linear,      // seed = 0.5 - coverage, as a scalar (the plain TinySDF seed)
    edge_scalar, // seed = the gradient-aware subpixel edge distance, as a scalar
    edge_point_single, // seed = the subpixel edge point (a 2D offset) of the
                       // nearest seed pixel only
    edge_point,        // as edge_point_single, then the 3x3 seeds around the
                       // nearest one are re-measured and the nearest edge
                       // point wins (the refinement step)
};

// `coverage` is width * height values, row-major. Distances are clamped to
// [-max_distance, max_distance]. Throws std::invalid_argument on a size
// mismatch or a non-positive size.
std::vector<float> signedDistanceFromCoverage(std::span<const float> coverage, int width, int height,
                                              float max_distance, CoverageSeed seed = CoverageSeed::edge_point);

// Reference for (a): threshold at 0.5, then the exact Euclidean distance
// between pixel centres of the two classes, minus half a pixel.
std::vector<float> signedDistanceFromThreshold(std::span<const float> coverage, int width, int height,
                                               float max_distance);

struct Point2 {
    double x = 0.0;
    double y = 0.0;
};

enum class SegmentKind : std::uint8_t { line, quadratic, cubic };

// One edge of a contour. `points` holds 2 (line), 3 (quadratic) or 4 (cubic)
// control points; the first is the end of the previous segment.
struct OutlineSegment {
    SegmentKind kind = SegmentKind::line;
    std::array<Point2, 4> points{};
};

// Closed contours. The fill rule is nonzero; contour direction decides the
// winding only, never the sign of a distance on its own.
struct Outline {
    std::vector<std::vector<OutlineSegment>> contours;
};

// Outline coordinates to output pixels: pixel = (xx * x + xy * y + x0, yx * x + yy * y + y0).
// A font (y up) placed into an image (row 0 on top) uses a negative yy.
struct Affine2 {
    double xx = 1.0, xy = 0.0, x0 = 0.0;
    double yx = 0.0, yy = 1.0, y0 = 0.0;
};

struct MtsdfOptions {
    // Maximum distance of a cubic's quadratic approximation, in output pixels.
    double cubic_tolerance = 0.01;
    // Two edges meet at a corner when the turn between them exceeds this
    // angle (radians). msdfgen's default is 3.0.
    double corner_angle = 3.0;
    // Error correction: find texels whose bilinear median disagrees with the
    // exact shape, and replace their RGB by A.
    bool correct_errors = true;
    // The clamp half-width t of reconstructMtsdf the correction checks for.
    float clamp_width = 0.5f;
    // Count what the correction left (a second pass over the field; stats only).
    bool verify_correction = false;
    // Drop edge pieces with fill on both sides (overlapping contours). Off
    // only to measure what it costs, or for outlines known to be clean.
    bool remove_overlaps = true;
};

struct MtsdfStats {
    int contours = 0;            // after cubic splitting, before trimming
    int edges = 0;               // edges measured (after trimming and splitting)
    int removed_pieces = 0;      // edge pieces dropped because both sides were filled or empty
    int corrected_texels = 0;    // texels whose RGB was replaced by A
    int remaining_clashes = 0;   // with verify_correction: clash samples left after correction
    int unrepresentable_samples = 0; // with verify_correction: samples A itself puts on the wrong side
};

// Returns width * height * 4 floats (RGBA), row-major; distances in output
// pixels, clamped to [-range, range].
std::vector<float> multiChannelDistanceField(const Outline &outline, int width, int height, const Affine2 &transform,
                                             float range, const MtsdfOptions &options = {},
                                             MtsdfStats *stats = nullptr);

// d = clamp(median(r, g, b), a - t, a + t). t <= 0 returns plain a; a negative
// t returns the plain median (no clamp).
float reconstructMtsdf(const float *rgba, float t);
// reconstructMtsdf near the outline, A alone away from it: the clamped median
// up to |a| = near, A from 2 * near, smoothstep between. Corners stay sharp
// where the fill and thin outlines are; glows and wide outlines read the
// true (round-cornered) distance without the median's far-field jumps.
float reconstructMtsdfNear(const float *rgba, float t, float near);

// Exact helpers shared with the tests.
// The outline with cubics split into quadratics, transformed to pixels.
Outline transformOutline(const Outline &outline, const Affine2 &transform, double cubic_tolerance);
// Coverage of each pixel of an already-transformed outline, nonzero rule:
// `subsamples` horizontal scanlines per pixel, each with exact horizontal
// extent.
std::vector<float> outlineCoverage(const Outline &pixel_outline, int width, int height, int subsamples);
// The exact signed distance (nonzero winding sign, overlaps removed) at any
// point of an already-transformed outline. Brute force; tests only.
class OutlineDistance {
public:
    explicit OutlineDistance(const Outline &pixel_outline);
    double signedDistance(Point2 point) const;
    int removedPieces() const;

private:
    struct State;
    std::shared_ptr<State> state;
};

} // namespace Pelican
