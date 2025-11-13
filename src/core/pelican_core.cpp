#include "pelican_core.hpp"
#include "appflow/loop.hpp"
#include "log.hpp"

#include "loader/projectsrc.hpp"
#include "model/gltf.hpp"
#include "renderer/camera.hpp"
#include "renderer/polygoninstancecontainer.hpp"

#include "battery/embed.hpp"

namespace Pelican {

void PelicanCore::run() {
    try {
        setupLogger();

        DependencyContainer container;
        container.get<ProjectSource>().setSourceByData(b::embed<"default_config.json">().str());

        auto &loop = container.get<Loop>();
        loop.run();
    } catch (std::exception &e) {
        LOG_ERROR(logger, "Pelican fatal error : {}", e.what());
        return;
    }
}

} // namespace Pelican
