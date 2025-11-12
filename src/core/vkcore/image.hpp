#pragma once

#include <vk_mem_alloc.hpp>

namespace Pelican {

struct ImageWrapper {
    vk::Extent3D extent;
    vk::Format format;
    vma::UniqueImage image;
    vma::UniqueAllocation allocation;
};

} // namespace Pelican