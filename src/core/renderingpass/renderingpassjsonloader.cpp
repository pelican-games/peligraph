#include "renderingpassjsonloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "renderingpassvalidation.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../shader/shadercontainer.hpp"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Pelican {

namespace {

GlobalShaderId registerShaderFromFile(ShaderContainer &shader_container, const std::string &path) {
    const auto data = readBinaryFile(path);
    return shader_container.registerShader(data.size(), data.data());
}

GlobalRenderTargetId resolveRenderTarget(RenderTargetContainer &rt_container, const std::string &name,
                                         const std::string &role) {
    const auto rt_id = rt_container.getRenderTargetIdByName(name);
    if (!isConcreteRenderTarget(rt_id)) {
        throw std::runtime_error(role + " render target not found: " + name);
    }
    return rt_id;
}

void registerRenderTargets(const nlohmann::json &data, vk::Extent2D base_extent,
                           RenderTargetContainer &rt_container) {
    if (!data.contains("render_targets")) {
        return;
    }

    const auto &render_targets = data.at("render_targets");
    if (!render_targets.is_array()) {
        throw std::runtime_error("render_targets must be an array");
    }

    std::unordered_set<std::string> render_target_names;
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

        const vk::Extent2D extent{
            static_cast<uint32_t>(base_extent.width * extent_scale),
            static_cast<uint32_t>(base_extent.height * extent_scale),
        };

        if (extent.width == 0 || extent.height == 0) {
            throw std::runtime_error("Render target extent became zero-sized: " + name);
        }

        rt_container.registerRenderTarget(name, extent, stringToFormat(format_str), stringToUsageFlags(usage_strs),
                                          vma::MemoryUsage::eAutoPreferDevice);
    }
}

std::vector<GlobalRenderTargetId> parseColorOutputs(RenderTargetContainer &rt_container,
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

GlobalRenderTargetId parseDepthOutput(RenderTargetContainer &rt_container, const nlohmann::json &depth_output) {
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

std::vector<GlobalRenderTargetId> parseInputTargets(RenderTargetContainer &rt_container,
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

void parseMaterialInfo(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial() || !pass_json.contains("material_range")) {
        return;
    }

    const auto &mat_range = pass_json.at("material_range");
    if (!mat_range.is_object()) {
        throw std::runtime_error("material_range must be an object: " + pass_def.name);
    }

    auto &materialInfo = pass_def.materialInfo();
    materialInfo.material_start = parseUint32Field(mat_range, "start", "material_range in pass: " + pass_def.name);
    materialInfo.material_count = parseUint32Field(mat_range, "count", "material_range in pass: " + pass_def.name);
}

void parseFullscreenInfo(PassDefinition &pass_def, const nlohmann::json &pass_json,
                         ShaderContainer &shader_container) {
    if (!pass_def.isFullscreen()) {
        return;
    }

    auto &fullscreenInfo = pass_def.fullscreenInfo();
    if (pass_json.contains("push_constants")) {
        if (!pass_json.at("push_constants").is_string()) {
            throw std::runtime_error("Fullscreen pass push_constants must be a string: " + pass_def.name);
        }
        fullscreenInfo.push_constants =
            stringToFullscreenPushConstantData(pass_json.at("push_constants").get<std::string>());
    } else if (pass_json.contains("needs_projection_matrix")) {
        if (!pass_json.at("needs_projection_matrix").is_boolean()) {
            throw std::runtime_error("Fullscreen pass needs_projection_matrix must be a boolean: " + pass_def.name);
        }
        if (pass_json.at("needs_projection_matrix").get<bool>()) {
            fullscreenInfo.push_constants = FullscreenPushConstantData::eProjectionView;
        }
    } else if (pass_def.name == "lighting_pass") {
        // Keep old project configs working until they declare push_constants explicitly.
        fullscreenInfo.push_constants = FullscreenPushConstantData::eCameraPosition;
    }
    if (pass_json.contains("uses_light_data")) {
        if (!pass_json.at("uses_light_data").is_boolean()) {
            throw std::runtime_error("Fullscreen pass uses_light_data must be a boolean: " + pass_def.name);
        }
        fullscreenInfo.uses_light_data = pass_json.at("uses_light_data").get<bool>();
    } else if (pass_def.name == "lighting_pass") {
        // Keep old project configs working until they declare uses_light_data explicitly.
        fullscreenInfo.uses_light_data = true;
    }
    if (!pass_json.contains("shader")) {
        throw std::runtime_error("Fullscreen pass requires shader: " + pass_def.name);
    }

    const auto &shader = pass_json.at("shader");
    if (!shader.is_object()) {
        throw std::runtime_error("Fullscreen pass shader must be an object: " + pass_def.name);
    }
    if (!shader.contains("vertex") || !shader.contains("fragment")) {
        throw std::runtime_error("Fullscreen pass shader requires vertex and fragment: " + pass_def.name);
    }
    if (!shader.at("vertex").is_string() || !shader.at("fragment").is_string()) {
        throw std::runtime_error("Fullscreen pass shader paths must be strings: " + pass_def.name);
    }

    fullscreenInfo.vert_shader = registerShaderFromFile(shader_container, shader.at("vertex").get<std::string>());
    fullscreenInfo.frag_shader = registerShaderFromFile(shader_container, shader.at("fragment").get<std::string>());
}

void applyPassDefaults(PassDefinition &pass_def) {
    if (pass_def.isUi()) {
        pass_def.color_load_op = vk::AttachmentLoadOp::eLoad;
    }
}

PassDefinition parsePassDefinition(const nlohmann::json &pass_json, RenderTargetContainer &rt_container,
                                   ShaderContainer &shader_container) {
    if (!pass_json.is_object()) {
        throw std::runtime_error("passes entries must be objects");
    }

    PassDefinition pass_def;
    pass_def.name = parseStringField(pass_json, "name", "pass");
    validateName(pass_def.name, "Pass");
    pass_def.pass_info = makePassInfo(parseStringField(pass_json, "type", "pass: " + pass_def.name));
    applyPassDefaults(pass_def);

    if (!pass_json.contains("output")) {
        throw std::runtime_error("Pass requires output field: " + pass_def.name);
    }
    const auto &output = pass_json.at("output");
    if (!output.is_object()) {
        throw std::runtime_error("Pass output must be an object: " + pass_def.name);
    }
    if (!output.contains("color") || !output.contains("depth")) {
        throw std::runtime_error("Pass output requires color and depth fields: " + pass_def.name);
    }
    pass_def.output_color = parseColorOutputs(rt_container, output.at("color"));
    pass_def.output_depth = parseDepthOutput(rt_container, output.at("depth"));

    validatePassOutputs(pass_def);
    validatePassSpecificFields(pass_def, pass_json);

    parseMaterialInfo(pass_def, pass_json);

    if (pass_json.contains("input")) {
        pass_def.input_targets = parseInputTargets(rt_container, pass_json.at("input"));
    }
    validatePassInputs(pass_def);
    validatePassTargetUsage(pass_def, rt_container);
    validateUniqueRenderTargets(pass_def.output_color, "color output", pass_def, rt_container);
    validateUniqueRenderTargets(pass_def.input_targets, "input", pass_def, rt_container);
    validatePassOutputExtents(pass_def, rt_container);
    validateMaterialPassAttachments(pass_def, rt_container);

    if (pass_json.contains("clear_color")) {
        pass_def.clear_color = jsonToClearColor(pass_json.at("clear_color"));
    }
    if (pass_json.contains("color_load_op")) {
        pass_def.color_load_op =
            stringToLoadOp(parseStringField(pass_json, "color_load_op", "pass: " + pass_def.name));
    }
    if (pass_json.contains("color_store_op")) {
        pass_def.color_store_op =
            stringToStoreOp(parseStringField(pass_json, "color_store_op", "pass: " + pass_def.name));
    }

    parseFullscreenInfo(pass_def, pass_json, shader_container);
    return pass_def;
}

RenderingPassDefinition parseRenderingPassDefinition(const nlohmann::json &pass_set_json,
                                                     RenderTargetContainer &rt_container,
                                                     ShaderContainer &shader_container) {
    if (!pass_set_json.is_object()) {
        throw std::runtime_error("rendering_passes entries must be objects");
    }

    RenderingPassDefinition pass_def;
    pass_def.name = parseStringField(pass_set_json, "name", "rendering pass");
    validateName(pass_def.name, "Rendering pass");

    if (!pass_set_json.contains("passes")) {
        throw std::runtime_error("Rendering pass requires passes array: " + pass_def.name);
    }
    const auto &passes_json = pass_set_json.at("passes");
    if (!passes_json.is_array()) {
        throw std::runtime_error("Rendering pass requires passes array: " + pass_def.name);
    }

    std::unordered_set<std::string> pass_names;
    ProducedColorTargetSet produced_color_targets;
    for (const auto &pass_json : passes_json) {
        const std::string pass_name = parseStringField(pass_json, "name", "pass");
        validateName(pass_name, "Pass");
        if (!pass_names.insert(pass_name).second) {
            throw std::runtime_error("Duplicate pass name: " + pass_name);
        }

        auto parsed_pass = parsePassDefinition(pass_json, rt_container, shader_container);
        validatePassInputsProduced(parsed_pass, produced_color_targets, rt_container);
        recordPassOutputs(parsed_pass, produced_color_targets);
        pass_def.passes.push_back(std::move(parsed_pass));
    }

    return pass_def;
}

} // namespace

void RenderingPassJsonLoader::registerRenderingPassesFromJson(const std::string &json_path) const {
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &shader_container = GET_MODULE(ShaderContainer);
    auto &pass_container = GET_MODULE(RenderingPassContainer);

    const auto rendering_pass_data = nlohmann::json::parse(readBinaryFile(json_path));
    if (!rendering_pass_data.is_object()) {
        throw std::runtime_error("Rendering config must be an object: " + json_path);
    }

    const auto window_size = config.initialWindowSize();
    const vk::Extent2D base_extent{
        static_cast<uint32_t>(window_size.width),
        static_cast<uint32_t>(window_size.height),
    };

    registerRenderTargets(rendering_pass_data, base_extent, rt_container);

    if (!rendering_pass_data.contains("rendering_passes")) {
        return;
    }

    const auto &rendering_passes = rendering_pass_data.at("rendering_passes");
    if (!rendering_passes.is_array()) {
        throw std::runtime_error("rendering_passes must be an array");
    }

    std::unordered_set<std::string> rendering_pass_names;
    for (const auto &pass_set_json : rendering_passes) {
        if (!pass_set_json.is_object()) {
            throw std::runtime_error("rendering_passes entries must be objects");
        }

        const std::string rendering_pass_name = parseStringField(pass_set_json, "name", "rendering pass");
        validateName(rendering_pass_name, "Rendering pass");
        if (!rendering_pass_names.insert(rendering_pass_name).second) {
            throw std::runtime_error("Duplicate rendering pass name: " + rendering_pass_name);
        }
        pass_container.registerRenderingPass(
            parseRenderingPassDefinition(pass_set_json, rt_container, shader_container));
    }
}

} // namespace Pelican
