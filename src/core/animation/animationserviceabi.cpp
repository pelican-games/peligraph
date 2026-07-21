#include "animationserviceabi.hpp"

namespace Pelican::Animation::Internal {

bool decodeSha256(std::string_view text, std::uint8_t (&output)[32]) {
    if (text.size() != 64) return false;
    const auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < 32; ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) return false;
        output[index] =
            static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

Status ResourceGenerationLedger::validate(
    std::uint64_t identity, std::uint32_t generation) const noexcept {
    const auto found = generations_.find(identity);
    if (found == generations_.end()) return Status::invalid_handle;
    return found->second == generation ? Status::ok
                                       : Status::stale_generation;
}

void ResourceGenerationLedger::remember(std::uint64_t identity,
                                        std::uint32_t generation) {
    generations_[identity] = generation;
}

void ResourceGenerationLedger::tombstone(std::uint64_t identity,
                                         std::uint32_t generation) {
    if (++generation == 0) ++generation;
    remember(identity, generation);
}

} // namespace Pelican::Animation::Internal
