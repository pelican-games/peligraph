#include "renderingpassjsonloader.hpp"
#include "renderingpasscontainer.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/basicconfig.hpp"
#include "../shader/shadercontainer.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unordered_map>
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
        }
    }
    return flags;
}

PassType stringToPassType(const std::string &type_str) {
    if (type_str == "material") {
        return PassType::eMaterial;
    }
    if (type_str == "fullscreen") {
        return PassType::eFullscreen;
    }
    if (type_str == "ui") {
        return PassType::eUi;
    }
    throw std::runtime_error("Unknown pass type: " + type_str);
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

    for (const auto &rt_json : data.at("render_targets")) {
        const std::string name = rt_json.at("name");
        const float extent_scale = rt_json.at("extent_scale");
        const std::string format_str = rt_json.at("format");
        const std::vector<std::string> usage_strs = rt_json.at("usage");

        const vk::Extent2D extent{
            static_cast<uint32_t>(base_extent.width * extent_scale),
            static_cast<uint32_t>(base_extent.height * extent_scale),
        };

        rt_container.registerRenderTarget(name, extent, stringToFormat(format_str), stringToUsageFlags(usage_strs),
                                          vma::MemoryUsage::eAutoPreferDevice);
    }
}

std::vector<GlobalRenderTargetId> parseColorOutputs(RenderTargetContainer &rt_container,
                                                    nlohmann::json color_output) {
    if (!color_output.is_array()) {
        color_output = nlohmann::json::array({color_output});
    }

    std::vector<GlobalRenderTargetId> output_color;
    for (const auto &color_name_json : color_output) {
        const std::string color_name = color_name_json;
        if (color_name == "swapchain") {
            output_color.push_back(GlobalRenderTargetId{-2});
        } else {
            output_color.push_back(resolveRenderTarget(rt_container, color_name, "Color"));
        }
    }
    return output_color;
}

std::vector<GlobalRenderTargetId> parseInputTargets(RenderTargetContainer &rt_container,
                                                    nlohmann::json input_output) {
    if (!input_output.is_array()) {
        input_output = nlohmann::json::array({input_output});
    }

    std::vector<GlobalRenderTargetId> input_targets;
    for (const auto &input_name_json : input_output) {
        const std::string input_name = input_name_json;
        input_targets.push_back(resolveRenderTarget(rt_container, input_name, "Input"));
    }
    return input_targets;
}

void parseMaterialInfo(PassDefinition &pass_def, const nlohmann::json &pass_json) {
    if (pass_def.type != PassType::eMaterial || !pass_json.contains("material_range")) {
        return;
    }

    const auto &mat_range = pass_json.at("material_range");
    pass_def.material_info.material_start = mat_range.at("start");
    pass_def.material_info.material_count = mat_range.at("count");
}

void parseFullscreenInfo(PassDefinition &pass_def, const nlohmann::json &pass_json,
                         ShaderContainer &shader_container) {
    if (pass_def.type != PassType::eFullscreen || !pass_json.contains("shader")) {
        return;
    }

    const auto &shader = pass_json.at("shader");
    pass_def.fullscreen_info.vert_shader = registerShaderFromFile(shader_container, shader.at("vertex"));
    pass_def.fullscreen_info.frag_shader = registerShaderFromFile(shader_container, shader.at("fragment"));
}

PassDefinition parsePassDefinition(const nlohmann::json &pass_json, RenderTargetContainer &rt_container,
                                   ShaderContainer &shader_container) {
    PassDefinition pass_def;
    pass_def.name = pass_json.at("name");
    pass_def.type = stringToPassType(pass_json.at("type"));

    const auto &output = pass_json.at("output");
    pass_def.output_color = parseColorOutputs(rt_container, output.at("color"));

    if (!output.at("depth").is_null()) {
        const std::string output_depth_name = output.at("depth");
        pass_def.output_depth = resolveRenderTarget(rt_container, output_depth_name, "Depth");
    } else {
        pass_def.output_depth = GlobalRenderTargetId{-1};
    }

    parseMaterialInfo(pass_def, pass_json);

    if (pass_json.contains("input")) {
        pass_def.input_targets = parseInputTargets(rt_container, pass_json.at("input"));
    }
    if (pass_json.contains("needs_projection_matrix") && pass_json.at("needs_projection_matrix").is_boolean()) {
        pass_def.fullscreen_info.needs_projection_matrix = pass_json.at("needs_projection_matrix");
    }
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

    for (const auto &pass_json : pass_set_json.at("passes")) {
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

    for (const auto &pass_set_json : rendering_pass_data.at("rendering_passes")) {
        pass_container.registerRenderingPass(
            parseRenderingPassDefinition(pass_set_json, rt_container, shader_container));
    }
}

} // namespace Pelican
