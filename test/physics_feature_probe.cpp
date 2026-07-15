#include "../src/core/userpublic/physics/abi_v1.hpp"

int main() {
    auto api = Pelican::Physics::descriptor<Pelican::Physics::ApiV1>();
    const auto status = Pelican::Physics::getApiV1(Pelican::Physics::abiVersionV1, &api);
#if PELICAN_WITH_PHYSICS
    return status == Pelican::Physics::Status::ok ? 0 : 1;
#else
    return status == Pelican::Physics::Status::unavailable ? 0 : 1;
#endif
}
