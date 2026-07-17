#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include "../renderer/modelinstance.hpp"

#include <optional>

namespace Pelican {

DECLARE_MODULE(VatPlayer) {
    bool enabled = false;
    std::optional<ModelTemplate> vat_model;
    std::optional<ModelInstanceId> instance;

    void applyCameraOverride();

  public:
    VatPlayer();

    void releaseInstanceForSceneLoad();
    bool isEnabled() const { return enabled; }
};

} // namespace Pelican
