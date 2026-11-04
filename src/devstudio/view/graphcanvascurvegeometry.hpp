#pragma once

// SDK-2a: the frame and path a value.curve is drawn with. The large curve in
// the Graph tab's fields and the fixed-size plot on the open node call these
// same functions, so their frame, sampling and outside-key behaviour cannot
// drift apart. The path follows the engine's ScalarCurve::sample (curve.cpp):
// linear and step jumps at keys, cubic through the tangents, and the value
// held before the first key and after the last.

#include "../model/graphcanvascurve.hpp"

#include <QPainterPath>
#include <QPointF>
#include <QRectF>

#include <cstddef>
#include <vector>

namespace PelicanStudio::GraphCanvasCurveGeometry {

// The curve's drawn value range, padded as the old small curve padded it:
// 8 % of the time range and 12 % of the value range, with a half-unit range
// for a degenerate axis. For a cubic segment the value range includes the
// segment's real extrema, not just its end keys.
[[nodiscard]] QRectF curveFrame(const std::vector<GraphCanvasCurveKey> &keys);

// The point of `time,value` in `plot`, through `frame` as
// QRectF(t0, v0, t1 - t0, v1 - v0).
[[nodiscard]] QPointF curvePoint(const QRectF &frame, const QRectF &plot, double time,
                                 double value);

struct CurvePaths {
    QPainterPath curve;
    QPainterPath keys;
};

// One sorted walk of the keys: a line between keys for linear, a horizontal
// run and a vertical jump at the next key for step, and sampled Hermite
// points for cubic. `cubic_intervals` samples are spent across all cubic
// segments in proportion to their width, with at least one interval each;
// the old 96-point whole-frame scan is gone. Key marks are circles of
// `key_radius`. Outside the keys, the curve is level at the first/last value.
[[nodiscard]] CurvePaths curvePaths(const std::vector<GraphCanvasCurveKey> &keys,
                                    const QRectF &frame, const QRectF &plot,
                                    qreal key_radius = 4.5, int cubic_intervals = 64);

} // namespace PelicanStudio::GraphCanvasCurveGeometry
