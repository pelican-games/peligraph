#pragma once

#include "../../project/passfieldownership.hpp"

namespace Pelican {

constexpr PassFieldOwnershipCapabilities
buildPassFieldOwnershipCapabilities() noexcept {
    return {
        .imgui_enabled = PELICAN_WITH_IMGUI != 0,
    };
}

} // namespace Pelican
