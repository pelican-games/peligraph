#include "../src/core/model/gltfaccessorreader.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstring>
#include <string>
#include <vector>

// The accessor layer is the one part of the glTF importer that needs no model
// file, no material container, and no Vulkan device: bytes plus an accessor
// description go in, typed vertex data or a named rejection comes out. These
// cases pin the decode, the normalized-integer conversion, and every bounds
// rejection, so an importer change that relaxes one of them fails here instead
// of in a GPU fixture.

namespace Pelican::GltfInternal {
namespace {

using Catch::Matchers::ContainsSubstring;

template <class T> void append(std::vector<unsigned char> &bytes, const T &value) {
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(T));
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

struct AccessorFixture {
    tinygltf::Model model;
    std::string source_path = "fixture.glb";

    int addBuffer(std::vector<unsigned char> bytes) {
        tinygltf::Buffer buffer;
        buffer.data = std::move(bytes);
        model.buffers.push_back(std::move(buffer));
        return static_cast<int>(model.buffers.size()) - 1;
    }

    int addView(int buffer, std::size_t byte_offset, std::size_t byte_length,
                std::size_t byte_stride = 0) {
        tinygltf::BufferView view;
        view.buffer = buffer;
        view.byteOffset = byte_offset;
        view.byteLength = byte_length;
        view.byteStride = byte_stride;
        model.bufferViews.push_back(view);
        return static_cast<int>(model.bufferViews.size()) - 1;
    }

    int addAccessor(int buffer_view, int type, int component_type,
                    std::size_t count, std::size_t byte_offset = 0,
                    bool normalized = false) {
        tinygltf::Accessor accessor;
        accessor.bufferView = buffer_view;
        accessor.type = type;
        accessor.componentType = component_type;
        accessor.count = count;
        accessor.byteOffset = byte_offset;
        accessor.normalized = normalized;
        model.accessors.push_back(accessor);
        return static_cast<int>(model.accessors.size()) - 1;
    }

    GltfAccessorReader reader() { return GltfAccessorReader{model, source_path}; }

    // Named so the rejection cases can call it inside REQUIRE_THROWS_WITH,
    // whose two-parameter macro cannot hold the comma of a template argument
    // list.
    std::vector<glm::vec3> readVec3(int accessor_index = 0) {
        return reader().getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(
            accessor_index);
    }
};

// Two tightly packed FLOAT VEC3 elements, and the accessor that reads them.
AccessorFixture twoFloatVec3() {
    AccessorFixture fixture;
    std::vector<unsigned char> bytes;
    for (const float value : {1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F}) append(bytes, value);
    const auto buffer = fixture.addBuffer(std::move(bytes));
    const auto view = fixture.addView(buffer, 0, 24);
    fixture.addAccessor(view, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT, 2);
    return fixture;
}

} // namespace

TEST_CASE("glTF accessor decode widens components to the destination type",
          "[gltf][accessor]") {
    SECTION("tightly packed FLOAT VEC3") {
        auto fixture = twoFloatVec3();
        const auto values = fixture.readVec3();
        REQUIRE(values.size() == 2);
        REQUIRE(values[0] == glm::vec3{1.0F, 2.0F, 3.0F});
        REQUIRE(values[1] == glm::vec3{4.0F, 5.0F, 6.0F});
    }

    SECTION("UNSIGNED_SHORT indices widen to uint32") {
        AccessorFixture fixture;
        std::vector<unsigned char> bytes;
        for (const std::uint16_t value :
             {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{65535}})
            append(bytes, value);
        const auto buffer = fixture.addBuffer(std::move(bytes));
        const auto view = fixture.addView(buffer, 0, 6);
        fixture.addAccessor(view, TINYGLTF_TYPE_SCALAR,
                            TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, 3);
        const auto values =
            fixture.reader()
                .getDataFromAccessor<TINYGLTF_TYPE_SCALAR, std::uint32_t>(0);
        REQUIRE(values == std::vector<std::uint32_t>{0, 1, 65535});
    }

    SECTION("an interleaved byteStride skips the padding between elements") {
        AccessorFixture fixture;
        std::vector<unsigned char> bytes;
        for (const float value : {1.0F, 2.0F, 3.0F, 0.0F}) append(bytes, value);
        for (const float value : {4.0F, 5.0F, 6.0F, 0.0F}) append(bytes, value);
        const auto buffer = fixture.addBuffer(std::move(bytes));
        const auto view = fixture.addView(buffer, 0, 32, 16);
        fixture.addAccessor(view, TINYGLTF_TYPE_VEC3,
                            TINYGLTF_COMPONENT_TYPE_FLOAT, 2);
        const auto values = fixture.readVec3();
        REQUIRE(values.size() == 2);
        REQUIRE(values[1] == glm::vec3{4.0F, 5.0F, 6.0F});
    }

    SECTION("the accessor byteOffset starts inside its bufferView") {
        AccessorFixture fixture;
        std::vector<unsigned char> bytes;
        for (const float value : {1.0F, 2.0F, 3.0F, 0.0F}) append(bytes, value);
        for (const float value : {4.0F, 5.0F, 6.0F, 0.0F}) append(bytes, value);
        const auto buffer = fixture.addBuffer(std::move(bytes));
        const auto view = fixture.addView(buffer, 0, 32, 16);
        fixture.addAccessor(view, TINYGLTF_TYPE_VEC3,
                            TINYGLTF_COMPONENT_TYPE_FLOAT, 1, 16);
        REQUIRE(fixture.readVec3() ==
                std::vector<glm::vec3>{glm::vec3{4.0F, 5.0F, 6.0F}});
    }

    SECTION("a zero-count accessor decodes to nothing and reads no bytes") {
        AccessorFixture fixture;
        const auto buffer = fixture.addBuffer({});
        const auto view = fixture.addView(buffer, 0, 0);
        fixture.addAccessor(view, TINYGLTF_TYPE_VEC3,
                            TINYGLTF_COMPONENT_TYPE_FLOAT, 0);
        REQUIRE(fixture.readVec3().empty());
    }

    SECTION("MAT4 decodes only from FLOAT") {
        AccessorFixture fixture;
        std::vector<unsigned char> bytes;
        const glm::mat4 identity{1.0F};
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) append(bytes, identity[column][row]);
        const auto buffer = fixture.addBuffer(std::move(bytes));
        const auto view = fixture.addView(buffer, 0, 64);
        fixture.addAccessor(view, TINYGLTF_TYPE_MAT4,
                            TINYGLTF_COMPONENT_TYPE_FLOAT, 1);
        const auto values =
            fixture.reader().getDataFromAccessor<TINYGLTF_TYPE_MAT4, glm::mat4>(0);
        REQUIRE(values.size() == 1);
        REQUIRE(values[0] == identity);
    }
}

TEST_CASE("glTF normalized integer components decode to floats",
          "[gltf][accessor]") {
    SECTION("UNSIGNED_BYTE VEC4 spans zero to one") {
        AccessorFixture fixture;
        const auto buffer = fixture.addBuffer({0, 128, 255, 255});
        const auto view = fixture.addView(buffer, 0, 4);
        fixture.addAccessor(view, TINYGLTF_TYPE_VEC4,
                            TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, 1, 0, true);
        const auto values =
            fixture.reader().getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::vec4>(0);
        REQUIRE(values.size() == 1);
        REQUIRE(values[0] == glm::vec4{0.0F, 128.0F / 255.0F, 1.0F, 1.0F});
    }

    SECTION("signed BYTE clamps the extra negative step to -1") {
        AccessorFixture fixture;
        std::vector<unsigned char> bytes;
        for (const std::int8_t value :
             {std::int8_t{-128}, std::int8_t{0}, std::int8_t{127}})
            append(bytes, value);
        const auto buffer = fixture.addBuffer(std::move(bytes));
        const auto view = fixture.addView(buffer, 0, 3);
        fixture.addAccessor(view, TINYGLTF_TYPE_SCALAR,
                            TINYGLTF_COMPONENT_TYPE_BYTE, 3, 0, true);
        const auto values =
            fixture.reader().getDataFromAccessor<TINYGLTF_TYPE_SCALAR, float>(0);
        REQUIRE(values == std::vector<float>{-1.0F, 0.0F, 1.0F});
    }

    SECTION("UNSIGNED_SHORT VEC2 uses the 16-bit maximum") {
        AccessorFixture fixture;
        std::vector<unsigned char> bytes;
        for (const std::uint16_t value : {std::uint16_t{0}, std::uint16_t{65535}})
            append(bytes, value);
        const auto buffer = fixture.addBuffer(std::move(bytes));
        const auto view = fixture.addView(buffer, 0, 4);
        fixture.addAccessor(view, TINYGLTF_TYPE_VEC2,
                            TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, 1, 0, true);
        const auto values =
            fixture.reader().getDataFromAccessor<TINYGLTF_TYPE_VEC2, glm::vec2>(0);
        REQUIRE(values == std::vector<glm::vec2>{glm::vec2{0.0F, 1.0F}});
    }

    SECTION("an integer destination is refused") {
        AccessorFixture fixture;
        const auto buffer = fixture.addBuffer({7});
        const auto view = fixture.addView(buffer, 0, 1);
        fixture.addAccessor(view, TINYGLTF_TYPE_SCALAR,
                            TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE, 1, 0, true);
        REQUIRE_THROWS_WITH(
            (fixture.reader()
                 .getDataFromAccessor<TINYGLTF_TYPE_SCALAR, std::uint32_t>(0)),
            ContainsSubstring(
                "is normalized but its destination is not floating-point"));
    }

    SECTION("a normalized FLOAT accessor is refused") {
        AccessorFixture fixture;
        std::vector<unsigned char> bytes;
        append(bytes, 1.0F);
        const auto buffer = fixture.addBuffer(std::move(bytes));
        const auto view = fixture.addView(buffer, 0, 4);
        fixture.addAccessor(view, TINYGLTF_TYPE_SCALAR,
                            TINYGLTF_COMPONENT_TYPE_FLOAT, 1, 0, true);
        REQUIRE_THROWS_WITH(
            (fixture.reader()
                 .getDataFromAccessor<TINYGLTF_TYPE_SCALAR, float>(0)),
            ContainsSubstring("uses normalized components with an unsupported "
                              "component type 5126"));
    }

    SECTION("a normalized matrix shape is refused") {
        AccessorFixture fixture;
        const auto buffer = fixture.addBuffer(std::vector<unsigned char>(64, 0));
        const auto view = fixture.addView(buffer, 0, 64);
        fixture.addAccessor(view, TINYGLTF_TYPE_MAT4,
                            TINYGLTF_COMPONENT_TYPE_FLOAT, 1, 0, true);
        REQUIRE_THROWS_WITH(
            (fixture.reader()
                 .getDataFromAccessor<TINYGLTF_TYPE_MAT4, glm::mat4>(0)),
            ContainsSubstring("uses normalized components for an unsupported "
                              "accessor shape"));
    }
}

TEST_CASE("glTF accessor decode rejects everything it cannot address",
          "[gltf][accessor]") {
    SECTION("an out-of-range accessor index names the source file") {
        auto fixture = twoFloatVec3();
        REQUIRE_THROWS_WITH(
            fixture.readVec3(-1),
            ContainsSubstring("glTF accessor -1 in 'fixture.glb' is out of range"));
        REQUIRE_THROWS_WITH(
            fixture.readVec3(1),
            ContainsSubstring("glTF accessor 1 in 'fixture.glb' is out of range"));
    }

    SECTION("the accessor shape must be the one the caller asked for") {
        auto fixture = twoFloatVec3();
        REQUIRE_THROWS_WITH(
            (fixture.reader()
                 .getDataFromAccessor<TINYGLTF_TYPE_VEC2, glm::vec2>(0)),
            ContainsSubstring("has unexpected accessor type 3"));
    }

    SECTION("sparse accessors are not decoded") {
        auto fixture = twoFloatVec3();
        fixture.model.accessors[0].sparse.isSparse = true;
        REQUIRE_THROWS_WITH(fixture.readVec3(),
                            ContainsSubstring("uses an unsupported sparse accessor"));
    }

    SECTION("a missing bufferView is refused") {
        auto fixture = twoFloatVec3();
        fixture.model.accessors[0].bufferView = -1;
        REQUIRE_THROWS_WITH(fixture.readVec3(),
                            ContainsSubstring("has no valid bufferView"));
    }

    SECTION("a bufferView pointing at no buffer is refused") {
        auto fixture = twoFloatVec3();
        fixture.model.bufferViews[0].buffer = 7;
        REQUIRE_THROWS_WITH(
            fixture.readVec3(),
            ContainsSubstring("bufferView references an invalid buffer"));
    }

    SECTION("an unknown component encoding is refused") {
        auto fixture = twoFloatVec3();
        fixture.model.accessors[0].componentType = 9999;
        REQUIRE_THROWS_WITH(
            fixture.readVec3(),
            ContainsSubstring("has an unsupported component encoding"));
    }

    SECTION("a stride narrower than one element is refused") {
        auto fixture = twoFloatVec3();
        fixture.model.bufferViews[0].byteStride = 4;
        REQUIRE_THROWS_WITH(fixture.readVec3(),
                            ContainsSubstring("has an invalid byte stride"));
    }

    SECTION("a bufferView that leaves its buffer is refused") {
        auto fixture = twoFloatVec3();
        fixture.model.bufferViews[0].byteLength = 1000;
        REQUIRE_THROWS_WITH(fixture.readVec3(),
                            ContainsSubstring("bufferView exceeds its buffer"));
    }

    SECTION("an accessor byteOffset past the bufferView is refused") {
        auto fixture = twoFloatVec3();
        fixture.model.accessors[0].byteOffset = 100;
        REQUIRE_THROWS_WITH(
            fixture.readVec3(),
            ContainsSubstring("byteOffset exceeds its bufferView"));
    }

    SECTION("a count that reads past the bufferView is refused") {
        auto fixture = twoFloatVec3();
        fixture.model.accessors[0].count = 3;
        REQUIRE_THROWS_WITH(fixture.readVec3(),
                            ContainsSubstring("data exceeds its bufferView"));
    }

    SECTION("the last element must fit even when the offset leaves room") {
        auto fixture = twoFloatVec3();
        fixture.model.accessors[0].byteOffset = 16;
        fixture.model.accessors[0].count = 1;
        REQUIRE_THROWS_WITH(fixture.readVec3(),
                            ContainsSubstring("data exceeds its bufferView"));
    }

    SECTION("a component type the shape cannot decode is named in the error") {
        AccessorFixture fixture;
        const auto buffer = fixture.addBuffer(std::vector<unsigned char>(32, 0));
        const auto view = fixture.addView(buffer, 0, 32);
        fixture.addAccessor(view, TINYGLTF_TYPE_MAT4,
                            TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT, 1);
        REQUIRE_THROWS_WITH(
            (fixture.reader()
                 .getDataFromAccessor<TINYGLTF_TYPE_MAT4, glm::mat4>(0)),
            ContainsSubstring("has unsupported component type 5123"));
    }
}

} // namespace Pelican::GltfInternal
