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

namespace Pelican {

struct RecursiveLoader {
    using ModelLocalMaterialId = int;

    MaterialContainer &mat_container;
    StandardMaterialResource &std_mat;
    VertBufContainer &buf_container;
    tinygltf::Model &model;
    std::vector<GlobalMaterialId> material_map;
    std::vector<GlobalTextureId> texture_map;
    std::unordered_map<ModelLocalMaterialId, std::vector<ModelTemplate::PrimitiveRefInfo>> tmp_material_primitives;

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
    void loadNode(const tinygltf::Node &node) {
        for (const auto child_index : node.children) {
            loadNode(model.nodes[child_index]);
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
                base_color_texture_index >= 0 ? texture_map[base_color_texture_index] : std_mat.transparentTexture();
            material_map[i] = mat_container.registerMaterial(Pelican::MaterialInfo{
                .vert_shader = std_mat.standardVertShader(),
                .frag_shader = std_mat.standardFragShader(),
                .base_color_texture = base_color_texture,
            });
        }

        const auto &scene = model.scenes[model.defaultScene < 0 ? 0 : model.defaultScene];
        for (const auto &node : scene.nodes) {
            loadNode(model.nodes[node]);
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

GltfLoader::GltfLoader(DependencyContainer &_con) : con{_con} {}

ModelTemplate GltfLoader::loadGltf(std::string path) {
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
    RecursiveLoader tmp_loader{
        con.get<MaterialContainer>(),
        con.get<StandardMaterialResource>(),
        con.get<VertBufContainer>(),
        model,
    };
    return tmp_loader.load();
}

} // namespace Pelican
