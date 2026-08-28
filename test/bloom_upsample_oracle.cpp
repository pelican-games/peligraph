#include "bloom_upsample_oracle.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace Pelican::TestSupport {
namespace {

std::size_t bytesPerPixel(BloomUpsampleStorage storage) {
    return storage == BloomUpsampleStorage::rgba8_srgb ? 4u : 8u;
}

void validateImage(const BloomUpsampleImageView &image,
                   const char *label) {
    if (image.width == 0 || image.height == 0) {
        throw std::invalid_argument(std::string{label} +
                                    " extent must be non-zero");
    }
    const auto required =
        static_cast<std::size_t>(image.width) * image.height *
        bytesPerPixel(image.storage);
    if (image.bytes.size() != required) {
        throw std::invalid_argument(std::string{label} +
                                    " byte size does not match extent/format");
    }
}

double srgbToLinear(double encoded) {
    return encoded <= 0.04045
               ? encoded / 12.92
               : std::pow((encoded + 0.055) / 1.055, 2.4);
}

double linearToSrgb(double linear) {
    const auto value = std::clamp(linear, 0.0, 1.0);
    return value <= 0.0031308
               ? value * 12.92
               : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

std::uint8_t roundUnorm8(double value) {
    const double scaled = std::clamp(value, 0.0, 1.0) * 255.0;
    const double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    double rounded = lower;
    if (fraction > 0.5 ||
        (fraction == 0.5 &&
         (static_cast<std::uint64_t>(lower) & 1u) != 0u)) {
        rounded += 1.0;
    }
    return static_cast<std::uint8_t>(rounded);
}

std::uint16_t floatToHalf(float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16u) & 0x8000u;
    const std::uint32_t exponent = (bits >> 23u) & 0xffu;
    std::uint32_t mantissa = bits & 0x7fffffu;

    if (exponent == 0xffu) {
        if (mantissa == 0) return static_cast<std::uint16_t>(sign | 0x7c00u);
        return static_cast<std::uint16_t>(sign | 0x7e00u);
    }

    int half_exponent = static_cast<int>(exponent) - 127 + 15;
    if (half_exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7c00u);
    }
    if (half_exponent <= 0) {
        if (half_exponent < -10) return static_cast<std::uint16_t>(sign);
        mantissa |= 0x800000u;
        const auto shift = static_cast<unsigned>(14 - half_exponent);
        std::uint32_t rounded = mantissa >> shift;
        const std::uint32_t remainder =
            mantissa & ((std::uint32_t{1} << shift) - 1u);
        const std::uint32_t halfway = std::uint32_t{1} << (shift - 1u);
        if (remainder > halfway ||
            (remainder == halfway && (rounded & 1u) != 0u)) {
            ++rounded;
        }
        return static_cast<std::uint16_t>(sign | rounded);
    }

    std::uint32_t result =
        sign | (static_cast<std::uint32_t>(half_exponent) << 10u) |
        (mantissa >> 13u);
    const std::uint32_t remainder = mantissa & 0x1fffu;
    if (remainder > 0x1000u ||
        (remainder == 0x1000u && (result & 1u) != 0u)) {
        ++result;
    }
    return static_cast<std::uint16_t>(result);
}

double halfToDouble(std::uint16_t bits) {
    const bool negative = (bits & 0x8000u) != 0;
    const std::uint16_t exponent = (bits >> 10u) & 0x1fu;
    const std::uint16_t mantissa = bits & 0x03ffu;
    double value = 0.0;
    if (exponent == 0) {
        value = std::ldexp(static_cast<double>(mantissa), -24);
    } else if (exponent == 0x1fu) {
        value = mantissa == 0
                    ? std::numeric_limits<double>::infinity()
                    : std::numeric_limits<double>::quiet_NaN();
    } else {
        value = std::ldexp(
            1.0 + static_cast<double>(mantissa) / 1024.0,
            static_cast<int>(exponent) - 15);
    }
    return negative ? -value : value;
}

std::uint16_t nextHalfUp(std::uint16_t bits) {
    if ((bits & 0x7fffu) > 0x7c00u || bits == 0x7c00u) return bits;
    if (bits == 0x8000u) return 0x0001u;
    return static_cast<std::uint16_t>(
        (bits & 0x8000u) != 0 ? bits - 1u : bits + 1u);
}

std::uint16_t nextHalfDown(std::uint16_t bits) {
    if ((bits & 0x7fffu) > 0x7c00u || bits == 0xfc00u) return bits;
    if (bits == 0x0000u) return 0x8001u;
    return static_cast<std::uint16_t>(
        (bits & 0x8000u) != 0 ? bits + 1u : bits - 1u);
}

std::uint16_t halfFloor(double value) {
    auto result = floatToHalf(static_cast<float>(value));
    if (halfToDouble(result) > value) result = nextHalfDown(result);
    return result;
}

std::uint16_t halfCeil(double value) {
    auto result = floatToHalf(static_cast<float>(value));
    if (halfToDouble(result) < value) result = nextHalfUp(result);
    return result;
}

std::uint32_t orderedHalf(std::uint16_t bits) {
    return (bits & 0x8000u) != 0
               ? static_cast<std::uint32_t>(~bits & 0xffffu)
               : static_cast<std::uint32_t>(bits ^ 0x8000u);
}

int addressIndex(int index, std::uint32_t extent,
                 BloomUpsampleAddressMode mode) {
    const auto signed_extent = static_cast<int>(extent);
    if (mode == BloomUpsampleAddressMode::clamp_to_edge) {
        return std::clamp(index, 0, signed_extent - 1);
    }
    auto wrapped = index % signed_extent;
    if (wrapped < 0) wrapped += signed_extent;
    return wrapped;
}

std::uint16_t halfStorage(const BloomUpsampleImageView &image,
                          int x, int y, std::size_t channel,
                          BloomUpsampleAddressMode mode) {
    const auto addressed_x = addressIndex(x, image.width, mode);
    const auto addressed_y = addressIndex(y, image.height, mode);
    const auto offset =
        (static_cast<std::size_t>(addressed_y) * image.width +
         static_cast<std::size_t>(addressed_x)) * 8u +
        channel * 2u;
    return static_cast<std::uint16_t>(
        image.bytes[offset] |
        (static_cast<std::uint16_t>(image.bytes[offset + 1u]) << 8u));
}

std::uint8_t byteStorage(const BloomUpsampleImageView &image,
                         int x, int y, std::size_t channel,
                         BloomUpsampleAddressMode mode) {
    const auto addressed_x = addressIndex(x, image.width, mode);
    const auto addressed_y = addressIndex(y, image.height, mode);
    return image.bytes[
        (static_cast<std::size_t>(addressed_y) * image.width +
         static_cast<std::size_t>(addressed_x)) * 4u + channel];
}

double decoded(const BloomUpsampleImageView &image,
               int x, int y, std::size_t channel,
               BloomUpsampleAddressMode mode) {
    if (image.storage == BloomUpsampleStorage::rgba16_sfloat) {
        return halfToDouble(halfStorage(image, x, y, channel, mode));
    }
    const auto encoded = static_cast<double>(
                             byteStorage(image, x, y, channel, mode)) /
                         255.0;
    return channel == 3 ? encoded : srgbToLinear(encoded);
}

struct FilterSample {
    double value = 0.0;
    double bound = 0.0;
};

FilterSample sampleBilinear(
    const BloomUpsampleOracleRequest &request,
    std::size_t channel) {
    const double source_x =
        request.pixel_center_x * request.source.width /
            request.destination.width -
        0.5;
    const double source_y =
        request.pixel_center_y * request.source.height /
            request.destination.height -
        0.5;
    const int x0 = static_cast<int>(std::floor(source_x));
    const int y0 = static_cast<int>(std::floor(source_y));
    const double fx = source_x - std::floor(source_x);
    const double fy = source_y - std::floor(source_y);
    const double p00 = decoded(request.source, x0, y0, channel,
                               request.address_mode);
    const double p10 = decoded(request.source, x0 + 1, y0, channel,
                               request.address_mode);
    const double p01 = decoded(request.source, x0, y0 + 1, channel,
                               request.address_mode);
    const double p11 = decoded(request.source, x0 + 1, y0 + 1, channel,
                               request.address_mode);
    const double top = p00 + (p10 - p00) * fx;
    const double bottom = p01 + (p11 - p01) * fx;
    const double dx = std::max(std::abs(p10 - p00),
                               std::abs(p11 - p01));
    const double dy = std::max(std::abs(p01 - p00),
                               std::abs(p11 - p10));
    const double precision_scale = std::ldexp(
        1.0, static_cast<int>(request.sub_texel_precision_bits));
    return {
        .value = top + (bottom - top) * fy,
        .bound = dx / precision_scale + dy / precision_scale,
    };
}

BloomUpsampleOracleChannel evaluateLdr(
    const BloomUpsampleOracleRequest &request,
    int destination_x, int destination_y,
    std::size_t channel) {
    const auto sample = sampleBilinear(request, channel);
    const double destination =
        request.blend_mode == BloomUpsampleBlendMode::one_plus_one
            ? decoded(request.destination, destination_x, destination_y,
                      channel, request.address_mode)
            : 0.0;
    const double ideal = std::clamp(destination + sample.value, 0.0, 1.0);
    const double lower_linear =
        std::clamp(destination + sample.value - sample.bound, 0.0, 1.0);
    const double upper_linear =
        std::clamp(destination + sample.value + sample.bound, 0.0, 1.0);
    const auto encode = [channel](double linear) {
        return roundUnorm8(channel == 3 ? linear : linearToSrgb(linear));
    };
    const auto ideal_storage = encode(ideal);
    const auto lower_encoded = encode(lower_linear);
    const auto upper_encoded = encode(upper_linear);
    const std::uint32_t lower_storage = lower_encoded == 0
                                            ? 0u
                                            : lower_encoded - 1u;
    const std::uint32_t upper_storage =
        std::min<std::uint32_t>(255u, upper_encoded + 1u);
    const std::uint32_t actual_storage = byteStorage(
        request.actual, destination_x, destination_y, channel,
        request.address_mode);
    const auto storage_distance = static_cast<std::uint32_t>(std::abs(
        static_cast<int>(actual_storage) - static_cast<int>(ideal_storage)));
    const auto allowance = actual_storage < ideal_storage
                               ? ideal_storage - lower_storage
                               : upper_storage - ideal_storage;
    const bool matches = actual_storage >= lower_storage &&
                         actual_storage <= upper_storage;
    return {
        .ideal_linear = ideal,
        .actual_linear = channel == 3
                             ? static_cast<double>(actual_storage) / 255.0
                             : srgbToLinear(
                                   static_cast<double>(actual_storage) / 255.0),
        .lower_linear = lower_linear,
        .upper_linear = upper_linear,
        .filter_bound_linear = sample.bound,
        .ideal_storage = ideal_storage,
        .actual_storage = actual_storage,
        .lower_storage = lower_storage,
        .upper_storage = upper_storage,
        .storage_distance = storage_distance,
        .storage_allowance = allowance,
        .storage_margin = matches ? allowance - storage_distance : 0u,
        .matches = matches,
    };
}

BloomUpsampleOracleChannel evaluateHdr(
    const BloomUpsampleOracleRequest &request,
    int destination_x, int destination_y,
    std::size_t channel) {
    const auto sample = sampleBilinear(request, channel);
    const double destination =
        request.blend_mode == BloomUpsampleBlendMode::one_plus_one
            ? decoded(request.destination, destination_x, destination_y,
                      channel, request.address_mode)
            : 0.0;
    const double ideal = destination + sample.value;

    // First bracket the texture-filter result by outward-rounded adjacent
    // legal binary16 values. Then bracket the attachment blend/store by one
    // further adjacent binary16 value, which is the legal blend precision
    // allowance used by this oracle.
    auto filtered_lower = halfFloor(sample.value - sample.bound);
    auto filtered_upper = halfCeil(sample.value + sample.bound);
    filtered_lower = nextHalfDown(filtered_lower);
    filtered_upper = nextHalfUp(filtered_upper);
    auto lower_storage = halfFloor(
        destination + halfToDouble(filtered_lower));
    auto upper_storage = halfCeil(
        destination + halfToDouble(filtered_upper));
    lower_storage = nextHalfDown(lower_storage);
    upper_storage = nextHalfUp(upper_storage);

    const auto ideal_storage = floatToHalf(static_cast<float>(ideal));
    const auto actual_storage = halfStorage(
        request.actual, destination_x, destination_y, channel,
        request.address_mode);
    const double actual = halfToDouble(actual_storage);
    const double lower = halfToDouble(lower_storage);
    const double upper = halfToDouble(upper_storage);
    const bool finite = std::isfinite(ideal) && std::isfinite(actual) &&
                        std::isfinite(lower) && std::isfinite(upper);
    const bool matches = finite && actual >= lower && actual <= upper;
    const auto ideal_ordered = orderedHalf(ideal_storage);
    const auto actual_ordered = orderedHalf(actual_storage);
    const auto lower_ordered = orderedHalf(lower_storage);
    const auto upper_ordered = orderedHalf(upper_storage);
    const auto distance = ideal_ordered > actual_ordered
                              ? ideal_ordered - actual_ordered
                              : actual_ordered - ideal_ordered;
    const auto allowance = actual_ordered < ideal_ordered
                               ? ideal_ordered - lower_ordered
                               : upper_ordered - ideal_ordered;
    return {
        .ideal_linear = ideal,
        .actual_linear = actual,
        .lower_linear = lower,
        .upper_linear = upper,
        .filter_bound_linear = sample.bound,
        .ideal_storage = ideal_storage,
        .actual_storage = actual_storage,
        .lower_storage = lower_storage,
        .upper_storage = upper_storage,
        .storage_distance = distance,
        .storage_allowance = allowance,
        .storage_margin = matches ? allowance - distance : 0u,
        .matches = matches,
    };
}

} // namespace

BloomUpsampleOracleResult evaluateBloomUpsampleOracle(
    const BloomUpsampleOracleRequest &request) {
    validateImage(request.destination, "destination");
    validateImage(request.source, "source");
    validateImage(request.actual, "actual");
    if (request.destination.storage != request.source.storage ||
        request.destination.storage != request.actual.storage) {
        throw std::invalid_argument("oracle image formats must match");
    }
    if (request.destination.width != request.actual.width ||
        request.destination.height != request.actual.height) {
        throw std::invalid_argument(
            "destination and actual extents must match");
    }
    if (!std::isfinite(request.pixel_center_x) ||
        !std::isfinite(request.pixel_center_y) ||
        request.pixel_center_x < 0.5 || request.pixel_center_y < 0.5 ||
        request.pixel_center_x >= request.destination.width ||
        request.pixel_center_y >= request.destination.height) {
        throw std::invalid_argument(
            "pixel center must name a destination pixel");
    }
    const int destination_x =
        static_cast<int>(std::floor(request.pixel_center_x));
    const int destination_y =
        static_cast<int>(std::floor(request.pixel_center_y));

    BloomUpsampleOracleResult result;
    result.matches = true;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        result.channels[channel] =
            request.destination.storage == BloomUpsampleStorage::rgba8_srgb
                ? evaluateLdr(request, destination_x, destination_y, channel)
                : evaluateHdr(request, destination_x, destination_y, channel);
        result.matches = result.matches && result.channels[channel].matches;
    }
    return result;
}

} // namespace Pelican::TestSupport
