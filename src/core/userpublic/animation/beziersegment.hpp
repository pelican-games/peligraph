#pragma once

// CB-1: the one Bezier segment solver. The engine's BezierCurve and Studio's
// key reading both call these functions, so a drawn curve and an evaluated
// curve cannot disagree about the parameterisation.
//
// A segment runs from a_time to b_time. Its handles are (dt, dv): a time
// offset from the owning key and a value offset from it. out_dt is clamped to
// [0, width] and in_dt to [-width, 0] when evaluating; dv is never clamped.
// Everything internal is normalised by the segment width, so the solver never
// mixes an absolute time with a normalised coordinate.

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace Pelican::Animation {

struct BezierSegmentSplit {
    double u{};
    double time{};
    double value{};
    std::array<double, 2> a_out_handle{};
    std::array<double, 2> middle_in_handle{};
    std::array<double, 2> middle_out_handle{};
    std::array<double, 2> b_in_handle{};
};

namespace bezier_detail {

inline double cubic(double p0, double p1, double p2, double p3, double u) {
    const double v = 1.0 - u;
    return v * v * v * p0 + 3.0 * v * v * u * p1 + 3.0 * v * u * u * p2 + u * u * u * p3;
}

inline double cubicDerivative(double p0, double p1, double p2, double p3, double u) {
    const double v = 1.0 - u;
    return 3.0 * (v * v * (p1 - p0) + 2.0 * v * u * (p2 - p1) + u * u * (p3 - p2));
}

inline double lerp(double a, double b, double u) { return a + (b - a) * u; }

// Solve x(u) = s on [0, 1], where x is the normalised time coordinate of a
// segment with control x values 0, x1, x2, 1. The bracket always contains the
// root; Newton is used only when its candidate stays inside it.
inline std::optional<double> solveNormalised(double x1, double x2, double s) {
    if (!std::isfinite(x1) || !std::isfinite(x2) || !std::isfinite(s)) return std::nullopt;
    if (s <= 0.0) return 0.0;
    if (s >= 1.0) return 1.0;
    const auto x_at = [=](long double u) {
        const long double v = 1.0L - u;
        return 3.0L * v * v * u * static_cast<long double>(x1) +
               3.0L * v * u * u * static_cast<long double>(x2) + u * u * u;
    };
    const auto slope_at = [=](long double u) {
        const long double v = 1.0L - u;
        return 3.0L * (v * v * x1 + 2.0L * v * u * (x2 - x1) + u * u * (1.0L - x2));
    };
    long double lo = 0.0L;
    long double hi = 1.0L;
    const long double wanted = s;
    for (int iteration = 0; iteration < 64; ++iteration) {
        if (hi - lo <= 1.0e-14L) break;
        const long double midpoint = 0.5L * (lo + hi);
        long double u = midpoint;
        const long double slope = slope_at(midpoint);
        if (slope > 0.0L && std::isfinite(slope)) {
            const long double candidate = midpoint - (x_at(midpoint) - wanted) / slope;
            if (candidate > lo && candidate < hi) u = candidate;
        }
        const long double xu = x_at(u);
        if (xu < wanted) lo = u;
        else if (xu > wanted) hi = u;
        else return static_cast<double>(u);
    }
    return static_cast<double>(0.5L * (lo + hi));
}

} // namespace bezier_detail

// The value of the segment at `time`; values outside the segment are held at
// its ends. The caller normally passes a time already known to be inside.
inline double bezierSegmentValue(double a_time, double a_value, double out_dt, double out_dv,
                                 double b_time, double b_value, double in_dt, double in_dv,
                                 double time) {
    if (!std::isfinite(a_time) || !std::isfinite(b_time) || !std::isfinite(a_value) ||
        !std::isfinite(b_value) || !std::isfinite(time))
        return a_value;
    if (!(b_time > a_time)) return a_value;
    if (time <= a_time) return a_value;
    if (time >= b_time) return b_value;
    if (!std::isfinite(out_dt) || !std::isfinite(out_dv) || !std::isfinite(in_dt) ||
        !std::isfinite(in_dv))
        return a_value;
    const double width = b_time - a_time;
    if (!std::isfinite(width)) return a_value;
    out_dt = std::clamp(out_dt, 0.0, width);
    in_dt = std::clamp(in_dt, -width, 0.0);
    const double s = (time - a_time) / width;
    const auto u = bezier_detail::solveNormalised(out_dt / width, 1.0 + in_dt / width, s);
    if (!u) return a_value;
    return bezier_detail::cubic(a_value, a_value + out_dv, b_value + in_dv, b_value, *u);
}

// The point (time, value) at parameter `u`, with the evaluation clamps. The
// canvas draws in u, not in time, so its ink follows the same control points.
inline std::array<double, 2> bezierSegmentPointAtU(
    double a_time, double a_value, double out_dt, double out_dv,
    double b_time, double b_value, double in_dt, double in_dv, double u) {
    if (!(b_time > a_time) || !std::isfinite(u)) return {a_time, a_value};
    if (u < 0.0) u = 0.0;
    if (u > 1.0) u = 1.0;
    const double width = b_time - a_time;
    if (!std::isfinite(out_dt)) out_dt = 0.0;
    if (!std::isfinite(in_dt)) in_dt = 0.0;
    if (!std::isfinite(out_dv)) out_dv = 0.0;
    if (!std::isfinite(in_dv)) in_dv = 0.0;
    out_dt = std::clamp(out_dt, 0.0, width);
    in_dt = std::clamp(in_dt, -width, 0.0);
    return {a_time + bezier_detail::cubic(0.0, out_dt, width + in_dt, width, u),
            bezier_detail::cubic(a_value, a_value + out_dv, b_value + in_dv, b_value, u)};
}

// The segment's value range, including its ends and every interior extremum
// (the roots of dy/du). x(u) is monotone for clamped handles, so these are all
// the values the segment can draw.
inline std::array<double, 2> bezierSegmentValueRange(
    double a_time, double a_value, double out_dt, double out_dv,
    double b_time, double b_value, double in_dt, double in_dv) {
    if (!(b_time > a_time) || !std::isfinite(a_value) || !std::isfinite(b_value) ||
        !std::isfinite(out_dv) || !std::isfinite(in_dv))
        return {a_value, a_value};
    (void)out_dt;
    (void)in_dt;
    const double p0 = a_value;
    const double p1 = a_value + out_dv;
    const double p2 = b_value + in_dv;
    const double p3 = b_value;
    double low = std::min(p0, p3);
    double high = std::max(p0, p3);
    // dy/du = 3[(P1-P0)(1-u)^2 + 2(P2-P1)(1-u)u + (P3-P2)u^2].
    const double qa = 3.0 * (-p0 + 3.0 * p1 - 3.0 * p2 + p3);
    const double qb = 6.0 * (p0 - 2.0 * p1 + p2);
    const double qc = 3.0 * (p1 - p0);
    double roots[2];
    int count = 0;
    const double scale = std::max({std::abs(qa), std::abs(qb), std::abs(qc), 1.0});
    if (std::abs(qa) <= 1.0e-14 * scale) {
        if (std::abs(qb) > 1.0e-14 * scale) roots[count++] = -qc / qb;
    } else {
        const double discriminant = qb * qb - 4.0 * qa * qc;
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            roots[count++] = (-qb - root) / (2.0 * qa);
            roots[count++] = (-qb + root) / (2.0 * qa);
        }
    }
    for (int index = 0; index < count; ++index) {
        const double u = roots[index];
        if (!(u > 0.0 && u < 1.0) || !std::isfinite(u)) continue;
        const double value = bezier_detail::cubic(p0, p1, p2, p3, u);
        low = std::min(low, value);
        high = std::max(high, value);
    }
    return {low, high};
}

// Split at `time` (which has to lie strictly inside the segment) with De
// Casteljau. The four control points are taken after the evaluation clamps.
inline std::optional<BezierSegmentSplit> splitBezierSegment(
    double a_time, double a_value, double out_dt, double out_dv,
    double b_time, double b_value, double in_dt, double in_dv, double time) {
    if (!std::isfinite(a_time) || !std::isfinite(b_time) || !std::isfinite(time) ||
        !std::isfinite(a_value) || !std::isfinite(b_value) || !std::isfinite(out_dt) ||
        !std::isfinite(out_dv) || !std::isfinite(in_dt) || !std::isfinite(in_dv))
        return std::nullopt;
    if (!(a_time < time && time < b_time)) return std::nullopt;
    const double width = b_time - a_time;
    const double width_left = time - a_time;
    const double width_right = b_time - time;
    if (!(width > 0.0) || !(width_left > 0.0) || !(width_right > 0.0)) return std::nullopt;
    out_dt = std::clamp(out_dt, 0.0, width);
    in_dt = std::clamp(in_dt, -width, 0.0);
    const double s = width_left / width;
    const auto u = bezier_detail::solveNormalised(out_dt / width, 1.0 + in_dt / width, s);
    if (!u || !std::isfinite(*u)) return std::nullopt;

    const std::array<double, 4> x{0.0, out_dt, width + in_dt, width};
    const std::array<double, 4> y{a_value, a_value + out_dv, b_value + in_dv, b_value};
    const double t = *u;
    const std::array<double, 4> ax{bezier_detail::lerp(x[0], x[1], t),
                                   bezier_detail::lerp(x[1], x[2], t),
                                   bezier_detail::lerp(x[2], x[3], t)};
    const std::array<double, 4> ay{bezier_detail::lerp(y[0], y[1], t),
                                   bezier_detail::lerp(y[1], y[2], t),
                                   bezier_detail::lerp(y[2], y[3], t)};
    const std::array<double, 4> bx{bezier_detail::lerp(ax[0], ax[1], t),
                                   bezier_detail::lerp(ax[1], ax[2], t)};
    const std::array<double, 4> by{bezier_detail::lerp(ay[0], ay[1], t),
                                   bezier_detail::lerp(ay[1], ay[2], t)};
    const double middle_y = bezier_detail::lerp(by[0], by[1], t);

    BezierSegmentSplit result;
    result.u = t;
    result.time = time;
    result.value = middle_y;
    // The middle key's stored time is `time`; its sub-segments' endpoints have
    // to use that same x coordinate, not the solver's last bit.
    result.a_out_handle = {std::clamp(ax[0], 0.0, width_left), ay[0] - a_value};
    result.middle_in_handle = {std::clamp(bx[0] - width_left, -width_left, 0.0),
                               by[0] - middle_y};
    result.middle_out_handle = {std::clamp(bx[1] - width_left, 0.0, width_right),
                                by[1] - middle_y};
    result.b_in_handle = {std::clamp(ax[2] - width, -width_right, 0.0), ay[2] - b_value};
    if (!std::isfinite(result.value) || !std::isfinite(result.u) ||
        !std::isfinite(result.a_out_handle[0]) || !std::isfinite(result.a_out_handle[1]) ||
        !std::isfinite(result.middle_in_handle[0]) || !std::isfinite(result.middle_in_handle[1]) ||
        !std::isfinite(result.middle_out_handle[0]) || !std::isfinite(result.middle_out_handle[1]) ||
        !std::isfinite(result.b_in_handle[0]) || !std::isfinite(result.b_in_handle[1]))
        return std::nullopt;
    return result;
}

} // namespace Pelican::Animation
