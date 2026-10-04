#pragma once

#include "../container.hpp"
#include "../../project/renderpipeline.hpp"

#include <string>
#include <vector>

namespace Pelican {

// PB-1 (engine_backlog 23): a launch with --render-fallback whose rendering
// configuration composed, but leaves a material it registers at start (an
// engine built-in, or a scene's) without a material pass for its route. Such
// a launch does not stop on material_route_unserved@1: while `collecting`
// (the renderer sets it for the start of a --render-fallback launch on the
// project's generation, and clears it once the module graph is built), the
// material is registered as if no material pass existed and is written down
// here. The renderer then starts the fallback generation with the refusal
// those materials make (Renderer::enterStartupFallbackForUnservedMaterials).
DECLARE_MODULE(MaterialStartupUnserved) {
  public:
    bool collecting = false;
    std::vector<UnservedMaterialRoute> unserved;

    void add(MaterialRouteClass route, MaterialShaderContract contract,
             UnservedMaterial material) {
        for (auto &entry : unserved) {
            if (entry.route == route && entry.shader_contract == contract) {
                entry.materials.push_back(std::move(material));
                return;
            }
        }
        unserved.push_back({route, contract, {std::move(material)}});
    }
};

} // namespace Pelican
