#pragma once

#include <memory>
#include <vk_mem_alloc.hpp>

namespace Pelican {

struct ImageWrapper {
    vk::Extent3D extent;
    vk::Format format;
    vk::ImageType image_type = vk::ImageType::e2D;
    uint32_t mip_levels = 1;
    uint32_t array_layers = 1;
    // Images are destroyed before their allocation. The shared owner also
    // keeps aliased allocations alive until the last bound image retires.
    std::shared_ptr<vma::UniqueAllocation> allocation;
    vma::UniqueImage image;
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
    vk::ImageUsageFlags usage;
    vk::ImageCreateFlags create_flags;
};

} // namespace Pelican
