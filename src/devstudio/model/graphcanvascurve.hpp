#pragma once

// SDK-2a: the one reading of a value.curve's keys, beside the engine's
// ScalarCurve (src/core/userpublic/animation/curve.cpp). The Graph tab's
// fields and the node body both use this reading, so a key that the engine
// would refuse makes the whole curve unusable here too -- never a partial
// curve, never an exception out of JSON parsing.

#include "graphcanvasmodel.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <vector>

namespace PelicanStudio {

// The keys of a value.curve's `parameters`, or nullopt when the engine's
// ScalarCurve would refuse them: a missing or empty `keys`, an element that
// is not an object, a non-finite time/value/tangent, an interpolation the
// engine does not know, an unknown field, or times that do not strictly rise.
// Missing interpolation and tangents take the engine's defaults. The tangent
// fields stay in `rest` exactly as authored, so a round trip writes what it
// read.
[[nodiscard]] std::optional<std::vector<GraphCanvasCurveKey>>
curveKeysFromParameters(const nlohmann::json &parameters);

// The value at `time`, as engine ScalarCurve::sample computes it: ends held,
// linear lerp, a step's value, and a cubic segment through its tangents.
// `keys` may be empty (0.0); a valid curve from curveKeysFromParameters is
// never empty.
[[nodiscard]] double sampleCurve(const std::vector<GraphCanvasCurveKey> &keys, double time);

// A key's cubic tangents, 0 when the key does not carry them (the engine's
// default). curveKeysFromParameters has already checked their finiteness.
[[nodiscard]] double curveInTangent(const GraphCanvasCurveKey &key);
[[nodiscard]] double curveOutTangent(const GraphCanvasCurveKey &key);

} // namespace PelicanStudio
