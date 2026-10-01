#include "renderingpassjsonhelpers.hpp"
#include "passfieldownershipcapabilities.hpp"
#include "../../project/logicalrendergraph.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vulkan/vulkan_format_traits.hpp>

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

// F-1: the one table of usage names the parser accepts. Studio's render
// target form offers exactly these, read from the engine rather than copied.
constexpr std::array image_usage_names{
    std::pair{"COLOR_ATTACHMENT", vk::ImageUsageFlagBits::eColorAttachment},
    std::pair{"DEPTH_STENCIL_ATTACHMENT",
              vk::ImageUsageFlagBits::eDepthStencilAttachment},
    std::pair{"SAMPLED", vk::ImageUsageFlagBits::eSampled},
    std::pair{"STORAGE", vk::ImageUsageFlagBits::eStorage},
    std::pair{"TRANSFER_DST", vk::ImageUsageFlagBits::eTransferDst},
    std::pair{"TRANSFER_SRC", vk::ImageUsageFlagBits::eTransferSrc},
};

} // namespace

std::vector<std::string> imageFormatNames() {
    std::vector<std::string> names;
    names.reserve(format_names.size());
    for (const auto &entry : format_names) names.emplace_back(entry.first);
    return names;
}

std::vector<std::pair<std::string, vk::Format>> imageFormats() {
    std::vector<std::pair<std::string, vk::Format>> formats;
    formats.reserve(format_names.size());
    for (const auto &[name, format] : format_names) formats.emplace_back(name, format);
    return formats;
}

std::string_view imageFormatAspectName(vk::Format format) {
    bool depth = false;
    bool stencil = false;
    for (std::uint8_t component = 0; component < vk::componentCount(format); ++component) {
        const std::string_view name = vk::componentName(format, component);
        depth = depth || name == "D";
        stencil = stencil || name == "S";
    }
    if (depth) return stencil ? "depth_stencil" : "depth";
    return stencil ? "stencil" : "color";
}

std::vector<std::string> imageUsageFlagNames() {
    std::vector<std::string> names;
    names.reserve(image_usage_names.size());
    for (const auto &entry : image_usage_names) names.emplace_back(entry.first);
    return names;
}

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
    for (const auto &usage_str : usage_strs) {
        const auto it = std::find_if(
            image_usage_names.begin(), image_usage_names.end(),
            [&](const auto &entry) { return entry.first == usage_str; });
        if (it != image_usage_names.end()) {
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

std::string parseStringField(const nlohmann::json &json, std::string_view field_name,
                             std::string_view context) {
    if (!json.contains(field_name) || !json.at(field_name).is_string()) {
        throw std::runtime_error(std::string{context} + " requires string field: " +
                                 std::string{field_name});
    }
    return json.at(field_name).get<std::string>();
}

std::vector<std::string> parseStringList(const nlohmann::json &json,
                                         std::string_view context) {
    if (json.is_null()) {
        return {};
    }
    if (json.is_string()) {
        return {json.get<std::string>()};
    }
    if (!json.is_array()) {
        throw std::runtime_error(std::string{context} + " must be a string or string array");
    }

    std::vector<std::string> values;
    values.reserve(json.size());
    for (const auto &entry : json) {
        if (!entry.is_string()) {
            throw std::runtime_error(std::string{context} + " entries must be strings");
        }
        values.push_back(entry.get<std::string>());
    }
    return values;
}

std::vector<std::string> parseOptionalStringList(const nlohmann::json &json,
                                                 std::string_view field,
                                                 std::string_view context) {
    if (!json.contains(field)) {
        return {};
    }
    return parseStringList(json.at(field), std::string{context} + "." + std::string{field});
}

void appendUnique(std::vector<std::string> &values, std::string value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(std::move(value));
    }
}

void appendUnique(std::vector<std::string> &values,
                  const std::vector<std::string> &more_values) {
    for (const auto &value : more_values) {
        appendUnique(values, value);
    }
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

RenderViewSelect parseRenderViewSelect(
    const nlohmann::json &json,
    const std::string &context) {
    RenderViewSelect result;
    const auto found = json.find("view_select");
    if (found == json.end()) {
        return result;
    }
    if (!found->is_object() || found->empty()) {
        throw std::runtime_error(
            context +
            " view_select must be a non-empty object of axis names "
            "to non-negative integers");
    }
    for (auto entry = found->begin(); entry != found->end();
         ++entry) {
        if (entry.key().empty()) {
            throw std::runtime_error(
                context +
                " view_select has an empty axis name");
        }
        const auto &value = entry.value();
        if (!value.is_number_integer() ||
            (value.is_number_unsigned()
                 ? value.get<std::uint64_t>() >
                       std::numeric_limits<std::uint32_t>::max()
                 : value.get<std::int64_t>() < 0 ||
                       value.get<std::int64_t>() >
                           static_cast<std::int64_t>(
                               std::numeric_limits<
                                   std::uint32_t>::max()))) {
            throw std::runtime_error(
                context + " view_select axis '" + entry.key() +
                "' must be a non-negative 32-bit integer");
        }
        result.emplace(
            entry.key(),
            static_cast<std::uint32_t>(
                value.is_number_unsigned()
                    ? value.get<std::uint64_t>()
                    : static_cast<std::uint64_t>(
                          value.get<std::int64_t>())));
    }
    return result;
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
