#include "../userpublic/physics/abi_v2.hpp"

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

} // namespace Pelican::Physics
