#include "../src/core/userpublic/physics/abi_v2.hpp"

int main() {
    auto api = Pelican::Physics::descriptor<Pelican::Physics::ApiV1>();
    const auto status = Pelican::Physics::getApiV1(Pelican::Physics::abiVersionV1, &api);
    auto api_v2 = Pelican::Physics::descriptor<Pelican::Physics::ApiV2>();
    const auto status_v2 =
        Pelican::Physics::getApiV2(Pelican::Physics::abiVersionV2, &api_v2);
#if PELICAN_WITH_PHYSICS
    return status == Pelican::Physics::Status::ok &&
                   status_v2 == Pelican::Physics::Status::ok
               ? 0
               : 1;
#else
    return status == Pelican::Physics::Status::unavailable &&
                   status_v2 == Pelican::Physics::Status::unavailable
               ? 0
               : 1;
#endif
}
