#include "renderingpasstargetjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetcontainer.hpp"
#include <stdexcept>
#include <string>

namespace Pelican {

namespace {

GlobalRenderTargetId resolveRenderTarget(RenderTargetContainer &rt_container, const std::string &name,
                                         const std::string &role) {
    const auto rt_id = rt_container.getRenderTargetIdByName(name);
    if (!isConcreteRenderTarget(rt_id)) {
        throw std::runtime_error(role + " render target not found: " + name);
    }
    return rt_id;
}

} // namespace

std::vector<GlobalRenderTargetId> parseColorOutputTargetsFromJson(RenderTargetContainer &rt_container,
                                                                  nlohmann::json color_output) {
    if (color_output.is_null()) {
        return {};
    }
    if (!color_output.is_array()) {
        color_output = nlohmann::json::array({color_output});
    }

    std::vector<GlobalRenderTargetId> output_color;
    for (const auto &color_name_json : color_output) {
        if (!color_name_json.is_string()) {
            throw std::runtime_error("Color output must be a render target name");
        }
        const std::string color_name = color_name_json;
        validateName(color_name, "Color output target");
        if (color_name == "swapchain") {
            output_color.push_back(swapchainRenderTargetId());
        } else {
            output_color.push_back(resolveRenderTarget(rt_container, color_name, "Color"));
        }
    }
    return output_color;
}

GlobalRenderTargetId parseDepthOutputTargetFromJson(RenderTargetContainer &rt_container,
                                                    const nlohmann::json &depth_output) {
    if (depth_output.is_null()) {
        return noRenderTargetId();
    }
    if (!depth_output.is_string()) {
        throw std::runtime_error("Depth output must be null or a render target name");
    }

    const std::string depth_name = depth_output.get<std::string>();
    validateName(depth_name, "Depth output target");
    if (depth_name == "swapchain") {
        throw std::runtime_error("Depth output target cannot be swapchain");
    }
    return resolveRenderTarget(rt_container, depth_name, "Depth");
}

std::vector<GlobalRenderTargetId> parseInputTargetsFromJson(RenderTargetContainer &rt_container,
                                                            nlohmann::json input_output) {
    if (input_output.is_null()) {
        return {};
    }
    if (!input_output.is_array()) {
        input_output = nlohmann::json::array({input_output});
    }

    std::vector<GlobalRenderTargetId> input_targets;
    for (const auto &input_name_json : input_output) {
        if (!input_name_json.is_string()) {
            throw std::runtime_error("Input target must be a render target name");
        }
        const std::string input_name = input_name_json.get<std::string>();
        validateName(input_name, "Input target");
        if (input_name == "swapchain") {
            throw std::runtime_error("Input target cannot be swapchain");
        }
        input_targets.push_back(resolveRenderTarget(rt_container, input_name, "Input"));
    }
    return input_targets;
}

} // namespace Pelican
