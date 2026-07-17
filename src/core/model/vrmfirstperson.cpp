#include "vrmfirstperson.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Pelican {
namespace {

std::vector<std::uint32_t>
triangleIndices(const VrmAutoTriangleSplitInput &input) {
    if (!input.indices.empty())
        return {input.indices.begin(), input.indices.end()};
    std::vector<std::uint32_t> generated(input.vertex_count);
    for (std::uint32_t index = 0; index < input.vertex_count; ++index)
        generated[index] = index;
    return generated;
}

} // namespace

VrmAutoTriangleSplit
splitVrmAutoTriangles(const VrmAutoTriangleSplitInput &input) {
    const auto indices = triangleIndices(input);
    if (indices.size() % 3 != 0)
        throw std::runtime_error(
            "VRM firstPerson auto requires TRIANGLES topology");
    for (const auto index : indices) {
        if (index >= input.vertex_count)
            throw std::runtime_error(
                "VRM firstPerson auto index exceeds POSITION count");
    }
    if (input.joints.empty() && input.weights.empty())
        return {indices, {}};
    if (input.joints.size() != input.vertex_count ||
        input.weights.size() != input.vertex_count)
        throw std::runtime_error(
            "VRM firstPerson auto requires matching JOINTS_0 and WEIGHTS_0");

    std::vector<std::uint8_t> head_related_vertices(input.vertex_count);
    for (std::uint32_t vertex = 0; vertex < input.vertex_count; ++vertex) {
        for (int component = 0; component < 4; ++component) {
            const auto weight = input.weights[vertex][component];
            if (!std::isfinite(weight))
                throw std::runtime_error(
                    "VRM firstPerson auto WEIGHTS_0 contains a non-finite value");
            if (!(weight > 0.0f)) continue;
            const auto joint = input.joints[vertex][component];
            if (joint < 0 ||
                static_cast<std::size_t>(joint) >= input.skin_joint_nodes.size())
                throw std::runtime_error(
                    "VRM firstPerson auto JOINTS_0 exceeds its skin joint array");
            const auto node = input.skin_joint_nodes[static_cast<std::size_t>(joint)];
            if (node < 0 ||
                static_cast<std::size_t>(node) >= input.head_related_nodes.size())
                throw std::runtime_error(
                    "VRM firstPerson auto skin references an invalid node");
            if (input.head_related_nodes[static_cast<std::size_t>(node)] != 0) {
                head_related_vertices[vertex] = 1;
                break;
            }
        }
    }

    VrmAutoTriangleSplit result;
    result.both_indices.reserve(indices.size());
    result.third_person_only_indices.reserve(indices.size());
    for (std::size_t first = 0; first < indices.size(); first += 3) {
        const bool head = head_related_vertices[indices[first]] != 0 ||
                          head_related_vertices[indices[first + 1]] != 0 ||
                          head_related_vertices[indices[first + 2]] != 0;
        auto &output = head ? result.third_person_only_indices
                            : result.both_indices;
        output.insert(output.end(), indices.begin() + first,
                      indices.begin() + first + 3);
    }
    return result;
}

} // namespace Pelican
