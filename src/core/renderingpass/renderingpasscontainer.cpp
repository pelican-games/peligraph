#include "renderingpasscontainer.hpp"

namespace Pelican {

std::span<PassId> RenderingPassContainer::getPasses(RenderingPassId rendering_pass_id) const {
    // TODO
    static PassId single_pass[1] = {0};

    return single_pass;
}

} // namespace Pelican
