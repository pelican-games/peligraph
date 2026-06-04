#include "renderingpassconfigloader.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "rendertargetjsonparser.hpp"
#include "../loader/fileio.hpp"
#include "../profiler.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {

std::vector<RenderingPassDefinition> loadRenderingPassDefinitionsFromJson(const std::string &json_path,
                                                                          vk::Extent2D base_extent,
                                                                          RenderTargetContainer &rt_container) {
    ScopedLogTimer timer{"load rendering pass definitions from json"};

    const auto rendering_pass_data = nlohmann::json::parse(readBinaryFile(json_path));
    if (!rendering_pass_data.is_object()) {
        throw std::runtime_error("Rendering config must be an object: " + json_path);
    }

    const auto render_target_definitions = parseRenderTargetDefinitionsFromJson(rendering_pass_data, base_extent);
    registerRenderTargetDefinitions(render_target_definitions, rt_container);

    return parseRenderingPassDefinitionsFromConfigJson(rendering_pass_data, rt_container);
}

} // namespace Pelican
