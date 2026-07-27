#include "outputcompilefacts.hpp"

#include <type_traits>

namespace Pelican {

namespace {

void appendU32(std::vector<std::uint8_t> &bytes,
               std::uint32_t value) {
    for (std::uint32_t shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(
            (value >> shift) & 0xffU));
    }
}

template <class Enum>
std::uint32_t enumValue(Enum value) {
    return static_cast<std::uint32_t>(
        static_cast<std::underlying_type_t<Enum>>(value));
}

} // namespace

std::vector<std::uint8_t> canonicalOutputCompileFacts(
    const OutputCompileFacts &facts) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(12 * sizeof(std::uint32_t));
    // Canonical encoding version. This is process-internal compiler data,
    // not a persisted project or public ABI version.
    appendU32(bytes, 1);
    appendU32(bytes, enumValue(facts.target_kind));
    appendU32(bytes, facts.extent.width);
    appendU32(bytes, facts.extent.height);
    appendU32(bytes, enumValue(facts.color_format));
    appendU32(bytes, enumValue(facts.color_space));
    appendU32(bytes, enumValue(facts.encoding_path));
    appendU32(
        bytes,
        static_cast<std::uint32_t>(
            static_cast<VkImageUsageFlags>(
                facts.selected_usage)));
    appendU32(bytes, facts.capture_available ? 1U : 0U);
    appendU32(bytes, enumValue(facts.surface_transform));
    appendU32(bytes, facts.graphics_queue_family);
    appendU32(bytes, facts.presentation_queue_family);
    return bytes;
}

std::uint64_t outputCompileFactsFingerprint(
    const OutputCompileFacts &facts) {
    constexpr std::uint64_t offset_basis =
        14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    auto hash = offset_basis;
    for (const auto byte : canonicalOutputCompileFacts(facts)) {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

std::uint64_t wsiPresentConfigurationFingerprint(
    const WsiPresentConfiguration &configuration) {
    constexpr std::uint64_t offset_basis =
        14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::vector<std::uint8_t> bytes;
    bytes.reserve(5 * sizeof(std::uint32_t));
    appendU32(bytes, 1);
    appendU32(
        bytes, enumValue(configuration.present_mode));
    appendU32(bytes, configuration.image_count);
    appendU32(
        bytes, enumValue(configuration.composite_alpha));
    appendU32(bytes, configuration.clipped ? 1U : 0U);
    auto hash = offset_basis;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= prime;
    }
    return hash;
}

std::string_view outputTargetKindName(
    OutputTargetKind kind) noexcept {
    switch (kind) {
    case OutputTargetKind::window: return "window";
    case OutputTargetKind::offscreen: return "offscreen";
    }
    return "unknown";
}

std::string_view outputEncodingPathName(
    OutputEncodingPath path) noexcept {
    switch (path) {
    case OutputEncodingPath::srgb_hardware:
        return "srgb_hardware";
    case OutputEncodingPath::srgb_shader_unorm:
        return "srgb_shader_unorm";
    }
    return "unknown";
}

std::string_view outputEncodingPathRpcName(
    OutputEncodingPath path) noexcept {
    switch (path) {
    case OutputEncodingPath::srgb_hardware: return "srgb";
    case OutputEncodingPath::srgb_shader_unorm:
        return "unorm_fallback";
    }
    return "unknown";
}

} // namespace Pelican
