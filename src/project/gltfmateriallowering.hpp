#pragma once

#include "materialformat.hpp"

#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view
    gltfCoreOpenPbrSurfaceDefine =
        "PELICAN_OPENPBR_GLTF_CORE_V1";

struct GltfCoreMaterialDescription {
    std::string name;
    MaterialBase base;
    std::string alpha_mode = "OPAQUE";
    double alpha_cutoff = 0.5;
    bool double_sided = false;
};

// Produces the ordinary pelican.material representation used by the material
// lowering pipeline. The glTF loader only transports source fields into this
// adapter; routing and wrapper selection live here.
MaterialDefinition makeGltfCoreMaterialDefinition(
    GltfCoreMaterialDescription description);

} // namespace Pelican
