#include "renderingpassjsonhelpers.hpp"
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace Pelican {

vk::Format stringToFormat(const std::string &format_str) {
    static const std::unordered_map<std::string, vk::Format> format_map = {
        {"B8G8R8A8_UNORM", vk::Format::eB8G8R8A8Unorm},
        {"R8G8B8A8_UNORM", vk::Format::eR8G8B8A8Unorm},
        {"R8_UNORM", vk::Format::eR8Unorm},
        {"R16G16B16A16_SFLOAT", vk::Format::eR16G16B16A16Sfloat},
        {"D32_SFLOAT", vk::Format::eD32Sfloat},
        {"D24_UNORM_S8_UINT", vk::Format::eD24UnormS8Uint},
        {"D16_UNORM", vk::Format::eD16Unorm},
    };

    if (auto it = format_map.find(format_str); it != format_map.end()) {
        return it->second;
    }
    throw std::runtime_error("Unknown format: " + format_str);
}

vk::ImageUsageFlags stringToUsageFlags(const std::vector<std::string> &usage_strs) {
    if (usage_strs.empty()) {
        throw std::runtime_error("Image usage flags must not be empty");
    }

    vk::ImageUsageFlags flags;
    static const std::unordered_map<std::string, vk::ImageUsageFlagBits> usage_map = {
        {"COLOR_ATTACHMENT", vk::ImageUsageFlagBits::eColorAttachment},
        {"DEPTH_STENCIL_ATTACHMENT", vk::ImageUsageFlagBits::eDepthStencilAttachment},
        {"SAMPLED", vk::ImageUsageFlagBits::eSampled},
        {"STORAGE", vk::ImageUsageFlagBits::eStorage},
        {"TRANSFER_DST", vk::ImageUsageFlagBits::eTransferDst},
        {"TRANSFER_SRC", vk::ImageUsageFlagBits::eTransferSrc},
    };

    for (const auto &usage_str : usage_strs) {
        if (auto it = usage_map.find(usage_str); it != usage_map.end()) {
            flags |= it->second;
        } else {
            throw std::runtime_error("Unknown image usage flag: " + usage_str);
        }
    }
    return flags;
}

PassInfo makePassInfo(const std::string &type_str) {
    if (type_str == "material") {
        return MaterialPassInfo{};
    }
    if (type_str == "fullscreen") {
        return FullscreenPassInfo{};
    }
    if (type_str == "ui") {
        return UiPassInfo{};
    }
    throw std::runtime_error("Unknown pass type: " + type_str);
}

FullscreenPushConstantData stringToFullscreenPushConstantData(const std::string &data_str) {
    if (data_str == "none") {
        return FullscreenPushConstantData::eNone;
    }
    if (data_str == "camera_position") {
        return FullscreenPushConstantData::eCameraPosition;
    }
    if (data_str == "projection_view") {
        return FullscreenPushConstantData::eProjectionView;
    }
    throw std::runtime_error("Unknown fullscreen push constant data: " + data_str);
}

vk::AttachmentLoadOp stringToLoadOp(const std::string &op_str) {
    if (op_str == "Clear" || op_str == "clear") {
        return vk::AttachmentLoadOp::eClear;
    }
    if (op_str == "Load" || op_str == "load") {
        return vk::AttachmentLoadOp::eLoad;
    }
    if (op_str == "DontCare" || op_str == "dont_care" || op_str == "dontCare") {
        return vk::AttachmentLoadOp::eDontCare;
    }
    throw std::runtime_error("Unknown attachment load op: " + op_str);
}

vk::AttachmentStoreOp stringToStoreOp(const std::string &op_str) {
    if (op_str == "Store" || op_str == "store") {
        return vk::AttachmentStoreOp::eStore;
    }
    if (op_str == "DontCare" || op_str == "dont_care" || op_str == "dontCare") {
        return vk::AttachmentStoreOp::eDontCare;
    }
    throw std::runtime_error("Unknown attachment store op: " + op_str);
}

std::string parseStringField(const nlohmann::json &json, const std::string &field_name,
                             const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_string()) {
        throw std::runtime_error(context + " requires string field: " + field_name);
    }
    return json.at(field_name).get<std::string>();
}

void validateName(const std::string &name, const std::string &context) {
    if (name.empty()) {
        throw std::runtime_error(context + " name must not be empty");
    }
}

float parseFloatField(const nlohmann::json &json, const std::string &field_name,
                      const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_number()) {
        throw std::runtime_error(context + " requires numeric field: " + field_name);
    }
    return json.at(field_name).get<float>();
}

std::vector<std::string> parseStringArrayField(const nlohmann::json &json, const std::string &field_name,
                                               const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_array()) {
        throw std::runtime_error(context + " requires string array field: " + field_name);
    }

    std::vector<std::string> values;
    for (const auto &value_json : json.at(field_name)) {
        if (!value_json.is_string()) {
            throw std::runtime_error(context + " requires string array field: " + field_name);
        }
        values.push_back(value_json.get<std::string>());
    }
    return values;
}

uint32_t parseUint32Field(const nlohmann::json &json, const std::string &field_name,
                          const std::string &context) {
    if (!json.contains(field_name) || !json.at(field_name).is_number_integer()) {
        throw std::runtime_error(context + " requires non-negative integer field: " + field_name);
    }

    uint64_t value = 0;
    const auto &field = json.at(field_name);
    if (field.is_number_unsigned()) {
        value = field.get<uint64_t>();
    } else {
        const int64_t signed_value = field.get<int64_t>();
        if (signed_value < 0) {
            throw std::runtime_error(context + " requires non-negative integer field: " + field_name);
        }
        value = static_cast<uint64_t>(signed_value);
    }

    if (value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(context + " field is too large: " + field_name);
    }
    return static_cast<uint32_t>(value);
}

vk::ClearColorValue jsonToClearColor(const nlohmann::json &json) {
    if (!json.is_array() || json.size() != 4) {
        throw std::runtime_error("clear_color must be an array of four floats");
    }
    for (const auto &value_json : json) {
        if (!value_json.is_number()) {
            throw std::runtime_error("clear_color must be an array of four floats");
        }
    }

    return vk::ClearColorValue{std::array{
        json.at(0).get<float>(),
        json.at(1).get<float>(),
        json.at(2).get<float>(),
        json.at(3).get<float>(),
    }};
}

} // namespace Pelican
