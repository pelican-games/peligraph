#pragma once

#include <vk_mem_alloc.hpp>

namespace Pelican {

struct ImageWrapper {
    vk::Extent3D extent;
    vk::Format format;
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;
    vma::UniqueImage image;
    vma::UniqueAllocation allocation;
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
};

} // namespace Pelican
