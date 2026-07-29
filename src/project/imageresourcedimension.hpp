#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace Pelican {

// Physical image shape is independent from the view-family layout selected
// by the scheduler. A cube image may still expose 2D face views for raster
// output and a cube view for direction-space sampling.
enum class ImageResourceDimension : std::uint8_t {
    two_d,
    cube,
};

inline std::string_view imageResourceDimensionName(
    ImageResourceDimension dimension) {
    switch (dimension) {
    case ImageResourceDimension::two_d:
        return "2d";
    case ImageResourceDimension::cube:
        return "cube";
    }
    throw std::runtime_error(
        "unknown image resource dimension");
}

enum class ImageSubresourceViewDimension :
    std::uint8_t {
    two_d,
    two_d_array,
    cube,
};

inline std::string_view imageSubresourceViewDimensionName(
    ImageSubresourceViewDimension dimension) {
    switch (dimension) {
    case ImageSubresourceViewDimension::two_d:
        return "2d";
    case ImageSubresourceViewDimension::two_d_array:
        return "2d_array";
    case ImageSubresourceViewDimension::cube:
        return "cube";
    }
    throw std::runtime_error(
        "unknown image subresource view dimension");
}

} // namespace Pelican
