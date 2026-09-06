#include "../userpublic/physics/abi_v3.hpp"

namespace Pelican::Physics {

Status getApiV2(std::uint32_t client_abi_version, ApiV2 *out_api) noexcept {
    if (out_api == nullptr || out_api->struct_size < descriptorHeaderSize)
        return Status::invalid_argument;
    if (out_api->version != descriptorVersionV1) return Status::unsupported_version;
    if (out_api->reserved0 != 0 || out_api->reserved1 != 0)
        return Status::reserved_not_zero;
    if (client_abi_version != abiVersionV2) return Status::unsupported_version;
    return Status::unavailable;
}

Status getApiV3(std::uint32_t client_abi_version, ApiV3 *out_api) noexcept {
    if (out_api == nullptr || out_api->struct_size < sizeof(ApiV3)) return Status::invalid_argument;
    if (out_api->version != descriptorVersionV1 || client_abi_version != abiVersionV3)
        return Status::unsupported_version;
    if (out_api->reserved0 != 0 || out_api->reserved1 != 0) return Status::reserved_not_zero;
    return Status::unavailable;
}

} // namespace Pelican::Physics
