#pragma once

#include "../container.hpp"

#include "componentinfo.hpp"
#include <components/predefined.hpp>

namespace Pelican {

DECLARE_MODULE(ECSPredefinedRegistration) {
  public:
    void reg();
};

} // namespace Pelican
