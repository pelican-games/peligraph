#pragma once

#include "renderingpass.hpp"
#include "../../project/passfieldownership.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

// RC-3a1: the names of image formats and usages, what a render target may
// declare and what each format is, are the project's (imageformat.hpp,
// imageusage.hpp); core maps them to Vulkan (vulkanimagevocabulary.hpp).
PassInfo makePassInfo(const std::string &type_str);
PassInfo makePassInfo(RenderPassType type);
FullscreenPushConstantData stringToFullscreenPushConstantData(const std::string &data_str);
std::string_view renderResolutionDomainName(
    RenderResolutionDomain domain);
RenderResolutionDomain defaultRenderResolutionDomain(
    std::string_view pass_type);
RenderResolutionDomain parseRenderResolutionDomain(
    const nlohmann::json &pass_json, std::string_view pass_type,
    const std::string &pass_name);
vk::AttachmentLoadOp stringToLoadOp(const std::string &op_str);
vk::AttachmentStoreOp stringToStoreOp(const std::string &op_str);

std::string parseStringField(const nlohmann::json &json, std::string_view field_name,
                             std::string_view context);
// A field that accepts a single string or an array of strings. The context
// is the caller's own wording, so the thrown message stays the caller's.
std::vector<std::string> parseStringList(const nlohmann::json &json,
                                         std::string_view context);
std::vector<std::string> parseOptionalStringList(const nlohmann::json &json,
                                                 std::string_view field,
                                                 std::string_view context);
// Name lists keep their authored order and admit each name once.
void appendUnique(std::vector<std::string> &values, std::string value);
void appendUnique(std::vector<std::string> &values,
                  const std::vector<std::string> &more_values);
void validateName(const std::string &name, const std::string &context);
float parseFloatField(const nlohmann::json &json, const std::string &field_name, const std::string &context);
std::vector<std::string> parseStringArrayField(const nlohmann::json &json, const std::string &field_name,
                                               const std::string &context);
std::vector<std::string> parseOptionalRegionTags(
    const nlohmann::json &json, const std::string &context);
std::string parseRenderViewFamilyId(
    const nlohmann::json &json,
    const std::string &context);
// LP-Va: optional `view_select` object, axis name -> non-negative integer.
// Absent -> empty. Whether the axis exists is checked at recompile time
// against the family's provider (validateRenderViewSelects).
RenderViewSelect parseRenderViewSelect(
    const nlohmann::json &json,
    const std::string &context);
// PS-1: a pass's optional `schedule`, "per_frame" (the default) or "once".
// True for "once". Which pass types may carry it is the field ownership
// table's (fullscreen, raster).
bool parsePassSchedule(const nlohmann::json &json,
                       const std::string &pass_name);
uint32_t parseUint32Field(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context);
std::array<double, 4>
jsonToClearColor(const nlohmann::json &json);

} // namespace Pelican
