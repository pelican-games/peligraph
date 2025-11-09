#include "pelican_core.hpp"
#include "appflow/loop.hpp"
#include "log.hpp"

namespace Pelican {

void PelicanCore::run() {
    try {
        setupLogger();

        DependencyContainer container;

        auto &loop = container.get<Loop>();
        loop.run();
    } catch (std::exception &e) {
        LOG_ERROR(logger, "Pelican fatal error : {}", e.what());
        return;
    }
}

} // namespace Pelican
