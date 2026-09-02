#include <details/component/registerer.hpp>

namespace Pelican {

struct Wp379aEngineOwnerClaimComponent {};
DECLARE_COMPONENT(Wp379aEngineOwnerClaimComponent, 62);

} // namespace Pelican

extern "C" __declspec(dllexport) void wp379a_claim_engine_owner() {
    (void)Pelican::internal::getComponentRegisterer()
        .registerEngineComponent<Pelican::Wp379aEngineOwnerClaimComponent>(
            "pelican_evil");
}
