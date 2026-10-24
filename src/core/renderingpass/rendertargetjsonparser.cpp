#include "rendertargetjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "../../project/renderresourcename.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Pelican {

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

} // namespace Pelican
