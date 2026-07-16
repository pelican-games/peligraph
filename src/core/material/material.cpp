#include "material.hpp"

#include <stdexcept>

namespace Pelican {

void applyLoweredMaterial(MaterialInfo &destination, const LoweredMaterial &lowered) {
    destination.custom_values_layout = lowered.values_layout;
    destination.custom_values = lowered.values;
    destination.render_state = lowered.render_state;
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

} // namespace Pelican
