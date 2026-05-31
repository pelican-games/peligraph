#include "renderingpassjsonloader.hpp"
#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../shader/shadercontainer.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Pelican {

namespace {

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

vk::ClearColorValue jsonToClearColor(const nlohmann::json &json) {
    if (!json.is_array() || json.size() != 4) {
        throw std::runtime_error("clear_color must be an array of four floats");
    }

    return vk::ClearColorValue{std::array{
        json.at(0).get<float>(),
        json.at(1).get<float>(),
        json.at(2).get<float>(),
        json.at(3).get<float>(),
    }};
}

std::string readBinaryFile(const std::string &path) {
    const auto sz = std::filesystem::file_size(path);
    std::ifstream file{path, std::ios_base::binary};
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path);
    }

    std::string data;
    data.resize(sz, '\0');
    file.read(data.data(), sz);
    return data;
}

GlobalShaderId registerShaderFromFile(ShaderContainer &shader_container, const std::string &path) {
    const auto data = readBinaryFile(path);
    return shader_container.registerShader(data.size(), data.data());
}

GlobalRenderTargetId resolveRenderTarget(RenderTargetContainer &rt_container, const std::string &name,
                                         const std::string &role) {
    const auto rt_id = rt_container.getRenderTargetIdByName(name);
    if (rt_id.value < 0) {
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
        const std::string name = rt_json.at("name");
        if (!render_target_names.insert(name).second) {
            throw std::runtime_error("Duplicate render target name: " + name);
        }
        const float extent_scale = rt_json.at("extent_scale");
        const std::string format_str = rt_json.at("format");
        const std::vector<std::string> usage_strs = rt_json.at("usage");

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
        if (color_name == "swapchain") {
            output_color.push_back(GlobalRenderTargetId{-2});
        } else {
            output_color.push_back(resolveRenderTarget(rt_container, color_name, "Color"));
        }
    }
    return output_color;
}

GlobalRenderTargetId parseDepthOutput(RenderTargetContainer &rt_container, const nlohmann::json &depth_output) {
    if (depth_output.is_null()) {
        return GlobalRenderTargetId{-1};
    }
    if (!depth_output.is_string()) {
        throw std::runtime_error("Depth output must be null or a render target name");
    }

    return resolveRenderTarget(rt_container, depth_output.get<std::string>(), "Depth");
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
        input_targets.push_back(resolveRenderTarget(rt_container, input_name, "Input"));
    }
    return input_targets;
}

void validatePassInputs(const PassDefinition &pass_def) {
    if (!pass_def.input_targets.empty() && !pass_def.isFullscreen()) {
        throw std::runtime_error("Only fullscreen passes support input targets: " + pass_def.name);
    }
}

void validatePassOutputs(const PassDefinition &pass_def) {
    if (pass_def.output_color.empty() && pass_def.output_depth.value < 0) {
        throw std::runtime_error("Pass must output color or depth: " + pass_def.name);
    }

    if (pass_def.isFullscreen()) {
        if (pass_def.output_color.size() != 1) {
            throw std::runtime_error("Fullscreen pass requires exactly one color output: " + pass_def.name);
        }
        if (pass_def.output_depth.value >= 0) {
            throw std::runtime_error("Fullscreen pass does not support depth output: " + pass_def.name);
        }
    }

    if (pass_def.isUi()) {
        if (pass_def.output_color.size() != 1) {
            throw std::runtime_error("UI pass requires exactly one color output: " + pass_def.name);
        }
        if (pass_def.output_depth.value >= 0) {
            throw std::runtime_error("UI pass does not support depth output: " + pass_def.name);
        }
    }
}

void validatePassSpecificFields(const PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial() && pass_json.contains("material_range")) {
        throw std::runtime_error("Only material passes support material_range: " + pass_def.name);
    }

    if (pass_def.isFullscreen()) {
        return;
    }

    static constexpr std::array fullscreen_fields{
        "shader",
        "push_constants",
        "uses_light_data",
        "needs_projection_matrix",
    };
    for (const char *field : fullscreen_fields) {
        if (pass_json.contains(field)) {
            throw std::runtime_error("Only fullscreen passes support " + std::string{field} + ": " + pass_def.name);
        }
    }
}

void parseMaterialInfo(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (!pass_def.isMaterial() || !pass_json.contains("material_range")) {
        return;
    }

    const auto &mat_range = pass_json.at("material_range");
    auto &materialInfo = pass_def.materialInfo();
    materialInfo.material_start = mat_range.at("start");
    materialInfo.material_count = mat_range.at("count");
}

void parseFullscreenInfo(PassDefinition &pass_def, const nlohmann::json &pass_json,
                         ShaderContainer &shader_container) {
    if (!pass_def.isFullscreen()) {
        return;
    }

    auto &fullscreenInfo = pass_def.fullscreenInfo();
    if (pass_json.contains("push_constants")) {
        fullscreenInfo.push_constants =
            stringToFullscreenPushConstantData(pass_json.at("push_constants").get<std::string>());
    } else if (pass_json.contains("needs_projection_matrix") &&
               pass_json.at("needs_projection_matrix").is_boolean() &&
               pass_json.at("needs_projection_matrix").get<bool>()) {
        fullscreenInfo.push_constants = FullscreenPushConstantData::eProjectionView;
    } else if (pass_def.name == "lighting_pass") {
        // Keep old project configs working until they declare push_constants explicitly.
        fullscreenInfo.push_constants = FullscreenPushConstantData::eCameraPosition;
    }
    if (pass_json.contains("uses_light_data")) {
        fullscreenInfo.uses_light_data = pass_json.at("uses_light_data").get<bool>();
    } else if (pass_def.name == "lighting_pass") {
        // Keep old project configs working until they declare uses_light_data explicitly.
        fullscreenInfo.uses_light_data = true;
    }
    if (!pass_json.contains("shader")) {
        throw std::runtime_error("Fullscreen pass requires shader: " + pass_def.name);
    }

    const auto &shader = pass_json.at("shader");
    if (!shader.contains("vertex") || !shader.contains("fragment")) {
        throw std::runtime_error("Fullscreen pass shader requires vertex and fragment: " + pass_def.name);
    }

    fullscreenInfo.vert_shader = registerShaderFromFile(shader_container, shader.at("vertex"));
    fullscreenInfo.frag_shader = registerShaderFromFile(shader_container, shader.at("fragment"));
}

PassDefinition parsePassDefinition(const nlohmann::json &pass_json, RenderTargetContainer &rt_container,
                                   ShaderContainer &shader_container) {
    PassDefinition pass_def;
    pass_def.name = pass_json.at("name");
    pass_def.pass_info = makePassInfo(pass_json.at("type"));

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

    if (pass_json.contains("clear_color")) {
        pass_def.clear_color = jsonToClearColor(pass_json.at("clear_color"));
    }
    if (pass_json.contains("color_load_op")) {
        pass_def.color_load_op = stringToLoadOp(pass_json.at("color_load_op").get<std::string>());
    }
    if (pass_json.contains("color_store_op")) {
        pass_def.color_store_op = stringToStoreOp(pass_json.at("color_store_op").get<std::string>());
    }

    parseFullscreenInfo(pass_def, pass_json, shader_container);
    return pass_def;
}

RenderingPassDefinition parseRenderingPassDefinition(const nlohmann::json &pass_set_json,
                                                     RenderTargetContainer &rt_container,
                                                     ShaderContainer &shader_container) {
    RenderingPassDefinition pass_def;
    pass_def.name = pass_set_json.at("name");

    const auto &passes_json = pass_set_json.at("passes");
    if (!passes_json.is_array()) {
        throw std::runtime_error("Rendering pass requires passes array: " + pass_def.name);
    }

    std::unordered_set<std::string> pass_names;
    for (const auto &pass_json : passes_json) {
        const std::string pass_name = pass_json.at("name");
        if (!pass_names.insert(pass_name).second) {
            throw std::runtime_error("Duplicate pass name: " + pass_name);
        }
        pass_def.passes.push_back(parsePassDefinition(pass_json, rt_container, shader_container));
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
        const std::string rendering_pass_name = pass_set_json.at("name");
        if (!rendering_pass_names.insert(rendering_pass_name).second) {
            throw std::runtime_error("Duplicate rendering pass name: " + rendering_pass_name);
        }
        pass_container.registerRenderingPass(
            parseRenderingPassDefinition(pass_set_json, rt_container, shader_container));
    }
}

} // namespace Pelican
