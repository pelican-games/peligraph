#include "rendertargetjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace Pelican {

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
        const std::string format_str = parseStringField(rt_json, "format", "render target: " + name);
        const std::vector<std::string> usage_strs = parseStringArrayField(rt_json, "usage", "render target: " + name);

        if (extent_scale <= 0.0f) {
            throw std::runtime_error("Render target extent_scale must be positive: " + name);
        }

        definitions.push_back(RenderTargetDefinition{
            name,
            extent_scale,
            stringToFormat(format_str),
            stringToUsageFlags(usage_strs),
        });
    }

    return definitions;
}

} // namespace Pelican
