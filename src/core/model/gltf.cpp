#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#include "../log.hpp"
#include "../build_features.hpp"
#include "../material/material.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/standardmaterialresource.hpp"
#include "gltf.hpp"
#include "vatformat.hpp"
#include "vertbufcontainer.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace Pelican {

namespace {

const tinygltf::Value *objectMember(const tinygltf::Value::Object &object, const char *name) {
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found->second;
}

std::optional<std::string> valueString(const tinygltf::Value *value) {
    if (value == nullptr || !value->IsString()) {
        return std::nullopt;
    }
    return value->Get<std::string>();
}

std::optional<int64_t> valueInteger(const tinygltf::Value *value) {
    if (value == nullptr || !value->IsInt()) {
        return std::nullopt;
    }
    return value->Get<int>();
}

std::optional<double> valueNumber(const tinygltf::Value *value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->IsInt()) {
        return static_cast<double>(value->Get<int>());
    }
    if (value->IsNumber()) {
        return value->Get<double>();
    }
    return std::nullopt;
}

std::optional<bool> valueBool(const tinygltf::Value *value) {
    if (value == nullptr || !value->IsBool()) {
        return std::nullopt;
    }
    return value->Get<bool>();
}

std::optional<std::array<double, 3>> valueVec3(const tinygltf::Value *value) {
    if (value == nullptr || !value->IsArray()) {
        return std::nullopt;
    }
    const auto &array = value->Get<tinygltf::Value::Array>();
    if (array.size() != 3) {
        return std::nullopt;
    }

    std::array<double, 3> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        const auto component = valueNumber(&array[i]);
        if (!component) {
            return std::nullopt;
        }
        result[i] = *component;
    }
    return result;
}

VatPrimitiveMeta tinyGltfValueToVatMeta(const tinygltf::Value &extras) {
    VatPrimitiveMeta meta;
    if (!extras.IsObject()) {
        return meta;
    }

    const auto &extras_object = extras.Get<tinygltf::Value::Object>();
    const auto *vat_value = objectMember(extras_object, "pelican.vat");
    if (vat_value == nullptr) {
        return meta;
    }

    meta.present = true;
    if (!vat_value->IsObject()) {
        meta.single_clip_object = false;
        return meta;
    }

    const auto &vat = vat_value->Get<tinygltf::Value::Object>();
    meta.schema = valueString(objectMember(vat, "schema"));
    meta.version = valueInteger(objectMember(vat, "version"));
    meta.generator = valueString(objectMember(vat, "generator"));
    meta.fps = valueNumber(objectMember(vat, "fps"));
    meta.frame_count = valueInteger(objectMember(vat, "frame_count"));
    meta.vertex_count = valueInteger(objectMember(vat, "vertex_count"));
    meta.bounds_min = valueVec3(objectMember(vat, "bounds_min"));
    meta.bounds_max = valueVec3(objectMember(vat, "bounds_max"));
    meta.loop = valueBool(objectMember(vat, "loop"));
    meta.position_view = valueInteger(objectMember(vat, "position_view"));
    meta.normal_view = valueInteger(objectMember(vat, "normal_view"));
    return meta;
}

void rejectVatModelIfDisabled(const tinygltf::Model &model) {
#if PELICAN_WITH_VAT
    (void)model;
#else
    for (const auto &mesh : model.meshes) {
        for (const auto &primitive : mesh.primitives) {
            if (tinyGltfValueToVatMeta(primitive.extras).present) {
                throwBuildFeatureDisabled("PELICAN_WITH_VAT", "GLB contains pelican.vat primitive extras");
            }
        }
    }
#endif
}

#if PELICAN_WITH_VAT

size_t vatTextureBytes(const VatPrimitiveInfo &vat) {
    return static_cast<size_t>(vat.vertex_count) * vat.frame_count * 4 * sizeof(uint16_t);
}

#endif

} // namespace

struct InternalGltfLoader {
    using ModelLocalMaterialId = int;

    MaterialContainer &mat_container;
    StandardMaterialResource &std_mat;
    VertBufContainer &buf_container;
    tinygltf::Model &model;
    std::vector<GlobalMaterialId> material_map;
    std::vector<MaterialInfo> material_infos;
    std::vector<GlobalTextureId> texture_map;
    std::unordered_map<ModelLocalMaterialId, GlobalMaterialId> resolved_materials;
    std::unordered_map<ModelLocalMaterialId, std::vector<ModelTemplate::PrimitiveRefInfo>> tmp_material_primitives;
    ModelLocalMaterialId next_generated_material = -2;

    uint8_t toUnorm8(double value) {
        return static_cast<uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    }

    float toUnormFloat(double value) {
        return static_cast<float>(toUnorm8(value)) / 255.0f;
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
            255, 255, 255);
    }

    GlobalTextureId emissiveTextureForMaterial(const tinygltf::Material &material) {
        const auto texture_index = material.emissiveTexture.index;
        if (texture_index >= 0) {
            return texture_map[texture_index];
        }

        return std_mat.whiteTexture();
    }

#if PELICAN_WITH_VAT
    std::vector<VatBufferViewInfo> vatBufferViewInfos() const {
        std::vector<VatBufferViewInfo> infos;
        infos.reserve(model.bufferViews.size());
        for (size_t i = 0; i < model.bufferViews.size(); ++i) {
            const auto &view = model.bufferViews[i];
            infos.push_back(VatBufferViewInfo{
                .index = static_cast<int>(i),
                .buffer = view.buffer,
                .byte_offset = view.byteOffset,
                .byte_length = view.byteLength,
            });
        }
        return infos;
    }

    const std::byte *bufferViewData(const VatBufferViewSpan &span, size_t required_bytes) const {
        if (span.buffer < 0 || span.buffer >= static_cast<int>(model.buffers.size())) {
            throw std::runtime_error("pelican.vat references invalid buffer index");
        }
        const auto &buffer = model.buffers[span.buffer];
        if (span.byte_offset > buffer.data.size() || required_bytes > buffer.data.size() - span.byte_offset) {
            throw std::runtime_error("pelican.vat bufferView range exceeds GLB buffer data");
        }
        return reinterpret_cast<const std::byte *>(buffer.data.data() + span.byte_offset);
    }

    GlobalTextureId registerVatTexture(const VatBufferViewSpan &span, uint32_t vertex_count,
                                       uint32_t frame_count, size_t required_bytes) const {
        const auto *data = bufferViewData(span, required_bytes);
        return mat_container.registerTexture(vk::Extent3D{vertex_count, frame_count, 1}, data,
                                             vk::Format::eR16G16B16A16Sfloat, required_bytes);
    }

    MaterialInfo materialInfoForPrimitive(int local_material_id) const {
        if (local_material_id >= 0) {
            return material_infos.at(local_material_id);
        }

        return MaterialInfo{
            .vert_shader = std_mat.standardVertShader(),
            .frag_shader = std_mat.standardFragShader(),
            .base_color_texture = std_mat.whiteTexture(),
            .metallic_roughness_texture = std_mat.metallicRoughnessDefaultTexture(),
            .normal_texture = std_mat.normalDefaultTexture(),
            .emissive_texture = std_mat.emissiveDefaultTexture(),
        };
    }

    ModelLocalMaterialId registerVatMaterial(int local_material_id, const VatPrimitiveInfo &vat,
                                             const ModelTemplate::PrimitiveRefInfo &primitive_info) {
        const auto required_bytes = vatTextureBytes(vat);
        auto material_info = materialInfoForPrimitive(local_material_id);
        const auto position_texture =
            registerVatTexture(vat.position_view, vat.vertex_count, vat.frame_count, required_bytes);
        const auto normal_texture =
            vat.normal_view ? registerVatTexture(*vat.normal_view, vat.vertex_count, vat.frame_count,
                                                 required_bytes)
                            : std_mat.normalDefaultTexture();

        material_info.vert_shader = std_mat.vatVertShader();
        material_info.vat = MaterialInfo::VatPlaybackInfo{
            .position_texture = position_texture,
            .normal_texture = normal_texture,
            .bounds_min = vat.bounds_min,
            .bounds_max = vat.bounds_max,
            .fps = static_cast<float>(vat.fps),
            .frame_count = vat.frame_count,
            .base_vertex = primitive_info.vert_offset,
            .loop = vat.loop,
            .has_normal = vat.normal_view.has_value(),
        };

        const auto generated_material = next_generated_material--;
        resolved_materials[generated_material] = mat_container.registerMaterial(material_info);
        return generated_material;
    }
#endif

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
            }
            if (auto it = primitive.attributes.find("JOINTS_0"); it != primitive.attributes.end())
                dat.joint = getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::i16vec4>(it->second);
            if (auto it = primitive.attributes.find("WEIGHTS_0"); it != primitive.attributes.end())
                dat.weight = getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::vec4>(it->second);

            const auto vat_meta = tinyGltfValueToVatMeta(primitive.extras);
#if PELICAN_WITH_VAT
            const auto vat_info = parseVatPrimitiveExtras(
                vat_meta, static_cast<uint32_t>(dat.pos.size()), vatBufferViewInfos());
            const auto primitive_mode = primitive.mode < 0 ? TINYGLTF_MODE_TRIANGLES : primitive.mode;
            if (vat_info && primitive_mode != TINYGLTF_MODE_TRIANGLES) {
                throw std::runtime_error("pelican.vat only supports TRIANGLES topology");
            }
#else
            if (vat_meta.present) {
                throwBuildFeatureDisabled("PELICAN_WITH_VAT", "GLB contains pelican.vat primitive extras");
            }
#endif

            transformVertexData(dat, world_transform);
            auto primitive_info = buf_container.addPrimitiveEntry(std::move(dat));
#if PELICAN_WITH_VAT
            const auto material_id =
                vat_info ? registerVatMaterial(primitive.material, *vat_info, primitive_info)
                         : primitive.material;
#else
            const auto material_id = primitive.material;
#endif
            tmp_material_primitives[material_id].emplace_back(std::move(primitive_info));
        }
    }
    ModelTemplate load() {
        material_map.resize(model.materials.size());
        material_infos.resize(model.materials.size());
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
            const auto &base_factor = material.pbrMetallicRoughness.baseColorFactor;
            const bool has_metallic_roughness_texture =
                material.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0;
            const bool has_emissive_texture = material.emissiveTexture.index >= 0;
            const auto materialFactor = [&](double value) {
                return has_metallic_roughness_texture ? static_cast<float>(value) : toUnormFloat(value);
            };
            const auto emissiveFactor = [&](size_t component) {
                const auto value = vectorValueOr(material.emissiveFactor, component, 0.0);
                return has_emissive_texture ? static_cast<float>(value) : toUnormFloat(value);
            };

            material_infos[i] = Pelican::MaterialInfo{
                .vert_shader = std_mat.standardVertShader(),
                .frag_shader = std_mat.standardFragShader(),
                .base_color_texture = base_color_texture,
                .metallic_roughness_texture = metallic_roughness_texture,
                .normal_texture = normal_texture,
                .emissive_texture = emissive_texture,
                .base_color_factor = glm::vec4{
                    static_cast<float>(vectorValueOr(base_factor, 0, 1.0)),
                    static_cast<float>(vectorValueOr(base_factor, 1, 1.0)),
                    static_cast<float>(vectorValueOr(base_factor, 2, 1.0)),
                    static_cast<float>(vectorValueOr(base_factor, 3, 1.0)),
                },
                .emissive_factor = glm::vec3{
                    emissiveFactor(0), emissiveFactor(1), emissiveFactor(2),
                },
                .metallic_factor = materialFactor(material.pbrMetallicRoughness.metallicFactor),
                .roughness_factor = materialFactor(material.pbrMetallicRoughness.roughnessFactor),
                .normal_scale = static_cast<float>(material.normalTexture.scale),
                .occlusion_strength = static_cast<float>(material.occlusionTexture.strength),
            };
            material_map[i] = mat_container.registerMaterial(material_infos[i]);
            resolved_materials[i] = material_map[i];
        }

        const auto &scene = model.scenes[model.defaultScene < 0 ? 0 : model.defaultScene];
        for (const auto &node : scene.nodes) {
            loadNode(model.nodes[node], glm::mat4{1.0f});
        }

        ModelTemplate m;
        for (const auto &[local_material_id, primitive] : tmp_material_primitives) {
            const auto found = resolved_materials.find(local_material_id);
            m.material_primitives.emplace_back(ModelTemplate::MaterialPrimitives{
                .material = found != resolved_materials.end() ? found->second : std_mat.standardTransparentMaterial(),
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
    rejectVatModelIfDisabled(model);

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
    rejectVatModelIfDisabled(model);

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
