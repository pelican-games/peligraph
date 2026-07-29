#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

vk::Format stringToFormat(const std::string &format_str);
std::string formatToString(vk::Format format);
vk::ImageUsageFlags stringToUsageFlags(const std::vector<std::string> &usage_strs);
PassInfo makePassInfo(const std::string &type_str);
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

std::string parseStringField(const nlohmann::json &json, const std::string &field_name,
                             const std::string &context);
void validateName(const std::string &name, const std::string &context);
float parseFloatField(const nlohmann::json &json, const std::string &field_name, const std::string &context);
std::vector<std::string> parseStringArrayField(const nlohmann::json &json, const std::string &field_name,
                                               const std::string &context);
std::vector<std::string> parseOptionalRegionTags(
    const nlohmann::json &json, const std::string &context);
std::string parseRenderViewFamilyId(
    const nlohmann::json &json,
    const std::string &context);
uint32_t parseUint32Field(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context);
std::array<double, 4>
jsonToClearColor(const nlohmann::json &json);

} // namespace Pelican
