#pragma once

#include "../model/modeltemplate.hpp"
#include "../phys/physworld.hpp"

#include <limits>
#include <stdexcept>

namespace Pelican {

// Import already baked each node's transform into model-space vertices.
// One binding per instance keeps primitive colliders independently addressable.
inline std::optional<PhysWorld::Binding> makeModelPhysicsBinding(
    const ModelTemplate &model, std::string object_name,
    std::variant<GameObjectId, PhysWorldTransform> transform) {
    if (!model.collision_meshes || model.collision_meshes->empty()) return std::nullopt;
    auto geometry = std::make_shared<PhysWorld::MeshGeometry>();
    for (const auto &source : *model.collision_meshes) {
        const auto offset = geometry->vertices.size();
        if (source.positions.size() > std::numeric_limits<std::uint32_t>::max() - offset)
            throw std::overflow_error("Model collision mesh vertex count exceeds uint32");
        for (const auto vertex : source.positions)
            geometry->vertices.push_back({vertex.x, vertex.y, vertex.z});
        for (const auto index : source.indices) {
            if (index >= source.positions.size())
                throw std::invalid_argument("Model collision mesh index out of range");
            geometry->indices.push_back(static_cast<std::uint32_t>(offset + index));
        }
    }
    PhysWorld::Binding binding;
    binding.identity.name = std::move(object_name) + "/mesh";
    binding.transform_source = std::move(transform);
    binding.mesh = std::move(geometry);
    return binding;
}

} // namespace Pelican
