#include "material.hpp"

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

} // namespace Pelican
