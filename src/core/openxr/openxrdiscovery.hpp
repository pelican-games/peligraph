#pragma once

#include "../xractivation.hpp"

namespace Pelican::OpenXr {

// XR1a replaces this deliberately unavailable hook with runtime/system/
// graphics-binding discovery. XR0 must not create an XrInstance or session.
XrDiscoveryResult queryDiscovery(void *context);

} // namespace Pelican::OpenXr
