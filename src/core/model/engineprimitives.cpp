#include "engineprimitives.hpp"

#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

constexpr auto primitive_names = std::to_array<std::string_view>({"cube", "plane", "sphere"});

struct Vec3 {
    float x, y, z;
};

// One quad of a box-like shape. `right` is the direction U grows in and `up`
// the direction V shrinks in (glTF UV has its origin at the top left);
// right x up == normal, so (0,0) (1,0) (1,1) (0,1) runs counter-clockwise
// seen from outside and the glTF bitangent cross(normal, tangent) * w is `up`
// with w = +1.
void appendQuad(EnginePrimitiveMesh &mesh, Vec3 centre, Vec3 normal, Vec3 right, Vec3 up) {
    const auto base = static_cast<std::uint32_t>(mesh.vertexCount());
    constexpr std::array<std::array<float, 2>, 4> corners{{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
    for (const auto &[s, t] : corners) {
        const float a = s - 0.5F;
        const float b = t - 0.5F;
        mesh.positions.insert(mesh.positions.end(), {centre.x + right.x * a + up.x * b,
                                                     centre.y + right.y * a + up.y * b,
                                                     centre.z + right.z * a + up.z * b});
        mesh.normals.insert(mesh.normals.end(), {normal.x, normal.y, normal.z});
        mesh.texcoords.insert(mesh.texcoords.end(), {s, 1.0F - t});
        mesh.tangents.insert(mesh.tangents.end(), {right.x, right.y, right.z, 1.0F});
    }
    mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

EnginePrimitiveMesh cube() {
    EnginePrimitiveMesh mesh;
    struct Face {
        Vec3 normal, right, up;
    };
    constexpr std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    }};
    for (const auto &face : faces) {
        appendQuad(mesh, {face.normal.x * 0.5F, face.normal.y * 0.5F, face.normal.z * 0.5F},
                   face.normal, face.right, face.up);
    }
    return mesh;
}

EnginePrimitiveMesh plane() {
    EnginePrimitiveMesh mesh;
    appendQuad(mesh, {0, 0, 0}, {0, 1, 0}, {1, 0, 0}, {0, 0, -1});
    return mesh;
}

// A UV sphere: ring 0 is the +Y pole, ring `rings` the -Y pole; U runs with
// the segment index (towards +X seen from +Z), V with the ring index. The
// seam column is duplicated so U reaches 1. Pole rows keep one triangle per
// quad; the other would have zero area.
EnginePrimitiveMesh sphere() {
    constexpr auto segments = enginePrimitiveSphereSegments;
    constexpr auto rings = enginePrimitiveSphereRings;
    constexpr double radius = 0.5;
    EnginePrimitiveMesh mesh;
    const std::size_t vertices = static_cast<std::size_t>(rings + 1) * (segments + 1);
    mesh.positions.reserve(vertices * 3);
    mesh.normals.reserve(vertices * 3);
    mesh.texcoords.reserve(vertices * 2);
    mesh.tangents.reserve(vertices * 4);
    for (std::uint32_t ring = 0; ring <= rings; ++ring) {
        const double theta = std::numbers::pi * static_cast<double>(ring) / rings;
        // The end rings sit exactly on the poles, and the seam column exactly
        // on column 0, whatever the sin/cos rounding.
        const double sin_theta = ring == 0 || ring == rings ? 0.0 : std::sin(theta);
        const double cos_theta = ring == 0 ? 1.0 : (ring == rings ? -1.0 : std::cos(theta));
        for (std::uint32_t segment = 0; segment <= segments; ++segment) {
            const double phi = 2.0 * std::numbers::pi * static_cast<double>(segment % segments) / segments;
            const double sin_phi = std::sin(phi);
            const double cos_phi = std::cos(phi);
            const double nx = sin_theta * sin_phi;
            const double ny = cos_theta;
            const double nz = sin_theta * cos_phi;
            mesh.positions.insert(mesh.positions.end(), {static_cast<float>(nx * radius),
                                                         static_cast<float>(ny * radius),
                                                         static_cast<float>(nz * radius)});
            mesh.normals.insert(mesh.normals.end(),
                                {static_cast<float>(nx), static_cast<float>(ny), static_cast<float>(nz)});
            mesh.texcoords.insert(mesh.texcoords.end(),
                                  {static_cast<float>(static_cast<double>(segment) / segments),
                                   static_cast<float>(static_cast<double>(ring) / rings)});
            mesh.tangents.insert(mesh.tangents.end(),
                                 {static_cast<float>(cos_phi), 0.0F, static_cast<float>(-sin_phi), 1.0F});
        }
    }
    const auto at = [](std::uint32_t ring, std::uint32_t segment) {
        return ring * (segments + 1) + segment;
    };
    for (std::uint32_t ring = 0; ring < rings; ++ring) {
        for (std::uint32_t segment = 0; segment < segments; ++segment) {
            const auto a = at(ring, segment);
            const auto b = at(ring, segment + 1);
            const auto c = at(ring + 1, segment + 1);
            const auto d = at(ring + 1, segment);
            if (ring != rings - 1) mesh.indices.insert(mesh.indices.end(), {a, d, c});
            if (ring != 0) mesh.indices.insert(mesh.indices.end(), {a, c, b});
        }
    }
    return mesh;
}

std::string_view primitiveName(std::string_view reference) {
    return reference.substr(enginePrimitivePrefix.size());
}

} // namespace

bool isEnginePrimitiveReference(std::string_view reference) noexcept {
    return reference.starts_with(enginePrimitivePrefix);
}

std::span<const std::string_view> enginePrimitiveNames() noexcept {
    return primitive_names;
}

void requireKnownEnginePrimitive(std::string_view reference) {
    if (isEnginePrimitiveReference(reference)) {
        const auto name = primitiveName(reference);
        for (const auto known : primitive_names)
            if (known == name) return;
    }
    std::string known;
    for (const auto name : primitive_names) {
        if (!known.empty()) known += ", ";
        known += name;
    }
    throw std::runtime_error("model.primitive_unknown@1: '" + std::string{reference} +
                             "' is not an engine primitive (known: " + known + ")");
}

EnginePrimitiveMesh generateEnginePrimitive(std::string_view reference) {
    requireKnownEnginePrimitive(reference);
    const auto name = primitiveName(reference);
    if (name == "cube") return cube();
    if (name == "plane") return plane();
    return sphere();
}

} // namespace Pelican
