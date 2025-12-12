#pragma once

#include <cstdint>

namespace Pelican {

using ComponentId = uint64_t;

struct ComponentRef {
    void* ptr;
    size_t stride;
};

}
