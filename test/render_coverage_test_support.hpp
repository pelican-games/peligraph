#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace Pelican::TestSupport {

// Shared SK-1/CB-1 silhouette oracle. Coordinates are inclusive; an empty
// image has no valid bbox and cannot establish render equivalence.
struct RenderCoverage {
    std::size_t width = 0, height = 0;
    std::vector<std::uint8_t> pixels;

    void validate() const {
        if (!width || !height || pixels.size() / width != height ||
            pixels.size() % width != 0)
            throw std::invalid_argument("coverage extent/size mismatch");
    }
    std::array<int, 4> bbox() const {
        validate();
        std::array<int, 4> result{static_cast<int>(width), static_cast<int>(height), -1, -1};
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            if (!pixels[i]) continue;
            const auto x = static_cast<int>(i % width), y = static_cast<int>(i / width);
            result[0] = std::min(result[0], x); result[1] = std::min(result[1], y);
            result[2] = std::max(result[2], x); result[3] = std::max(result[3], y);
        }
        return result;
    }
    std::size_t count() const {
        return std::count_if(pixels.begin(), pixels.end(), [](auto value) { return value != 0; });
    }
};

inline RenderCoverage depthCoverage(std::span<const float> depth, std::size_t width,
                                    std::size_t height) {
    RenderCoverage result{width, height, {}};
    result.pixels.reserve(depth.size());
    for (const auto value : depth) {
        if (!std::isfinite(value)) throw std::invalid_argument("non-finite depth readback");
        result.pixels.push_back(value < 1.0F);
    }
    result.validate();
    return result;
}

// R16G16_SFLOAT: ignore the sign bit of zero; reject infinities and NaNs.
inline RenderCoverage velocityCoverage(std::span<const std::uint16_t> rg,
                                       std::size_t width, std::size_t height) {
    if (rg.size() % 2) throw std::invalid_argument("incomplete velocity pixel");
    RenderCoverage result{width, height, {}};
    for (std::size_t i = 0; i < rg.size(); i += 2) {
        const auto x = rg[i] & 0x7fff, y = rg[i + 1] & 0x7fff;
        if (x >= 0x7c00 || y >= 0x7c00)
            throw std::invalid_argument("non-finite velocity readback");
        result.pixels.push_back(x != 0 || y != 0);
    }
    result.validate();
    return result;
}

struct CoverageComparison {
    std::size_t different = 0;
    bool nonempty = false, within_tolerance = false, equal_bbox = false, within_one_pixel = false;
    bool equivalent() const {
        return nonempty && within_tolerance && equal_bbox && within_one_pixel;
    }
};

inline CoverageComparison compareCoverage(const RenderCoverage &a, const RenderCoverage &b) {
    a.validate(); b.validate();
    if (a.width != b.width || a.height != b.height)
        throw std::invalid_argument("coverage images have different extents");
    CoverageComparison result;
    for (std::size_t i = 0; i < a.pixels.size(); ++i)
        result.different += bool(a.pixels[i]) != bool(b.pixels[i]);
    const auto ab = a.bbox(), bb = b.bbox();
    result.nonempty = a.count() > 0 && b.count() > 0;
    // Integer division is exact for the <= 0.1% criterion and cannot overflow.
    result.within_tolerance = result.different <= a.pixels.size() / 1000;
    result.equal_bbox = ab == bb;
    result.within_one_pixel = bb[0] >= ab[0] - 1 && bb[1] >= ab[1] - 1 &&
                              bb[2] <= ab[2] + 1 && bb[3] <= ab[3] + 1;
    return result;
}

inline RenderCoverage unionCoverage(RenderCoverage a, const RenderCoverage &b) {
    (void)compareCoverage(a, b);
    for (std::size_t i = 0; i < a.pixels.size(); ++i) a.pixels[i] = a.pixels[i] || b.pixels[i];
    return a;
}

// Portable, lossless image: white=both, red=a only, cyan=b only, black=neither.
inline void writeCoverageDifference(const std::filesystem::path &path,
                                    const RenderCoverage &a, const RenderCoverage &b) {
    (void)compareCoverage(a, b);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out{path, std::ios::binary};
    out << "P6\n" << a.width << ' ' << a.height << "\n255\n";
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        const char rgb[]{static_cast<char>(a.pixels[i] ? 255 : 0),
                         static_cast<char>(b.pixels[i] ? 255 : 0),
                         static_cast<char>(b.pixels[i] ? 255 : 0)};
        out.write(rgb, 3);
    }
    if (!out) throw std::runtime_error("cannot write coverage difference");
}

} // namespace Pelican::TestSupport
