#include "renderingpassjsonloader.hpp"
#include "renderingpasscontainer.hpp"
#include "renderingpassconfigloader.hpp"
#include "rendertargetcontainer.hpp"
#include "../loader/basicconfig.hpp"
#include <cstdint>
#include <utility>

namespace Pelican {

void RenderingPassJsonLoader::registerRenderingPassesFromJson(const std::string &json_path) const {
    auto &config = GET_MODULE(ProjectBasicConfig);
    auto &rt_container = GET_MODULE(RenderTargetContainer);
    auto &pass_container = GET_MODULE(RenderingPassContainer);

    const auto window_size = config.initialWindowSize();
    const vk::Extent2D base_extent{
        static_cast<uint32_t>(window_size.width),
        static_cast<uint32_t>(window_size.height),
    };

    auto compiled_passes = loadCompiledRenderingPassesFromJson(json_path, base_extent, rt_container);
    for (auto &compiled_pass : compiled_passes) {
        pass_container.registerCompiledRenderingPass(std::move(compiled_pass));
    }
}

} // namespace Pelican
