#pragma once

#include <stdexcept>

namespace Pelican {

inline constexpr const char *materialPassesRemovedCode = "material_passes_removed";

// A no-material-pass document is valid at startup. Live materials still own
// pipelines and bindings for the previous passes, so keep that generation
// intact and commit the document for a restart through the edit ticket.
class MaterialPassesRemoved : public std::runtime_error {
  public:
    MaterialPassesRemoved()
        : std::runtime_error("The last material pass was removed; restart the engine "
                             "to register materials without a receiving pass") {}
};

} // namespace Pelican
