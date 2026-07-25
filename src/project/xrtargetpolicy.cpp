#include "xrtargetpolicy.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace Pelican {

std::string_view xrViewExecutionPreferenceName(
    XrViewExecutionPreference preference) {
    switch (preference) {
    case XrViewExecutionPreference::automatic:
        return "auto";
    case XrViewExecutionPreference::sequential:
        return "sequential";
    case XrViewExecutionPreference::require_multiview:
        return "multiview";
    }
    return "unknown";
}

CompiledXrTargetPolicy compileXrTargetPolicy(
    const nlohmann::json &config,
    RenderPipelineGraphVariant variant) {
    CompiledXrTargetPolicy result;
    result.active = variant == RenderPipelineGraphVariant::xr;
    const auto declaration = config.find("xr");
    if (declaration == config.end()) return result;
    if (!declaration->is_object()) {
        throw std::runtime_error(
            "rendering config xr must be an object");
    }
    result.authored = true;
    for (auto field = declaration->begin();
         field != declaration->end(); ++field) {
        if (field.key() != "view_execution" &&
            field.key() != "multiview_auto") {
            throw std::runtime_error(
                "rendering config xr has unknown key '" +
                field.key() + "'");
        }
    }
    if (const auto automatic =
            declaration->find("multiview_auto");
        automatic != declaration->end()) {
        result.multiview_auto =
            compileXrMultiviewAutoPolicy(
                *automatic);
        result.multiview_auto_authored = true;
    }
    const auto execution =
        declaration->find("view_execution");
    if (execution == declaration->end()) return result;
    if (!execution->is_string()) {
        throw std::runtime_error(
            "rendering config xr view_execution must be a string");
    }
    const auto value = execution->get<std::string>();
    if (value == "auto") {
        result.view_execution =
            XrViewExecutionPreference::automatic;
    } else if (value == "sequential") {
        result.view_execution =
            XrViewExecutionPreference::sequential;
    } else if (value == "multiview") {
        result.view_execution =
            XrViewExecutionPreference::require_multiview;
    } else {
        throw std::runtime_error(
            "rendering config xr has unknown view_execution: " +
            value);
    }
    return result;
}

nlohmann::ordered_json xrTargetPolicyToJson(
    const CompiledXrTargetPolicy &policy) {
    return nlohmann::ordered_json{
        {"active", policy.active},
        {"authored", policy.authored},
        {"view_execution",
         xrViewExecutionPreferenceName(policy.view_execution)},
        {"multiview_auto",
         nlohmann::ordered_json{
             {"authored",
              policy.multiview_auto_authored},
             {"policy",
              xrMultiviewAutoPolicyToJson(
                  policy.multiview_auto)},
         }},
    };
}

} // namespace Pelican
