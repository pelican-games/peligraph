#include "renderingpassjsonloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassjsonhelpers.hpp"
#include "rendertargetcontainer.hpp"
#include "rendertargetjsonparser.hpp"
#include "../loader/basicconfig.hpp"
#include "../shader/shadercontainer.hpp"
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>

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

    const auto pass_definitions =
        parseRenderingPassDefinitionsFromConfigJson(rendering_pass_data, rt_container, shader_container);
    for (const auto &pass_definition : pass_definitions) {
        pass_container.registerRenderingPass(pass_definition);
    }
}

} // namespace Pelican
