#pragma once

#include <vk_mem_alloc.hpp>

namespace Pelican {

struct BufferWrapper {
    vma::UniqueBuffer buffer;
    vma::UniqueAllocation allocation;
};

} // namespace Pelican