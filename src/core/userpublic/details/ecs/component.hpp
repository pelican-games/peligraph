#pragma once

#include <cstddef>
#include <cstdint>

namespace Pelican {

using ComponentId = uint64_t;

struct ComponentRef {
    void* ptr;
    std::size_t stride;
};

}
