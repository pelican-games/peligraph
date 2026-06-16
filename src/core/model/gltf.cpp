#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include "../log.hpp"
#include "../material/material.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/standardmaterialresource.hpp"
#include "gltf.hpp"
#include "vertbufcontainer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Pelican {

struct InternalGltfLoader {
    using ModelLocalMaterialId = int;

    MaterialContainer &mat_container;
    StandardMaterialResource &std_mat;
    VertBufContainer &buf_container;
    tinygltf::Model &model;
    std::vector<GlobalMaterialId> material_map;
    std::vector<GlobalTextureId> texture_map;
    std::unordered_map<ModelLocalMaterialId, std::vector<ModelTemplate::PrimitiveRefInfo>> tmp_material_primitives;

    uint8_t toUnorm8(double value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    }

    double vectorValueOr(const std::vector<double> &values, size_t index, double fallback) {
        return index < values.size() ? values[index] : fallback;
    }

    GlobalTextureId registerSolidTexture(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        std::array<uint8_t, 4 * 16> data{};
        for (size_t i = 0; i < 16; ++i) {
            data[i * 4 + 0] = r;
            data[i * 4 + 1] = g;
            data[i * 4 + 2] = b;
            data[i * 4 + 3] = a;
        }
        return mat_container.registerTexture(vk::Extent3D{4, 4, 1}, data.data());
    }

    GlobalTextureId metallicRoughnessTextureForMaterial(const tinygltf::Material &material) {
        const auto texture_index = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if (texture_index >= 0) {
            return texture_map[texture_index];
        }

        return registerSolidTexture(
            toUnorm8(material.occlusionTexture.strength),
            toUnorm8(material.pbrMetallicRoughness.roughnessFactor),
            toUnorm8(material.pbrMetallicRoughness.metallicFactor));
    }

    GlobalTextureId emissiveTextureForMaterial(const tinygltf::Material &material) {
        const auto texture_index = material.emissiveTexture.index;
        if (texture_index >= 0) {
            return texture_map[texture_index];
        }

        return registerSolidTexture(
            toUnorm8(vectorValueOr(material.emissiveFactor, 0, 0.0)),
            toUnorm8(vectorValueOr(material.emissiveFactor, 1, 0.0)),
            toUnorm8(vectorValueOr(material.emissiveFactor, 2, 0.0)));
    }

    template <class InType, class OutType>
    std::vector<OutType> readComponentByType(const unsigned char *p_data, size_t count, int stride) {
        std::vector<OutType> buf(count);
        for (int i = 0; i < count; i++) {
            buf[i] = static_cast<OutType>(*reinterpret_cast<const InType *>(p_data + stride * i));
        }
        return buf;
    }
    template <int expected_type, class T> std::vector<T> getDataFromAccessor(int accessor_index) {
        const auto &accessor = model.accessors[accessor_index];
        const auto &buffer_view = model.bufferViews[accessor.bufferView];
        const auto &buffer = model.buffers[buffer_view.buffer];
        const auto p_data = buffer.data.data() + accessor.byteOffset + buffer_view.byteOffset;
        const auto stride = accessor.ByteStride(buffer_view);

        if (expected_type != accessor.type) {
            LOG_ERROR(logger, "gltf loading error, expected accessor type: {}, actual type : {}", expected_type,
                      accessor.type);
            return {};
        }

        if constexpr (expected_type == TINYGLTF_TYPE_SCALAR) {
            switch (accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<float, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<double, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<int8_t, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<int16_t, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<int32_t, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<uint8_t, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<uint16_t, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<uint32_t, T>(p_data, accessor.count, stride);
            }
        } else if constexpr (expected_type == TINYGLTF_TYPE_VEC2) {
            switch (accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<glm::vec2, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<glm::f64vec2, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<glm::i8vec2, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<glm::i16vec2, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<glm::i32vec2, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<glm::u8vec2, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<glm::u16vec2, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<glm::u32vec2, T>(p_data, accessor.count, stride);
            }
        } else if constexpr (expected_type == TINYGLTF_TYPE_VEC3) {
            switch (accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<glm::vec3, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<glm::f64vec3, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<glm::i8vec3, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<glm::i16vec3, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<glm::i32vec3, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<glm::u8vec3, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<glm::u16vec3, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<glm::u32vec3, T>(p_data, accessor.count, stride);
            }
        } else if constexpr (expected_type == TINYGLTF_TYPE_VEC4) {
            switch (accessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_FLOAT:
                return readComponentByType<glm::vec4, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_DOUBLE:
                return readComponentByType<glm::f64vec4, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_BYTE:
                return readComponentByType<glm::i8vec4, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_SHORT:
                return readComponentByType<glm::i16vec4, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_INT:
                return readComponentByType<glm::i32vec4, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                return readComponentByType<glm::u8vec4, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                return readComponentByType<glm::u16vec4, T>(p_data, accessor.count, stride);
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                return readComponentByType<glm::u32vec4, T>(p_data, accessor.count, stride);
            }
        }
        LOG_ERROR(logger, "gltf loading error : unsupported accessor, type={},componentType={}", accessor.type,
                  accessor.componentType);
        return {};
    }
    glm::mat4 nodeTransform(const tinygltf::Node &node) {
        if (node.matrix.size() == 16) {
            glm::mat4 matrix{1.0f};
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    matrix[col][row] = static_cast<float>(node.matrix[static_cast<size_t>(col * 4 + row)]);
                }
            }
            return matrix;
        }

        glm::vec3 translation{0.0f};
        if (node.translation.size() == 3) {
            translation = {
                static_cast<float>(node.translation[0]),
                static_cast<float>(node.translation[1]),
                static_cast<float>(node.translation[2]),
            };
        }

        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        if (node.rotation.size() == 4) {
            rotation = glm::quat{
                static_cast<float>(node.rotation[3]),
                static_cast<float>(node.rotation[0]),
                static_cast<float>(node.rotation[1]),
                static_cast<float>(node.rotation[2]),
            };
        }

        glm::vec3 scale{1.0f};
        if (node.scale.size() == 3) {
            scale = {
                static_cast<float>(node.scale[0]),
                static_cast<float>(node.scale[1]),
                static_cast<float>(node.scale[2]),
            };
        }

        return glm::translate(glm::mat4{1.0f}, translation) * glm::mat4_cast(rotation) *
               glm::scale(glm::mat4{1.0f}, scale);
    }

    void transformVertexData(CommonPolygonVertData &data, const glm::mat4 &transform) {
        for (auto &position : data.pos) {
            position = glm::vec3{transform * glm::vec4{position, 1.0f}};
        }

        const auto normal_transform = glm::transpose(glm::inverse(glm::mat3{transform}));
        for (auto &normal : data.normal) {
            normal = glm::normalize(normal_transform * normal);
        }
        for (auto &tangent : data.tangent) {
            const auto transformed = glm::normalize(normal_transform * glm::vec3{tangent});
            tangent = glm::vec4{transformed, tangent.w};
        }
    }

    void loadNode(const tinygltf::Node &node, const glm::mat4 &parent_transform) {
        const auto world_transform = parent_transform * nodeTransform(node);
        for (const auto child_index : node.children) {
            loadNode(model.nodes[child_index], world_transform);
        }
        if (node.mesh < 0)
            return;
        const auto &mesh = model.meshes[node.mesh];
        for (const auto &primitive : mesh.primitives) {
            CommonPolygonVertData dat;

            if (primitive.indices >= 0)
                dat.indices = getDataFromAccessor<TINYGLTF_TYPE_SCALAR, uint32_t>(primitive.indices);
            if (auto it = primitive.attributes.find("POSITION"); it != primitive.attributes.end())
                dat.pos = getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(it->second);
            if (auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end())
                dat.normal = getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(it->second);
            if (auto it = primitive.attributes.find("TANGENT"); it != primitive.attributes.end())
                dat.tangent = getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::vec4>(it->second);
            if (auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end())
                dat.texcoord = getDataFromAccessor<TINYGLTF_TYPE_VEC2, glm::vec2>(it->second);
            if (auto it = primitive.attributes.find("COLOR_0"); it != primitive.attributes.end()) {
                const auto &tmp_color = getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(it->second);
                dat.color.resize(tmp_color.size());
                std::transform(tmp_color.begin(), tmp_color.end(), dat.color.begin(),
                               [](glm::vec3 v3) { return glm::vec4{v3, 1.0f}; });
            } else if (primitive.material >= 0 &&
                       model.materials[primitive.material].pbrMetallicRoughness.baseColorTexture.index < 0 &&
                       !model.materials[primitive.material].pbrMetallicRoughness.baseColorFactor.empty()) {
                const auto &base_color = model.materials[primitive.material].pbrMetallicRoughness.baseColorFactor;
                const glm::vec4 base_color_vec4{
                    base_color[0],
                    base_color[1],
                    base_color[2],
                    base_color[3],
                };
                dat.color.resize(dat.pos.size());
                std::fill(dat.color.begin(), dat.color.end(), base_color_vec4);
            }
            if (auto it = primitive.attributes.find("JOINTS_0"); it != primitive.attributes.end())
                dat.joint = getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::i16vec4>(it->second);
            if (auto it = primitive.attributes.find("WEIGHTS_0"); it != primitive.attributes.end())
                dat.weight = getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::vec4>(it->second);

            transformVertexData(dat, world_transform);
            auto primitive_info = buf_container.addPrimitiveEntry(std::move(dat));
            tmp_material_primitives[primitive.material].emplace_back(std::move(primitive_info));
        }
    }
    ModelTemplate load() {
        material_map.resize(model.materials.size());
        texture_map.resize(model.textures.size());

        for (int i = 0; i < model.textures.size(); i++) {
            const auto &image = model.images[model.textures[i].source];
            texture_map[i] = mat_container.registerTexture(
                vk::Extent3D{
                    static_cast<uint32_t>(image.width),
                    static_cast<uint32_t>(image.height),
                    1,
                },
                image.image.data());
        }
        for (int i = 0; i < model.materials.size(); i++) {
            const auto &material = model.materials[i];

            const auto base_color_texture_index = material.pbrMetallicRoughness.baseColorTexture.index;
            const auto base_color_texture =
                base_color_texture_index >= 0 ? texture_map[base_color_texture_index] : std_mat.whiteTexture();
            const auto metallic_roughness_texture = metallicRoughnessTextureForMaterial(material);
            const auto normal_texture_index = material.normalTexture.index;
            const auto normal_texture =
                normal_texture_index >= 0 ? texture_map[normal_texture_index] : std_mat.normalDefaultTexture();
            const auto emissive_texture = emissiveTextureForMaterial(material);

            material_map[i] = mat_container.registerMaterial(Pelican::MaterialInfo{
                .vert_shader = std_mat.standardVertShader(),
                .frag_shader = std_mat.standardFragShader(),
                .base_color_texture = base_color_texture,
                .metallic_roughness_texture = metallic_roughness_texture,
                .normal_texture = normal_texture,
                .emissive_texture = emissive_texture
            });
        }

        const auto &scene = model.scenes[model.defaultScene < 0 ? 0 : model.defaultScene];
        for (const auto &node : scene.nodes) {
            loadNode(model.nodes[node], glm::mat4{1.0f});
        }

        ModelTemplate m;
        for (const auto &[local_material_id, primitive] : tmp_material_primitives) {
            m.material_primitives.emplace_back(ModelTemplate::MaterialPrimitives{
                .material =
                    local_material_id >= 0 ? material_map.at(local_material_id) : std_mat.standardTransparentMaterial(),
                .primitives = std::move(primitive),
            });
        }

        return m;
    }
};

GltfLoader::GltfLoader() {}

ModelTemplate GltfLoader::loadGltfBinary(std::string path) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;

    std::string err, warn;
    auto ret = loader.LoadBinaryFromFile(&model, &err, &warn, path);
    if (!warn.empty())
        LOG_WARNING(logger, "loading gltf file \"{}\" : {}", path, warn);
    if (!err.empty())
        LOG_ERROR(logger, "loading gltf file \"{}\" : {}", path, err);
    if (!ret)
        throw std::runtime_error("failed to load gltf file : " + path);

    ModelTemplate model_template;
    InternalGltfLoader tmp_loader{
        GET_MODULE(MaterialContainer),
        GET_MODULE(StandardMaterialResource),
        GET_MODULE(VertBufContainer),
        model,
    };
    return tmp_loader.load();
}

ModelTemplate GltfLoader::loadGltf(std::string path) {
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;

    std::string err, warn;
    auto ret = loader.LoadASCIIFromFile(&model, &err, &warn, path);
    if (!warn.empty())
        LOG_WARNING(logger, "loading gltf file \"{}\" : {}", path, warn);
    if (!err.empty())
        LOG_ERROR(logger, "loading gltf file \"{}\" : {}", path, err);
    if (!ret)
        throw std::runtime_error("failed to load gltf file : " + path);

    ModelTemplate model_template;
    InternalGltfLoader tmp_loader{
        GET_MODULE(MaterialContainer),
        GET_MODULE(StandardMaterialResource),
        GET_MODULE(VertBufContainer),
        model,
    };
    return tmp_loader.load();
}

} // namespace Pelican
