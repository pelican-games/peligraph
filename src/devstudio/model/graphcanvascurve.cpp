#include "graphcanvascurve.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string_view>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

bool finiteNumber(const Json &value) {
    return value.is_number() && std::isfinite(value.get<double>());
}

bool knownInterpolation(std::string_view interpolation) {
    return interpolation == "linear" || interpolation == "step" || interpolation == "cubic";
}

double tangentOf(const GraphCanvasCurveKey &key, const char *name) {
    if (!key.rest.is_object()) return 0.0;
    const auto found = key.rest.find(name);
    return found != key.rest.end() && found->is_number() ? found->get<double>() : 0.0;
}

} // namespace

std::optional<std::vector<GraphCanvasCurveKey>>
curveKeysFromParameters(const Json &parameters) {
    // The engine's constructor throws on every one of these; a curve the
    // canvas cannot draw is nullopt here, and the caller draws its frame
    // only.
    try {
        if (!parameters.is_object()) return std::nullopt;
        const auto found = parameters.find("keys");
        if (found == parameters.end() || !found->is_array() || found->empty())
            return std::nullopt;
        std::vector<GraphCanvasCurveKey> keys;
        keys.reserve(found->size());
        for (const Json &entry : *found) {
            if (!entry.is_object()) return std::nullopt;
            // Exactly the fields the engine reads (valuegraph.cpp op "curve").
            for (const auto &[name, unused] : entry.items()) {
                (void)unused;
                if (name != "time" && name != "value" && name != "interpolation" &&
                    name != "in_tangent" && name != "out_tangent")
                    return std::nullopt;
            }
            const auto time = entry.find("time");
            const auto value = entry.find("value");
            if (time == entry.end() || value == entry.end() || !finiteNumber(*time) ||
                !finiteNumber(*value))
                return std::nullopt;
            GraphCanvasCurveKey key;
            key.time = time->get<double>();
            key.value = value->get<double>();
            const auto interpolation = entry.find("interpolation");
            if (interpolation != entry.end()) {
                if (!interpolation->is_string()) return std::nullopt;
                key.interpolation = interpolation->get<std::string>();
                if (!knownInterpolation(key.interpolation)) return std::nullopt;
            }
            for (const char *name : {"in_tangent", "out_tangent"}) {
                const auto tangent = entry.find(name);
                if (tangent == entry.end()) continue;
                if (!finiteNumber(*tangent)) return std::nullopt;
                key.rest[name] = *tangent;
            }
            if (!keys.empty() && !(keys.back().time < key.time)) return std::nullopt;
            keys.push_back(std::move(key));
        }
        return keys;
    } catch (...) {
        // nlohmann type errors and out-of-range conversions are a refusal,
        // not an exception across the canvas.
        return std::nullopt;
    }
}

double sampleCurve(const std::vector<GraphCanvasCurveKey> &keys, double time) {
    if (keys.empty()) return 0.0;
    if (!std::isfinite(time)) return 0.0;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    // The engine's exact comparator: at an inner key's own time upper_bound
    // lands on the key after it, so a step returns that key's value.
    const auto next = std::upper_bound(keys.begin(), keys.end(), time,
        [](double at, const GraphCanvasCurveKey &key) { return at < key.time; });
    const auto &a = *std::prev(next);
    const auto &b = *next;
    if (a.interpolation == "step") return a.value;
    const double width = b.time - a.time;
    const double t = (time - a.time) / width;
    if (a.interpolation == "cubic") {
        const double t2 = t * t;
        const double t3 = t2 * t;
        return (2.0 * t3 - 3.0 * t2 + 1.0) * a.value +
               (t3 - 2.0 * t2 + t) * width * curveOutTangent(a) +
               (-2.0 * t3 + 3.0 * t2) * b.value +
               (t3 - t2) * width * curveInTangent(b);
    }
    return std::lerp(a.value, b.value, t);
}

double curveInTangent(const GraphCanvasCurveKey &key) { return tangentOf(key, "in_tangent"); }

double curveOutTangent(const GraphCanvasCurveKey &key) { return tangentOf(key, "out_tangent"); }

} // namespace PelicanStudio
