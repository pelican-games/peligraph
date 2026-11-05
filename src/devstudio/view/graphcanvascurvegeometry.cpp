#include "graphcanvascurvegeometry.hpp"

#include "../../core/userpublic/animation/beziersegment.hpp"

#include <algorithm>
#include <cmath>

namespace PelicanStudio::GraphCanvasCurveGeometry {
namespace {

double hermite(const GraphCanvasCurveKey &a, const GraphCanvasCurveKey &b, double t) {
    const double width = b.time - a.time;
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * a.value +
           (t3 - 2.0 * t2 + t) * width * curveOutTangent(a) +
           (-2.0 * t3 + 3.0 * t2) * b.value +
           (t3 - t2) * width * curveInTangent(b);
}

// Add the real extrema of a cubic segment: the roots of the Hermite
// derivative 3A t^2 + 2B t + C inside (0,1). Sampling only the ends is what
// used to clip an overshoot, and the frame must hold what is drawn.
void addCubicExtrema(const GraphCanvasCurveKey &a, const GraphCanvasCurveKey &b,
                     double &low, double &high) {
    const double width = b.time - a.time;
    const double A = 2.0 * (a.value - b.value) +
                     width * (curveOutTangent(a) + curveInTangent(b));
    const double B = 3.0 * (b.value - a.value) -
                     width * (2.0 * curveOutTangent(a) + curveInTangent(b));
    const double C = width * curveOutTangent(a);
    const double qa = 3.0 * A;
    const double qb = 2.0 * B;
    const double qc = C;
    double roots[2];
    int count = 0;
    const double scale = std::max({std::abs(qa), std::abs(qb), std::abs(qc), 1.0});
    if (std::abs(qa) <= 1.0e-12 * scale) {
        if (std::abs(qb) > 1.0e-12 * scale) roots[count++] = -qc / qb;
    } else {
        const double discriminant = qb * qb - 4.0 * qa * qc;
        if (discriminant >= 0.0) {
            const double root = std::sqrt(discriminant);
            roots[count++] = (-qb - root) / (2.0 * qa);
            roots[count++] = (-qb + root) / (2.0 * qa);
        }
    }
    for (int index = 0; index < count; ++index) {
        const double t = roots[index];
        if (!(t > 0.0 && t < 1.0) || !std::isfinite(t)) continue;
        const double value = hermite(a, b, t);
        low = std::min(low, value);
        high = std::max(high, value);
    }
}

void addBezierExtrema(const GraphCanvasCurveKey &a, const GraphCanvasCurveKey &b,
                      double &low, double &high) {
    const double width = b.time - a.time;
    if (!(width > 0.0)) return;
    const auto out = curveOutHandle(a).value_or(curveDefaultOutHandle(a, width));
    const auto in = curveInHandle(b).value_or(curveDefaultInHandle(b, width));
    const auto range = Pelican::Animation::bezierSegmentValueRange(
        a.time, a.value, out[0], out[1], b.time, b.value, in[0], in[1]);
    low = std::min(low, range[0]);
    high = std::max(high, range[1]);
}

} // namespace

QRectF curveFrame(const std::vector<GraphCanvasCurveKey> &keys) {
    if (keys.empty()) return QRectF{};
    double t0 = keys.front().time;
    double t1 = keys.back().time;
    double v0 = keys.front().value;
    double v1 = keys.front().value;
    for (const auto &key : keys) {
        v0 = std::min(v0, key.value);
        v1 = std::max(v1, key.value);
    }
    for (std::size_t index = 1; index < keys.size(); ++index) {
        if (keys[index - 1].interpolation == "cubic")
            addCubicExtrema(keys[index - 1], keys[index], v0, v1);
        else if (keys[index - 1].interpolation == "bezier")
            addBezierExtrema(keys[index - 1], keys[index], v0, v1);
    }
    if (!(t1 > t0)) {
        t0 -= 0.5;
        t1 += 0.5;
    }
    if (!(v1 > v0)) {
        v0 -= 0.5;
        v1 += 0.5;
    }
    const double pad_t = (t1 - t0) * 0.08;
    const double pad_v = (v1 - v0) * 0.12;
    t0 -= pad_t;
    t1 += pad_t;
    v0 -= pad_v;
    v1 += pad_v;
    return QRectF{t0, v0, t1 - t0, v1 - v0};
}

QPointF curvePoint(const QRectF &frame, const QRectF &plot, double time, double value) {
    const double frame_width = frame.width() > 0.0 ? frame.width() : 1.0;
    const double frame_height = frame.height() > 0.0 ? frame.height() : 1.0;
    return QPointF{plot.left() + plot.width() * (time - frame.left()) / frame_width,
                   plot.bottom() - plot.height() * (value - frame.top()) / frame_height};
}

CurvePaths curvePaths(const std::vector<GraphCanvasCurveKey> &keys, const QRectF &frame,
                      const QRectF &plot, qreal key_radius, int cubic_intervals) {
    CurvePaths paths;
    if (keys.empty()) return paths;
    const auto at = [&](double time, double value) {
        return curvePoint(frame, plot, time, value);
    };
    const auto key_at = [&](const GraphCanvasCurveKey &key) {
        return at(key.time, key.value);
    };

    QPainterPath curve;
    curve.moveTo(at(frame.left(), keys.front().value));
    curve.lineTo(key_at(keys.front()));

    // Spend the sampling budget where the curved ink is: a segment's share
    // of the plot's width. A segment that spans the whole plot gets all 64.
    double curved_width = 0.0;
    const double frame_width = frame.width() > 0.0 ? frame.width() : 1.0;
    for (std::size_t index = 1; index < keys.size(); ++index) {
        const auto &kind = keys[index - 1].interpolation;
        if (kind == "cubic" || kind == "bezier")
            curved_width +=
                (keys[index].time - keys[index - 1].time) / frame_width * plot.width();
    }

    for (std::size_t index = 1; index < keys.size(); ++index) {
        const auto &a = keys[index - 1];
        const auto &b = keys[index];
        if (a.interpolation == "step") {
            const QPointF next = key_at(b);
            curve.lineTo(QPointF{next.x(), curve.currentPosition().y()});
            curve.lineTo(next);
        } else if (a.interpolation == "cubic" || a.interpolation == "bezier") {
            int intervals = 1;
            if (curved_width > 0.0 && cubic_intervals > 0) {
                const double pixel_width =
                    (b.time - a.time) / frame_width * plot.width();
                intervals = std::max(1, static_cast<int>(
                                             std::lround(cubic_intervals * pixel_width / curved_width)));
            }
            if (a.interpolation == "cubic") {
                for (int step = 1; step < intervals; ++step) {
                    const double t = static_cast<double>(step) / intervals;
                    curve.lineTo(at(a.time + (b.time - a.time) * t, hermite(a, b, t)));
                }
            } else {
                const double width = b.time - a.time;
                const auto out = curveOutHandle(a).value_or(curveDefaultOutHandle(a, width));
                const auto in = curveInHandle(b).value_or(curveDefaultInHandle(b, width));
                for (int step = 1; step < intervals; ++step) {
                    const double u = static_cast<double>(step) / intervals;
                    const auto point = Pelican::Animation::bezierSegmentPointAtU(
                        a.time, a.value, out[0], out[1], b.time, b.value, in[0], in[1], u);
                    curve.lineTo(at(point[0], point[1]));
                }
            }
            curve.lineTo(key_at(b));
        } else {
            curve.lineTo(key_at(b));
        }
    }
    curve.lineTo(at(frame.right(), keys.back().value));

    QPainterPath marks;
    if (key_radius > 0.0)
        for (const auto &key : keys)
            marks.addEllipse(key_at(key), key_radius, key_radius);

    paths.curve = std::move(curve);
    paths.keys = std::move(marks);
    return paths;
}

} // namespace PelicanStudio::GraphCanvasCurveGeometry
