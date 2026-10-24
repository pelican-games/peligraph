#include "imageusage.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace Pelican {

namespace {

constexpr std::array<ImageUsageInfo, 8> kImageUsages{{
    {ImageUsageBit::color_attachment, "COLOR_ATTACHMENT", true},
    {ImageUsageBit::depth_stencil_attachment, "DEPTH_STENCIL_ATTACHMENT", true},
    {ImageUsageBit::sampled, "SAMPLED", true},
    {ImageUsageBit::storage, "STORAGE", true},
    {ImageUsageBit::transfer_dst, "TRANSFER_DST", true},
    {ImageUsageBit::transfer_src, "TRANSFER_SRC", true},
    {ImageUsageBit::input_attachment, "INPUT_ATTACHMENT", false},
    {ImageUsageBit::transient_attachment, "TRANSIENT_ATTACHMENT", false},
}};

} // namespace

std::span<const ImageUsageInfo> imageUsageTable() {
    return kImageUsages;
}

std::string_view imageUsageBitName(ImageUsageBit bit) {
    for (const auto &row : kImageUsages) {
        if (row.bit == bit) return row.name;
    }
    throw std::runtime_error("unknown image usage bit");
}

std::vector<std::string> imageUsageNames(ImageUsage usage) {
    std::vector<std::string> names;
    for (const auto &row : kImageUsages) {
        if (usage & row.bit) names.emplace_back(row.name);
    }
    return names;
}

std::vector<std::string> authoredImageUsageNames() {
    std::vector<std::string> names;
    for (const auto &row : kImageUsages) {
        if (row.authorable) names.emplace_back(row.name);
    }
    return names;
}

ImageUsage parseAuthoredImageUsage(std::span<const std::string> names) {
    if (names.empty()) {
        throw std::runtime_error("Image usage flags must not be empty");
    }
    ImageUsage usage;
    for (const auto &name : names) {
        const auto found = std::find_if(
            kImageUsages.begin(), kImageUsages.end(),
            [&](const ImageUsageInfo &row) { return row.authorable && row.name == name; });
        if (found == kImageUsages.end()) {
            throw std::runtime_error("Unknown image usage flag: " + name);
        }
        usage |= found->bit;
    }
    return usage;
}

} // namespace Pelican
