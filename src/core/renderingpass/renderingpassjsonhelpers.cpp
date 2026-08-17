#include "renderingpassjsonhelpers.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "../../project/logicalrendergraph.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace Pelican {

namespace {

constexpr std::array format_names{
    std::pair{"R8_UNORM", vk::Format::eR8Unorm},
    std::pair{"R8_SNORM", vk::Format::eR8Snorm},
    std::pair{"R8_UINT", vk::Format::eR8Uint},
    std::pair{"R8_SINT", vk::Format::eR8Sint},
    std::pair{"R8G8_UNORM", vk::Format::eR8G8Unorm},
    std::pair{"R8G8_SNORM", vk::Format::eR8G8Snorm},
    std::pair{"R8G8_UINT", vk::Format::eR8G8Uint},
    std::pair{"R8G8_SINT", vk::Format::eR8G8Sint},
    std::pair{"R8G8B8A8_UNORM", vk::Format::eR8G8B8A8Unorm},
    std::pair{"R8G8B8A8_SNORM", vk::Format::eR8G8B8A8Snorm},
    std::pair{"R8G8B8A8_UINT", vk::Format::eR8G8B8A8Uint},
    std::pair{"R8G8B8A8_SINT", vk::Format::eR8G8B8A8Sint},
    std::pair{"R8G8B8A8_SRGB", vk::Format::eR8G8B8A8Srgb},
    std::pair{"B8G8R8A8_UNORM", vk::Format::eB8G8R8A8Unorm},
    std::pair{"B8G8R8A8_SRGB", vk::Format::eB8G8R8A8Srgb},
    std::pair{"A2B10G10R10_UNORM_PACK32",
              vk::Format::eA2B10G10R10UnormPack32},
    std::pair{"A2R10G10B10_UNORM_PACK32",
              vk::Format::eA2R10G10B10UnormPack32},
    std::pair{"B10G11R11_UFLOAT_PACK32",
              vk::Format::eB10G11R11UfloatPack32},
    std::pair{"R16_UNORM", vk::Format::eR16Unorm},
    std::pair{"R16_SNORM", vk::Format::eR16Snorm},
    std::pair{"R16_UINT", vk::Format::eR16Uint},
    std::pair{"R16_SINT", vk::Format::eR16Sint},
    std::pair{"R16_SFLOAT", vk::Format::eR16Sfloat},
    std::pair{"R16G16_UNORM", vk::Format::eR16G16Unorm},
    std::pair{"R16G16_SNORM", vk::Format::eR16G16Snorm},
    std::pair{"R16G16_UINT", vk::Format::eR16G16Uint},
    std::pair{"R16G16_SINT", vk::Format::eR16G16Sint},
    std::pair{"R16G16_SFLOAT", vk::Format::eR16G16Sfloat},
    std::pair{"R16G16B16A16_UNORM",
              vk::Format::eR16G16B16A16Unorm},
    std::pair{"R16G16B16A16_SNORM",
              vk::Format::eR16G16B16A16Snorm},
    std::pair{"R16G16B16A16_UINT",
              vk::Format::eR16G16B16A16Uint},
    std::pair{"R16G16B16A16_SINT",
              vk::Format::eR16G16B16A16Sint},
    std::pair{"R16G16B16A16_SFLOAT",
              vk::Format::eR16G16B16A16Sfloat},
    std::pair{"R32_UINT", vk::Format::eR32Uint},
    std::pair{"R32_SINT", vk::Format::eR32Sint},
    std::pair{"R32_SFLOAT", vk::Format::eR32Sfloat},
    std::pair{"R32G32_UINT", vk::Format::eR32G32Uint},
    std::pair{"R32G32_SINT", vk::Format::eR32G32Sint},
    std::pair{"R32G32_SFLOAT", vk::Format::eR32G32Sfloat},
    std::pair{"R32G32B32A32_UINT",
              vk::Format::eR32G32B32A32Uint},
    std::pair{"R32G32B32A32_SINT",
              vk::Format::eR32G32B32A32Sint},
    std::pair{"R32G32B32A32_SFLOAT",
              vk::Format::eR32G32B32A32Sfloat},
    std::pair{"D16_UNORM", vk::Format::eD16Unorm},
    std::pair{"D24_UNORM_S8_UINT",
              vk::Format::eD24UnormS8Uint},
    std::pair{"D32_SFLOAT", vk::Format::eD32Sfloat},
};

} // namespace

vk::Format stringToFormat(const std::string &format_str) {
    const auto found = std::find_if(
        format_names.begin(), format_names.end(),
        [&](const auto &entry) {
            return entry.first == format_str;
        });
    if (found != format_names.end()) {
        return found->second;
    }
    throw std::runtime_error("Unknown format: " + format_str);
}

std::string formatToString(vk::Format format) {
    const auto found = std::find_if(
        format_names.begin(), format_names.end(),
        [format](const auto &entry) {
            return entry.second == format;
        });
    if (found != format_names.end()) {
        return std::string{found->first};
    }
    throw std::runtime_error(
        "Unsupported resolver v2 format: " +
        vk::to_string(format));
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

PassInfo makePassInfo(RenderPassType type) {
    switch (type) {
    case RenderPassType::material:
        return MaterialPassInfo{};
    case RenderPassType::fullscreen:
        return FullscreenPassInfo{};
    case RenderPassType::raster:
        return GenericRasterPassInfo{};
    case RenderPassType::output_transform:
        return FullscreenPassInfo{};
    case RenderPassType::debug_draw:
        return DebugDrawPassInfo{};
    case RenderPassType::gizmo:
        return GizmoPassInfo{};
    case RenderPassType::debug_text:
        return DebugTextPassInfo{};
    case RenderPassType::shadow_depth:
        return ShadowDepthPassInfo{};
    case RenderPassType::velocity:
        return VelocityPassInfo{};
    case RenderPassType::picking:
        return PickingPassInfo{};
    case RenderPassType::ui:
        return UiPassInfo{};
    case RenderPassType::imgui:
#if PELICAN_WITH_IMGUI
        return ImGuiPassInfo{};
#else
        break;
#endif
    case RenderPassType::canonical_anchor:
    case RenderPassType::snapshot_copy:
        break;
    }
    throw std::runtime_error(
        "Pass type cannot be materialized as a rendering pass: " +
        std::string{renderPassTypeName(type)});
}

PassInfo makePassInfo(const std::string &type_str) {
    const nlohmann::json pass{
        {"name", type_str},
        {"type", type_str},
    };
    return makePassInfo(validatePassFieldOwnership(
        pass, buildPassFieldOwnershipCapabilities()));
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

std::string_view renderResolutionDomainName(
    RenderResolutionDomain domain) {
    switch (domain) {
    case RenderResolutionDomain::unclassified:
        return "unclassified";
    case RenderResolutionDomain::scene:
        return "scene";
    case RenderResolutionDomain::output:
        return "output";
    case RenderResolutionDomain::independent:
        return "independent";
    }
    throw std::runtime_error("Unknown render resolution domain");
}

RenderResolutionDomain defaultRenderResolutionDomain(
    std::string_view pass_type) {
    if (pass_type == "material" || pass_type == "velocity" ||
        pass_type == "picking") {
        return RenderResolutionDomain::scene;
    }
    if (pass_type == "output_transform" || pass_type == "ui" ||
        pass_type == "gizmo" ||
        pass_type == "imgui") {
        return RenderResolutionDomain::output;
    }
    if (pass_type == "shadow_depth") {
        return RenderResolutionDomain::independent;
    }
    return RenderResolutionDomain::unclassified;
}

RenderResolutionDomain parseRenderResolutionDomain(
    const nlohmann::json &pass_json, std::string_view pass_type,
    const std::string &pass_name) {
    if (!pass_json.contains("resolution_domain")) {
        return defaultRenderResolutionDomain(pass_type);
    }
    const auto &encoded = pass_json.at("resolution_domain");
    if (!encoded.is_string()) {
        throw std::runtime_error(
            "Pass resolution_domain must be a string: " + pass_name);
    }
    const auto value = encoded.get<std::string>();
    if (value == "unclassified" || value == "none") {
        return RenderResolutionDomain::unclassified;
    }
    if (value == "scene") {
        return RenderResolutionDomain::scene;
    }
    if (value == "output") {
        return RenderResolutionDomain::output;
    }
    if (value == "independent") {
        return RenderResolutionDomain::independent;
    }
    throw std::runtime_error(
        "Unknown pass resolution_domain '" + value + "': " +
        pass_name);
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

std::vector<std::string> parseOptionalRegionTags(
    const nlohmann::json &json, const std::string &context) {
    if (!json.contains("regions")) {
        return {};
    }
    if (!json.at("regions").is_array()) {
        throw std::runtime_error(
            context + " regions must be a string array");
    }
    std::vector<std::string> result;
    result.reserve(json.at("regions").size());
    std::set<std::string, std::less<>> unique;
    for (const auto &encoded : json.at("regions")) {
        if (!encoded.is_string()) {
            throw std::runtime_error(
                context + " regions must be a string array");
        }
        auto region = encoded.get<std::string>();
        if (region.empty() ||
            region.size() > maximumLogicalRegionTagBytes) {
            throw std::runtime_error(
                context + " region tag is empty or too long");
        }
        if (!unique.insert(region).second) {
            throw std::runtime_error(
                context + " has duplicate region tag: " + region);
        }
        result.push_back(std::move(region));
    }
    return result;
}

std::string parseRenderViewFamilyId(
    const nlohmann::json &json,
    const std::string &context) {
    auto family_id =
        std::string{mainRenderViewFamilyId};
    if (json.contains("view_family")) {
        if (!json.at("view_family").is_string()) {
            throw std::runtime_error(
                context +
                " view_family must be a string");
        }
        family_id =
            json.at("view_family")
                .get<std::string>();
    }
    validateRenderViewFamilyId(
        family_id, context);
    return family_id;
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

std::array<double, 4>
jsonToClearColor(const nlohmann::json &json) {
    if (!json.is_array() || json.size() != 4) {
        throw std::runtime_error("clear_color must be an array of four numbers");
    }
    for (const auto &value_json : json) {
        if (!value_json.is_number()) {
            throw std::runtime_error("clear_color must be an array of four numbers");
        }
    }

    return {
        json.at(0).get<double>(),
        json.at(1).get<double>(),
        json.at(2).get<double>(),
        json.at(3).get<double>(),
    };
}

} // namespace Pelican
