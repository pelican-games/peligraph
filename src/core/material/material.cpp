#include "material.hpp"

#include <stdexcept>

namespace Pelican {

void applyLoweredMaterial(MaterialInfo &destination, const LoweredMaterial &lowered) {
    destination.tags = lowered.tags;
    destination.custom_values_layout = lowered.values_layout;
    destination.custom_values = lowered.values;
    destination.render_state = lowered.render_state;
    destination.route = lowered.route;
    // This overload is paired with SurfacePass::main.  Its five-MRT payload
    // is the legacy lit-material contract, even though it has the same
    // attachment count as the deferred G-buffer variant.
    destination.shader_contract = MaterialShaderContract::legacy_gbuffer_v1;
    destination.exact_pass = lowered.exact_pass;
    destination.screen_inputs = lowered.screen_input_contracts;
    destination.resource_ports = lowered.resource_ports;
    destination.custom_textures.clear();
    destination.custom_textures.reserve(lowered.textures.size());
    for (const auto &texture : lowered.textures) {
        destination.custom_textures.push_back(MaterialInfo::CustomTextureBinding{
            texture.name,
            std::nullopt,
            texture.role,
            texture.missing_default,
        });
    }
}

namespace {

MaterialShaderContract shaderContractForRoute(MaterialRouteClass route) {
    return route == MaterialRouteClass::deferred_geometry
               ? MaterialShaderContract::gbuffer_v1
               : MaterialShaderContract::forward_scene_color_v1;
}

} // namespace

void applyLoweredMaterial(MaterialInfo &destination, const LoweredMaterial &lowered,
                          const LoweredMaterialTextureResolver &resolve_texture) {
    if (!resolve_texture) {
        throw std::runtime_error("lowered material texture resolver is empty");
    }
    applyLoweredMaterial(destination, lowered);
    for (std::size_t index = 0; index < lowered.textures.size(); ++index) {
        const auto &source = lowered.textures[index];
        try {
            destination.custom_textures[index].texture =
                resolve_texture(source.reference, source.role);
        } catch (const std::exception &error) {
            throw std::runtime_error("material '" + lowered.name + "' texture '" +
                                     source.name + "' reference '" + source.reference +
                                     "': " + error.what());
        }
    }
}

void applyLoweredMaterialForRoute(MaterialInfo &destination,
                                  const LoweredMaterial &lowered) {
    applyLoweredMaterial(destination, lowered);
    destination.shader_contract = shaderContractForRoute(lowered.route);
}

void applyLoweredMaterialForRoute(MaterialInfo &destination,
                                  const LoweredMaterial &lowered,
                                  const LoweredMaterialTextureResolver &resolve_texture) {
    applyLoweredMaterial(destination, lowered, resolve_texture);
    destination.shader_contract = shaderContractForRoute(lowered.route);
}

} // namespace Pelican
