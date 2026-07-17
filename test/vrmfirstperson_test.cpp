#include "../src/core/model/vrmfirstperson.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("VRM firstPerson auto splits exact head weighted triangles deterministically",
          "[wp134][vrm][firstperson][unit]") {
    const std::vector<std::uint32_t> indices{0, 1, 2, 2, 1, 3};
    std::vector<glm::i16vec4> joints(4, {0, 0, 0, 0});
    joints[0] = {2, 0, 0, 0};
    // A zero-weight reference to Head must not classify the second triangle.
    joints[3] = {0, 1, 0, 0};
    std::vector<glm::vec4> weights(4, {1.0f, 0.0f, 0.0f, 0.0f});
    const std::vector<int> skin_joint_nodes{0, 3, 4};
    const std::vector<std::uint8_t> head_related_nodes{0, 0, 0, 1, 1};
    const VrmAutoTriangleSplitInput input{
        .indices = indices,
        .vertex_count = 4,
        .joints = joints,
        .weights = weights,
        .skin_joint_nodes = skin_joint_nodes,
        .head_related_nodes = head_related_nodes,
    };

    const auto first = splitVrmAutoTriangles(input);
    const auto second = splitVrmAutoTriangles(input);
    REQUIRE(first.third_person_only_indices ==
            std::vector<std::uint32_t>{0, 1, 2});
    REQUIRE(first.both_indices == std::vector<std::uint32_t>{2, 1, 3});
    REQUIRE(second.third_person_only_indices ==
            first.third_person_only_indices);
    REQUIRE(second.both_indices == first.both_indices);
}

} // namespace Pelican
