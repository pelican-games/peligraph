#pragma once

#include "../shader/pelican_sets.hpp"
#include <cstddef>
#include <cstdint>

namespace Pelican {

// Descriptor-set numbers and capacity limits shared by the material
// container and the pieces carved out of it (texture store, descriptor
// builder, generation planner). They were file-scope constants of
// materialcontainer.cpp; the split only moved them, it did not change
// any value.
constexpr std::uint32_t imageDescriptorSetNumber =
    PELICAN_SET_MATERIAL;
constexpr std::uint32_t baseColorBinding = 0;
constexpr std::uint32_t metallicRoughnessBinding = 1;
constexpr std::uint32_t normalBinding = 2;
constexpr std::uint32_t emissiveBinding = 3;
constexpr std::uint32_t vatPositionBinding = 4;
constexpr std::uint32_t vatNormalBinding = 5;
constexpr std::uint32_t materialBufferBinding =
    PELICAN_MATERIAL_BUFFER_BINDING;
constexpr std::uint32_t occlusionBinding =
    PELICAN_MATERIAL_OCCLUSION_TEXTURE_BINDING;
constexpr std::uint32_t baseMaterialTextureBindingCount = 5;
constexpr std::uint32_t vatMaterialTextureBindingCount = 7;
constexpr std::size_t maxMaterials = 1024;
constexpr std::uint32_t maxMaterialPassInputs = 8;
constexpr std::uint32_t maxMaterialPassDescriptors = 32;

} // namespace Pelican
