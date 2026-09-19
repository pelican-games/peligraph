#pragma once

// The reads only a test performs on MaterialContainer. They used to be
// sixteen public `*ForTesting` members on the container itself, which made a
// test-only surface indistinguishable from the engine API. They live here
// now, behind the one friend the container declares, so the container's
// public header states only what production calls.
//
// The two counts `memorydiagnostics.cpp` publishes in the engine memory
// report -- `textureCountForTesting` and `materialCountForTesting` -- stay
// public on the container; they are production readers, not tests.

#include "../src/core/material/materialcontainer.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace Pelican::test_support {

struct MaterialContainerTestAccess {
    using Container = MaterialContainer;

    // Texture resources.

    static std::pair<vk::ImageView, vk::ImageView>
    textureViews(const Container &materials, GlobalTextureId texture) {
        return materials.textureViewsForTesting(texture);
    }

    static SurfaceTextureDimension
    textureDimension(const Container &materials, GlobalTextureId texture) {
        return materials.texture_store.textures.get(texture)
            .dimension;
    }

    static vk::ImageViewType
    textureViewType(const Container &materials, GlobalTextureId texture) {
        return materials.texture_store.textures.get(texture)
            .view_type;
    }

    static std::vector<uint8_t> texturePixels(const Container &materials,
                                              GlobalTextureId texture,
                                              uint32_t mip_level = 0) {
        return materials.texturePixelsForTesting(texture, mip_level);
    }

    static uint32_t textureMipLevels(const Container &materials,
                                     GlobalTextureId texture) {
        return materials.texture_store.textures.get(texture).image.mip_levels;
    }

    static size_t referencingMaterialCount(const Container &materials,
                                           GlobalTextureId texture) {
        return materials.referencingMaterialCountForTesting(texture);
    }

    // Materials.

    static size_t materialCapacity(const Container &materials) {
        return materials.materialCapacityForTesting();
    }

    static std::optional<GlobalTextureId>
    materialTexture(const Container &materials, GlobalMaterialId material,
                    std::uint32_t binding) {
        return materials.materialTextureForTesting(material, binding);
    }

    static std::optional<ResolvedMaterialSampler>
    materialSamplerResolution(const Container &materials,
                              GlobalMaterialId material,
                              std::string_view texture_name) {
        return materials.materialSamplerResolutionForTesting(material,
                                                             texture_name);
    }

    static std::uint64_t
    materialDescriptorRevision(const Container &materials,
                               GlobalMaterialId material) {
        return materials.materials.get(material).descriptor_revision;
    }

    // The authored (CPU-side) values, as distinct from the bytes the GPU
    // record holds.
    static std::vector<std::byte> materialValues(const Container &materials,
                                                 GlobalMaterialId material) {
        return materials.materials.get(material).custom_values;
    }

    static std::vector<std::byte> materialGpuValues(const Container &materials,
                                                    GlobalMaterialId material) {
        return materials.materialGpuValuesForTesting(material);
    }

    static std::vector<std::byte> materialGpuRecord(const Container &materials,
                                                    GlobalMaterialId material) {
        return materials.materialGpuRecordForTesting(material);
    }

    static std::optional<MaterialOutputSchema>
    materialOutputSchema(const Container &materials,
                         GlobalMaterialId material) {
        return materials.materials.get(material).output_schema;
    }

    // Screen inputs.

    static std::vector<vk::ImageView>
    boundScreenInputImageViews(const Container &materials,
                               GlobalMaterialId material,
                               const PassDefinition &pass,
                               std::uint32_t view_index = 0) {
        return materials.boundScreenInputImageViewsForTesting(material, pass,
                                                              view_index);
    }

    static std::uint64_t
    screenInputBindingRevision(const Container &materials,
                               GlobalMaterialId material,
                               const PassDefinition &pass) {
        return materials.screenInputBindingRevisionForTesting(material, pass);
    }
};

} // namespace Pelican::test_support

// Short name for the call sites. This header is only ever included by tests,
// so the global-scope alias reaches no production translation unit.
using MaterialContainerTestAccess =
    Pelican::test_support::MaterialContainerTestAccess;
