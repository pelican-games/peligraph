#pragma once

#include <cstdint>
#include <details/ecs/componentdeclare.hpp>

namespace Pelican {

struct Wp379ExternalRayInstanceComponent {
    std::uint32_t marker = 0;

    template <class Archive> void ref(Archive &archive) {
        archive.prop("marker", marker);
    }
};

DECLARE_COMPONENT(Wp379ExternalRayInstanceComponent, 63);

} // namespace Pelican
