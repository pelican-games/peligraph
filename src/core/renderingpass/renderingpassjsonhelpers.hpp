#pragma once

#include "renderingpass.hpp"
#include "../../project/passfieldownership.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

vk::Format stringToFormat(const std::string &format_str);
std::string formatToString(vk::Format format);
vk::ImageUsageFlags stringToUsageFlags(const std::vector<std::string> &usage_strs);
// The names stringToFormat / stringToUsageFlags accept, in table order.
std::vector<std::string> imageFormatNames();
std::vector<std::string> imageUsageFlagNames();
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
uint32_t parseUint32Field(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context);
std::array<double, 4>
jsonToClearColor(const nlohmann::json &json);

} // namespace Pelican
