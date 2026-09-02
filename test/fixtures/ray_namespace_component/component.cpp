#include "ray_instance_component.hpp"

#include <details/component/registerer.hpp>

namespace {

class ExternalComponentRegistration final {
  public:
    ExternalComponentRegistration() {
        // Intentional old-name occurrence: this real DLL owns the negative
        // namespace/coexistence side of the WP379 production acceptance.
        (void)Pelican::internal::getComponentRegisterer()
            .registerComponent<Pelican::Wp379ExternalRayInstanceComponent>(
                "ray_instance");
    }
};

ExternalComponentRegistration registration;

} // namespace
