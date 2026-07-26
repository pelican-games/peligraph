#include "../src/core/userpublic/physics/abi_v2.hpp"

int main() {
    auto api = Pelican::Physics::descriptor<Pelican::Physics::ApiV2>();
    const auto status =
        Pelican::Physics::getApiV2(Pelican::Physics::abiVersionV2, &api);
    auto mismatched_api = Pelican::Physics::descriptor<Pelican::Physics::ApiV2>();
    const auto mismatch_status = Pelican::Physics::getApiV2(
        Pelican::Physics::abiVersionV2 + 1, &mismatched_api);
#if PELICAN_WITH_PHYSICS
    return status == Pelican::Physics::Status::ok &&
                   mismatch_status == Pelican::Physics::Status::unsupported_version
               ? 0
               : 1;
#else
    return status == Pelican::Physics::Status::unavailable &&
                   mismatch_status == Pelican::Physics::Status::unsupported_version
               ? 0
               : 1;
#endif
}
