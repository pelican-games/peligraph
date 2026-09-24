#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Pelican {

// GP-5: the shapes the engine ships. A model reference spelled
// `engine://primitives/<name>` resolves without an asset_data entry; the mesh
// is generated here, in code, and joins the glTF load path as an in-memory
// document (GltfLoader::prepareEnginePrimitive), so everything downstream of a
// model (renderer, BLAS, picking, shadow, physics binding) sees an ordinary
// ModelTemplate.
inline constexpr std::string_view enginePrimitivePrefix = "engine://primitives/";

// The glTF material every primitive carries: grey (0.8), metallic 0,
// roughness 0.5, lowered like any glTF material onto the OpenPBR base
// surface. A pelican.material_bindings document names it, or replaces it by
// binding mesh 0 primitive 0 to a project material.
inline constexpr std::string_view enginePrimitiveDefaultMaterialName = "pelican.primitive.default";

// The sphere's tessellation. Fixed so that the same reference always yields
// the same bytes; changing either value changes every sphere in every project.
inline constexpr std::uint32_t enginePrimitiveSphereSegments = 32;
inline constexpr std::uint32_t enginePrimitiveSphereRings = 16;

// One indexed triangle list in glTF conventions: +Y up, counter-clockwise
// front faces, UV origin at the top left, tangent.w the bitangent sign.
struct EnginePrimitiveMesh {
    std::vector<float> positions; // xyz
    std::vector<float> normals;   // xyz
    std::vector<float> texcoords; // uv
    std::vector<float> tangents;  // xyzw
    std::vector<std::uint32_t> indices;

    [[nodiscard]] std::size_t vertexCount() const noexcept { return positions.size() / 3; }
};

// True for any reference under engine://primitives/, known or not: the prefix
// claims the reference, and an unknown name is refused by name rather than
// falling through to the project file resolver.
[[nodiscard]] bool isEnginePrimitiveReference(std::string_view reference) noexcept;

// The names that resolve: cube, plane, sphere.
[[nodiscard]] std::span<const std::string_view> enginePrimitiveNames() noexcept;

// Throws std::runtime_error "model.primitive_unknown@1: ..." when the name
// after the prefix is not one of enginePrimitiveNames().
void requireKnownEnginePrimitive(std::string_view reference);

// cube: edge 1, centred. plane: 1x1 in XZ facing +Y. sphere: radius 0.5.
// Deterministic: the same reference yields the same bytes.
[[nodiscard]] EnginePrimitiveMesh generateEnginePrimitive(std::string_view reference);

} // namespace Pelican
