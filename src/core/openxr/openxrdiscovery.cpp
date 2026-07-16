#include "openxrdiscovery.hpp"

#include <openxr/openxr.h>

namespace Pelican::OpenXr {

XrDiscoveryResult queryDiscovery(void *) {
    static_assert(XR_CURRENT_API_VERSION >= XR_MAKE_VERSION(1, 1, 0));
    return {XrDiscoveryAvailability::hook_unavailable};
}

} // namespace Pelican::OpenXr
