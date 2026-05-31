#pragma once

#include "renderingpasscontainer.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican {

vk::Format stringToFormat(const std::string &format_str);
vk::ImageUsageFlags stringToUsageFlags(const std::vector<std::string> &usage_strs);
PassInfo makePassInfo(const std::string &type_str);
FullscreenPushConstantData stringToFullscreenPushConstantData(const std::string &data_str);
vk::AttachmentLoadOp stringToLoadOp(const std::string &op_str);
vk::AttachmentStoreOp stringToStoreOp(const std::string &op_str);

std::string parseStringField(const nlohmann::json &json, const std::string &field_name,
                             const std::string &context);
void validateName(const std::string &name, const std::string &context);
float parseFloatField(const nlohmann::json &json, const std::string &field_name, const std::string &context);
std::vector<std::string> parseStringArrayField(const nlohmann::json &json, const std::string &field_name,
                                               const std::string &context);
uint32_t parseUint32Field(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context);
vk::ClearColorValue jsonToClearColor(const nlohmann::json &json);

std::string readBinaryFile(const std::string &path);

} // namespace Pelican
