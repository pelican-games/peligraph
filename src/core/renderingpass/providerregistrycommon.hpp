#pragma once

// Shared ABI plumbing for the render provider registries
// (graph transform / subgraph replacement / render strategy /
// pass implementation). Only the pure helpers live here: handle
// and envelope validation, generation stepping, the v1 size
// guard and the Status name table. The registry lifecycle
// (registerProvider / unregisterProvider / snapshot /
// activateOwner / releaseOwner) stays with each registry.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican::provider_registry_common {

// Generation 0 means "no provider", so the counter skips it on
// wrap instead of handing out a handle that validHandle rejects.
inline std::uint32_t nextGeneration(
    std::uint32_t generation) noexcept {
    if (++generation == 0) ++generation;
    return generation;
}

// ProviderHandleV1 has the same shape in every render ABI:
// identity / generation / reserved.
template <typename Handle>
bool validHandle(Handle handle) noexcept {
    return handle.identity != 0 && handle.generation != 0 &&
           handle.reserved == 0;
}

inline bool validByteRange(
    const char *data, std::uint32_t size,
    std::uint32_t maximum) noexcept {
    return data != nullptr && size != 0 && size <= maximum &&
           std::find(data, data + size, '\0') == data + size;
}

// The provider envelope (struct_size, descriptor version, the
// three reserved words, the provider version handshake, the
// capability bits and the name byte range) is identical in
// every render ABI. Only the resolve callback member and the
// ABI's own constants differ, so the registry passes both in.
template <typename Provider, typename Resolve>
bool validProviderEnvelope(
    const Provider &provider, Resolve Provider::*resolve,
    std::uint32_t descriptor_version,
    std::uint32_t provider_version,
    std::uint64_t capability_bits,
    std::uint32_t maximum_name_bytes) noexcept {
    if (provider.struct_size < sizeof(Provider) ||
        provider.version != descriptor_version ||
        provider.reserved0 != 0 || provider.reserved1 != 0 ||
        provider.reserved2 != 0 ||
        provider.provider_version != provider_version ||
        provider.minimum_engine_provider_version >
            provider_version ||
        provider.capability_bits != capability_bits ||
        provider.*resolve == nullptr) {
        return false;
    }
    return validByteRange(
        provider.name_utf8, provider.name_size,
        maximum_name_bytes);
}

// The message reads "<subject> <field> exceeds the v1 ABI size
// limit for <target_noun>'<target>'", where target_noun is empty
// or a qualifier such as "region " or "pass ".
inline std::uint32_t abiSizeChecked(
    std::size_t size, std::string_view field,
    std::string_view subject, std::string_view target_noun,
    std::string_view target) {
    if (size >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error(
            std::string{subject} + " " + std::string{field} +
            " exceeds the v1 ABI size limit for " +
            std::string{target_noun} + "'" +
            std::string{target} + "'");
    }
    return static_cast<std::uint32_t>(size);
}

// The four render provider ABIs number these eleven Status
// values identically (pinned by static_assert next to each
// enum). An ABI that adds an enumerator (RenderPolicy has
// buffer_too_small) needs its own table.
template <typename Status>
const char *statusName(Status status) noexcept {
    switch (status) {
    case Status::ok: return "ok";
    case Status::invalid_argument:
        return "invalid_argument";
    case Status::unsupported_version:
        return "unsupported_version";
    case Status::reserved_not_zero:
        return "reserved_not_zero";
    case Status::duplicate_provider:
        return "duplicate_provider";
    case Status::stale_provider:
        return "stale_provider";
    case Status::wrong_owner: return "wrong_owner";
    case Status::stale_owner: return "stale_owner";
    case Status::provider_error:
        return "provider_error";
    case Status::out_of_memory:
        return "out_of_memory";
    case Status::unavailable: return "unavailable";
    }
    return "unknown_status";
}

} // namespace Pelican::provider_registry_common
