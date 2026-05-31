#include "renderingpassjsonloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassdefinitionjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetcontainer.hpp"
#include "rendertargetjsonparser.hpp"
#include "../loader/basicconfig.hpp"
#include "../shader/shadercontainer.hpp"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>

namespace Pelican {

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

    registerRenderTargetsFromJson(rendering_pass_data, base_extent, rt_container);

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
            parseRenderingPassDefinitionFromJson(pass_set_json, rt_container, shader_container));
    }
}

} // namespace Pelican
