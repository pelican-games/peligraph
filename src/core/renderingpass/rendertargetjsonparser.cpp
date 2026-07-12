#include "rendertargetjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <array>

namespace Pelican {

namespace {

std::optional<vk::Extent2D> parseFixedExtent(const nlohmann::json &rt_json,
                                             const std::string &name) {
    const bool has_width = rt_json.contains("width");
    const bool has_height = rt_json.contains("height");
    if (!has_width && !has_height) {
        return std::nullopt;
    }
    if (!has_width || !has_height) {
        throw std::runtime_error("Render target fixed extent requires width and height: " + name);
    }
    const auto width = parseUint32Field(rt_json, "width", "render target: " + name);
    const auto height = parseUint32Field(rt_json, "height", "render target: " + name);
    if (width == 0 || height == 0) {
        throw std::runtime_error("Render target fixed extent must be positive: " + name);
    }
    return vk::Extent2D{width, height};
}

vk::ClearColorValue parseHistoryClearColor(const nlohmann::json &rt_json,
                                           const std::string &name) {
    if (!rt_json.contains("clear_color")) {
        return vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 0.0f}};
    }
    const auto &value = rt_json.at("clear_color");
    if (!value.is_array() || value.size() != 4) {
        throw std::runtime_error("Render target clear_color must contain four numbers: " + name);
    }
    std::array<float, 4> color{};
    for (size_t i = 0; i < color.size(); ++i) {
        if (!value.at(i).is_number()) {
            throw std::runtime_error("Render target clear_color must contain four numbers: " + name);
        }
        color[i] = value.at(i).get<float>();
    }
    return vk::ClearColorValue{color};
}

} // namespace

nlohmann::json resolveRenderTargetFormatClassesV2(const nlohmann::json &data,
                                                   vk::Format frame_target_format,
                                                   vk::Extent2D frame_target_extent,
                                                   bool hdr_enabled) {
    if (!data.contains("resolver_version") || !data.at("resolver_version").is_number_integer() ||
        data.at("resolver_version").get<int>() != 2) {
        throw std::runtime_error("Rendering config requires resolver_version: 2");
    }
    auto resolved = data;
    if (!resolved.contains("render_targets")) {
        return resolved;
    }
    auto &targets = resolved.at("render_targets");
    if (!targets.is_array()) {
        throw std::runtime_error("render_targets must be an array");
    }
    for (auto &target : targets) {
        const auto name = parseStringField(target, "name", "render target");
        const auto format_class = parseStringField(target, "format_class", "render target: " + name);
        const bool explicit_class = format_class.rfind("explicit(", 0) == 0 &&
                                    format_class.size() > 10 && format_class.back() == ')';
        if (format_class != "scene" && format_class != "display" && format_class != "data" &&
            !explicit_class) {
            throw std::runtime_error("Unknown render target format_class: " + format_class);
        }
        if (format_class == "display") {
            target["format"] = "B8G8R8A8_SRGB";
            target["width"] = frame_target_extent.width;
            target["height"] = frame_target_extent.height;
        } else if (format_class == "scene") {
            target["format"] = hdr_enabled ? "R16G16B16A16_SFLOAT" : "B8G8R8A8_SRGB";
        } else {
            // Data and explicit resources retain their authored representation.
            (void)parseStringField(target, "format", "render target: " + name);
        }
    }
    return resolved;
}

std::vector<RenderTargetDefinition> parseRenderTargetDefinitionsFromJson(const nlohmann::json &data) {
    std::vector<RenderTargetDefinition> definitions;
    if (!data.contains("render_targets")) {
        return definitions;
    }

    const auto &render_targets = data.at("render_targets");
    if (!render_targets.is_array()) {
        throw std::runtime_error("render_targets must be an array");
    }

    std::unordered_set<std::string> render_target_names;
    definitions.reserve(render_targets.size());
    for (const auto &rt_json : render_targets) {
        if (!rt_json.is_object()) {
            throw std::runtime_error("render_targets entries must be objects");
        }

        const std::string name = parseStringField(rt_json, "name", "render target");
        validateName(name, "Render target");
        if (name == "swapchain") {
            throw std::runtime_error("Render target name is reserved: swapchain");
        }
        if (!render_target_names.insert(name).second) {
            throw std::runtime_error("Duplicate render target name: " + name);
        }
        const float extent_scale = parseFloatField(rt_json, "extent_scale", "render target: " + name);
        const auto fixed_extent = parseFixedExtent(rt_json, name);
        const std::string format_str = parseStringField(rt_json, "format", "render target: " + name);
        const std::string format_class = rt_json.value(
            "format_class", "explicit(" + format_str + ")");
        const std::string role = rt_json.value(
            "role", (format_class == "scene" || format_class == "display") ? "color" : "data");
        if (role != "color" && role != "data") {
            throw std::runtime_error("Render target role must be color or data: " + name);
        }
        const std::vector<std::string> usage_strs = parseStringArrayField(rt_json, "usage", "render target: " + name);
        if (rt_json.contains("history") && !rt_json.at("history").is_boolean()) {
            throw std::runtime_error("Render target history must be a boolean: " + name);
        }
        const bool history = rt_json.value("history", false);
        const auto history_clear_color = parseHistoryClearColor(rt_json, name);

        if (extent_scale <= 0.0f) {
            throw std::runtime_error("Render target extent_scale must be positive: " + name);
        }

        definitions.push_back(RenderTargetDefinition{
            name,
            format_class,
            role,
            extent_scale,
            fixed_extent,
            stringToFormat(format_str),
            stringToUsageFlags(usage_strs),
            history,
            history_clear_color,
        });
    }

    return definitions;
}

} // namespace Pelican
