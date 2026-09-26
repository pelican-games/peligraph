#pragma once

#include "viewfamilyrelation.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

// LP-Va named refusals for `view_select`. They travel as
// "<code>: <JSON fields>" so splitCodedRefusalMessage recovers the code and
// the fields on the editor's side.
//
// {pass, family, axis}: the family has no such axis (an authored family has
// none at all). {pass, family, axis, value, length} when the axis exists but
// the value is outside its compile-time length.
inline constexpr std::string_view viewSelectAxisUnknownCode =
    "graph.view_select_axis_unknown@1";
// {pass, family, axis}: the axis length is only known per frame (light).
inline constexpr std::string_view viewSelectRuntimeAxisCode =
    "graph.view_select_runtime_axis@1";
// {pass}: an execution without a physical target plan runs secondary-family
// nodes once on view 0 and cannot honor a selection.
inline constexpr std::string_view viewSelectNeedsTargetPlanCode =
    "graph.view_select_needs_target_plan@1";
// {pass, family}: only a secondary (provider-owned) family can be selected;
// `$main` views are the logical output views.
inline constexpr std::string_view viewSelectPrimaryFamilyCode =
    "graph.view_select_primary_family@1";

inline bool isViewSelectRefusalCode(std::string_view code) noexcept {
    return code == viewSelectAxisUnknownCode ||
           code == viewSelectRuntimeAxisCode ||
           code == viewSelectNeedsTargetPlanCode ||
           code == viewSelectPrimaryFamilyCode;
}

inline std::string viewSelectRefusalMessage(
    std::string_view code, const nlohmann::json &fields) {
    return std::string{code} + ": " + fields.dump();
}

[[noreturn]] inline void throwViewSelectRefusal(
    std::string_view code, const nlohmann::json &fields) {
    throw std::runtime_error(
        viewSelectRefusalMessage(code, fields));
}

// The first selected axis the coordinates do not carry, or nullopt when every
// selected axis is present. Matching is checked separately so a missing axis
// is reported instead of silently selecting nothing.
inline std::optional<std::string> missingViewSelectAxis(
    const RenderViewAxisCoordinates &coordinates,
    const RenderViewSelect &select) {
    for (const auto &[axis, value] : select) {
        (void)value;
        if (!coordinates.contains(axis)) return axis;
    }
    return std::nullopt;
}

inline bool renderViewMatchesSelect(
    const RenderViewAxisCoordinates &coordinates,
    const RenderViewSelect &select) {
    for (const auto &[axis, value] : select) {
        const auto found = coordinates.find(axis);
        if (found == coordinates.end() ||
            found->second != value) {
            return false;
        }
    }
    return true;
}

} // namespace Pelican
