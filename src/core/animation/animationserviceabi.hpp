#pragma once

#include "../userpublic/animation/abi_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace Pelican::Animation::Internal {

inline constexpr std::size_t descriptorHeaderSize = sizeof(DescriptorHeaderV1);

template <class T>
Status validateDescriptor(const T &value,
                          std::size_t minimum = sizeof(T)) {
    if (value.struct_size < minimum) return Status::invalid_argument;
    if (value.version != descriptorVersionV1)
        return Status::unsupported_version;
    if (value.reserved0 != 0 || value.reserved1 != 0)
        return Status::reserved_not_zero;
    return Status::ok;
}

template <class Handle>
bool sameHandle(Handle left, Handle right) {
    return left.identity == right.identity &&
           left.generation == right.generation && left.reserved == 0 &&
           right.reserved == 0;
}

inline std::string_view checkedString(const char *data, std::uint32_t size) {
    return data == nullptr ? std::string_view{} : std::string_view{data, size};
}

bool decodeSha256(std::string_view text, std::uint8_t (&output)[32]);

// Keeps generation tombstones after resources disappear so stale handles do
// not degrade into invalid-handle errors during reload and reset boundaries.
class ResourceGenerationLedger {
    std::unordered_map<std::uint64_t, std::uint32_t> generations_;

  public:
    Status validate(std::uint64_t identity,
                    std::uint32_t generation) const noexcept;
    void remember(std::uint64_t identity, std::uint32_t generation);
    void tombstone(std::uint64_t identity, std::uint32_t generation);
};

} // namespace Pelican::Animation::Internal
