#include "renderingpassconfigloader.hpp"
#include "renderingpassconfigjsonparser.hpp"
#include "renderingpassruntimecompiler.hpp"
#include "rendertargetjsonparser.hpp"
#include "../loader/fileio.hpp"
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {

std::vector<CompiledRenderingPass> loadCompiledRenderingPassesFromJson(const std::string &json_path,
                                                                       vk::Extent2D base_extent,
                                                                       RenderTargetContainer &rt_container) {
    const auto rendering_pass_data = nlohmann::json::parse(readBinaryFile(json_path));
    if (!rendering_pass_data.is_object()) {
        throw std::runtime_error("Rendering config must be an object: " + json_path);
    }

    registerRenderTargetsFromJson(rendering_pass_data, base_extent, rt_container);

    const auto pass_definitions =
        parseRenderingPassDefinitionsFromConfigJson(rendering_pass_data, rt_container);

    std::vector<CompiledRenderingPass> compiled_passes;
    compiled_passes.reserve(pass_definitions.size());
    for (const auto &pass_definition : pass_definitions) {
        compiled_passes.push_back(compileRenderingPassRuntime(pass_definition));
    }

    return compiled_passes;
}

} // namespace Pelican
