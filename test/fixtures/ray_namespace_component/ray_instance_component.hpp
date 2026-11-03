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

} // namespace Pelican

// From the global namespace: GCC rejects the macro's qualified
// specialisation inside namespace Pelican (see components/predefined.hpp).
DECLARE_COMPONENT(Pelican::Wp379ExternalRayInstanceComponent, 63);
