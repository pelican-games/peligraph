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
#include <cctype>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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
    std::string source_path;
    std::optional<AssetFragmentRef> fragment;
    std::vector<std::optional<GlobalMaterialId>> material_map;
    std::vector<MaterialInfo> material_infos;
    std::vector<std::optional<GlobalTextureId>> texture_map;
    std::unordered_map<ModelLocalMaterialId, GlobalMaterialId> resolved_materials;
    std::unordered_map<ModelLocalMaterialId, std::vector<ModelTemplate::PrimitiveRefInfo>> tmp_material_primitives;
    ModelLocalMaterialId next_generated_material = -2;

    struct NodeOccurrence {
        int node_index = -1;
        std::string full_path;
        glm::mat4 parent_transform{1.0f};
    };

    struct FragmentCandidate {
        int object_index = -1;
        int node_index = -1;
        std::string full_path;
        std::vector<std::string> aliases;
        glm::mat4 parent_transform{1.0f};
    };

    struct RootSelection {
        int node_index = -1;
        int mesh_index = -1;
        glm::mat4 parent_transform{1.0f};
        bool subtree = true;
    };

    struct LoadSelection {
        bool whole_model = false;
        std::vector<RootSelection> roots;
        std::optional<int> material_only;
    };

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
            return texture_map.at(texture_index).value();
        }

        return registerSolidTexture(
            255, 255, 255);
    }

    GlobalTextureId emissiveTextureForMaterial(const tinygltf::Material &material) {
        const auto texture_index = material.emissiveTexture.index;
        if (texture_index >= 0) {
            return texture_map.at(texture_index).value();
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

    std::string fragmentReference() const {
        if (!fragment) {
            return source_path;
        }
        return source_path + "#" + fragment->kind + "/" + fragment->path;
    }

    void collectNodeOccurrence(int node_index, const glm::mat4 &parent_transform,
                               const std::string &parent_path,
                               std::vector<NodeOccurrence> &occurrences,
                               std::set<std::pair<int, std::string>> &seen) {
        if (node_index < 0 || node_index >= static_cast<int>(model.nodes.size())) {
            return;
        }
        const auto &node = model.nodes[node_index];
        const auto full_path = parent_path.empty()
                                   ? node.name
                                   : node.name.empty() ? parent_path : parent_path + "/" + node.name;
        if (seen.emplace(node_index, full_path).second) {
            occurrences.push_back(NodeOccurrence{node_index, full_path, parent_transform});
        }
        const auto world_transform = parent_transform * nodeTransform(node);
        for (const auto child : node.children) {
            collectNodeOccurrence(child, world_transform, full_path, occurrences, seen);
        }
    }

    std::vector<NodeOccurrence> nodeOccurrences() {
        std::vector<NodeOccurrence> occurrences;
        std::set<std::pair<int, std::string>> seen;
        std::vector<bool> is_child(model.nodes.size(), false);
        for (const auto &node : model.nodes) {
            for (const auto child : node.children) {
                if (child >= 0 && child < static_cast<int>(is_child.size())) {
                    is_child[child] = true;
                }
            }
        }

        for (const auto &scene : model.scenes) {
            for (const auto root : scene.nodes) {
                collectNodeOccurrence(root, glm::mat4{1.0f}, {}, occurrences, seen);
            }
        }
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            if (!is_child[i]) {
                collectNodeOccurrence(i, glm::mat4{1.0f}, {}, occurrences, seen);
            }
        }
        return occurrences;
    }

    std::vector<FragmentCandidate> candidatesForKind(const std::string &kind) {
        std::vector<FragmentCandidate> candidates;
        if (kind == "node") {
            for (const auto &occurrence : nodeOccurrences()) {
                const auto &node = model.nodes[occurrence.node_index];
                if (occurrence.full_path.empty() || node.name.empty()) {
                    continue;
                }
                candidates.push_back(FragmentCandidate{
                    occurrence.node_index,
                    occurrence.node_index,
                    occurrence.full_path,
                    {node.name},
                    occurrence.parent_transform,
                });
            }
            return candidates;
        }
        if (kind == "mesh") {
            std::vector<bool> referenced(model.meshes.size(), false);
            for (const auto &occurrence : nodeOccurrences()) {
                const auto &node = model.nodes[occurrence.node_index];
                if (node.mesh < 0 || node.mesh >= static_cast<int>(model.meshes.size()) ||
                    occurrence.full_path.empty()) {
                    continue;
                }
                referenced[node.mesh] = true;
                std::vector<std::string> aliases;
                if (!node.name.empty()) {
                    aliases.push_back(node.name);
                }
                const auto &mesh_name = model.meshes[node.mesh].name;
                if (!mesh_name.empty() &&
                    std::find(aliases.begin(), aliases.end(), mesh_name) == aliases.end()) {
                    aliases.push_back(mesh_name);
                }
                candidates.push_back(FragmentCandidate{
                    node.mesh,
                    occurrence.node_index,
                    occurrence.full_path,
                    std::move(aliases),
                    occurrence.parent_transform,
                });
            }
            for (int i = 0; i < static_cast<int>(model.meshes.size()); ++i) {
                if (!referenced[i] && !model.meshes[i].name.empty()) {
                    candidates.push_back(FragmentCandidate{
                        i, -1, model.meshes[i].name, {model.meshes[i].name}, glm::mat4{1.0f}});
                }
            }
            return candidates;
        }

        const auto append_named = [&](const auto &objects) {
            for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
                if (!objects[i].name.empty()) {
                    candidates.push_back(FragmentCandidate{
                        i, -1, objects[i].name, {objects[i].name}, glm::mat4{1.0f}});
                }
            }
        };
        if (kind == "material") {
            append_named(model.materials);
        } else if (kind == "animation") {
            append_named(model.animations);
        }
        return candidates;
    }

    [[noreturn]] void throwDuplicateFullPath(const std::string &kind,
                                             const std::string &full_path,
                                             const std::vector<const FragmentCandidate *> &matches) const {
        std::ostringstream message;
        message << "Duplicate GLB " << kind << " full path '" << full_path << "' in '"
                << fragmentReference() << "' (indices: ";
        for (size_t i = 0; i < matches.size(); ++i) {
            if (i != 0) {
                message << ", ";
            }
            message << matches[i]->object_index;
        }
        message << ")";
        throw std::runtime_error(message.str());
    }

    FragmentCandidate resolveFragmentCandidate(const AssetFragmentRef &requested) {
        if (requested.kind != "mesh" && requested.kind != "material" &&
            requested.kind != "node" && requested.kind != "animation") {
            throw std::runtime_error("Unknown GLB fragment kind '" + requested.kind +
                                     "' in '" + fragmentReference() +
                                     "' (supported: mesh, material, node, animation)");
        }

        auto candidates = candidatesForKind(requested.kind);
        std::unordered_map<std::string, std::vector<const FragmentCandidate *>> by_full_path;
        for (const auto &candidate : candidates) {
            by_full_path[candidate.full_path].push_back(&candidate);
        }
        std::unordered_set<std::string> checked_paths;
        for (const auto &candidate : candidates) {
            if (!checked_paths.insert(candidate.full_path).second) {
                continue;
            }
            const auto &matches = by_full_path.at(candidate.full_path);
            if (matches.size() > 1) {
                throwDuplicateFullPath(requested.kind, candidate.full_path, matches);
            }
        }

        std::vector<const FragmentCandidate *> matches;
        if (requested.address_kind == AssetFragmentAddressKind::full_path) {
            if (const auto found = by_full_path.find(requested.path); found != by_full_path.end()) {
                matches = found->second;
            }
        } else {
            for (const auto &candidate : candidates) {
                if (std::find(candidate.aliases.begin(), candidate.aliases.end(), requested.path) !=
                    candidate.aliases.end()) {
                    matches.push_back(&candidate);
                }
            }
        }

        if (matches.empty()) {
            throw std::runtime_error("Unknown GLB " + requested.kind + " fragment '" +
                                     requested.path + "' in '" + fragmentReference() + "'");
        }
        if (matches.size() > 1) {
            std::ostringstream message;
            message << "Ambiguous GLB " << requested.kind << " fragment name '" << requested.path
                    << "' in '" << fragmentReference() << "'; matches: ";
            for (size_t i = 0; i < matches.size(); ++i) {
                if (i != 0) {
                    message << ", ";
                }
                message << matches[i]->full_path;
            }
            throw std::runtime_error(message.str());
        }
        return *matches.front();
    }

    LoadSelection selectLoad() {
        if (!fragment) {
            LoadSelection selection;
            selection.whole_model = true;
            const auto &scene = model.scenes[model.defaultScene < 0 ? 0 : model.defaultScene];
            for (const auto node : scene.nodes) {
                selection.roots.push_back(RootSelection{node, -1, glm::mat4{1.0f}, true});
            }
            return selection;
        }

        const auto selected = resolveFragmentCandidate(*fragment);
        LoadSelection selection;
        if (fragment->kind == "node") {
            selection.roots.push_back(
                RootSelection{selected.node_index, -1, selected.parent_transform, true});
        } else if (fragment->kind == "mesh") {
            selection.roots.push_back(
                RootSelection{selected.node_index, selected.object_index,
                              selected.parent_transform, false});
        } else if (fragment->kind == "material") {
            selection.material_only = selected.object_index;
        }
        // Animation GPU/runtime data is not represented by ModelTemplate yet. Resolving it here
        // deliberately creates no unrelated mesh, material, or texture resources.
        return selection;
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

    void collectMeshMaterials(int mesh_index, std::set<int> &materials) const {
        if (mesh_index < 0 || mesh_index >= static_cast<int>(model.meshes.size())) {
            return;
        }
        for (const auto &primitive : model.meshes[mesh_index].primitives) {
            if (primitive.material >= 0) {
                materials.insert(primitive.material);
            }
        }
    }

    void collectNodeMaterials(int node_index, bool subtree, std::set<int> &materials) const {
        if (node_index < 0 || node_index >= static_cast<int>(model.nodes.size())) {
            return;
        }
        const auto &node = model.nodes[node_index];
        collectMeshMaterials(node.mesh, materials);
        if (subtree) {
            for (const auto child_index : node.children) {
                collectNodeMaterials(child_index, true, materials);
            }
        }
    }

    void loadTexture(int texture_index) {
        const auto &image = model.images.at(model.textures.at(texture_index).source);
        texture_map.at(texture_index) = mat_container.registerTexture(
            vk::Extent3D{
                static_cast<uint32_t>(image.width),
                static_cast<uint32_t>(image.height),
                1,
            },
            image.image.data());
    }

    void loadMaterial(int material_index) {
        const auto &material = model.materials.at(material_index);

        const auto base_color_texture_index =
            material.pbrMetallicRoughness.baseColorTexture.index;
        const auto base_color_texture = base_color_texture_index >= 0
                                            ? texture_map.at(base_color_texture_index).value()
                                            : std_mat.whiteTexture();
        const auto metallic_roughness_texture = metallicRoughnessTextureForMaterial(material);
        const auto normal_texture_index = material.normalTexture.index;
        const auto normal_texture = normal_texture_index >= 0
                                        ? texture_map.at(normal_texture_index).value()
                                        : std_mat.normalDefaultTexture();
        const auto emissive_texture = emissiveTextureForMaterial(material);
        const auto &base_factor = material.pbrMetallicRoughness.baseColorFactor;
        const bool has_metallic_roughness_texture =
            material.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0;
        const auto materialFactor = [&](double value) {
            return has_metallic_roughness_texture ? static_cast<float>(value)
                                                  : toUnormFloat(value);
        };
        const auto emissiveFactor = [&](size_t component) {
            return static_cast<float>(vectorValueOr(material.emissiveFactor, component, 0.0));
        };
        float emissive_strength = 1.0f;
        if (const auto extension = material.extensions.find("KHR_materials_emissive_strength");
            extension != material.extensions.end() && extension->second.IsObject()) {
            const auto &object = extension->second.Get<tinygltf::Value::Object>();
            if (const auto strength = valueNumber(objectMember(object, "emissiveStrength"))) {
                emissive_strength = static_cast<float>(*strength);
            }
        }

        material_infos.at(material_index) = Pelican::MaterialInfo{
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
                emissiveFactor(0) * emissive_strength,
                emissiveFactor(1) * emissive_strength,
                emissiveFactor(2) * emissive_strength,
            },
            .metallic_factor = materialFactor(material.pbrMetallicRoughness.metallicFactor),
            .roughness_factor = materialFactor(material.pbrMetallicRoughness.roughnessFactor),
            .normal_scale = static_cast<float>(material.normalTexture.scale),
            .occlusion_strength = static_cast<float>(material.occlusionTexture.strength),
        };
        material_map.at(material_index) =
            mat_container.registerMaterial(material_infos.at(material_index));
        resolved_materials[material_index] = material_map.at(material_index).value();
    }

    std::set<int> texturesForMaterials(const std::set<int> &materials) const {
        std::set<int> textures;
        const auto append = [&](int texture_index) {
            if (texture_index >= 0) {
                textures.insert(texture_index);
            }
        };
        for (const auto material_index : materials) {
            const auto &material = model.materials.at(material_index);
            append(material.pbrMetallicRoughness.baseColorTexture.index);
            append(material.pbrMetallicRoughness.metallicRoughnessTexture.index);
            append(material.normalTexture.index);
            append(material.emissiveTexture.index);
        }
        return textures;
    }

    void loadMesh(int mesh_index, const glm::mat4 &world_transform) {
        const auto &mesh = model.meshes.at(mesh_index);
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

    void loadNode(int node_index, const glm::mat4 &parent_transform, bool subtree = true) {
        const auto &node = model.nodes.at(node_index);
        const auto world_transform = parent_transform * nodeTransform(node);
        if (subtree) {
            for (const auto child_index : node.children) {
                loadNode(child_index, world_transform);
            }
        }
        if (node.mesh >= 0) {
            loadMesh(node.mesh, world_transform);
        }
    }

    ModelTemplate load() {
        material_map.resize(model.materials.size());
        material_infos.resize(model.materials.size());
        texture_map.resize(model.textures.size());

        const auto selection = selectLoad();
        if (selection.whole_model) {
            for (int i = 0; i < static_cast<int>(model.textures.size()); ++i) {
                loadTexture(i);
            }
            for (int i = 0; i < static_cast<int>(model.materials.size()); ++i) {
                loadMaterial(i);
            }
        } else {
            std::set<int> selected_materials;
            if (selection.material_only) {
                selected_materials.insert(*selection.material_only);
            }
            for (const auto &root : selection.roots) {
                if (root.node_index >= 0) {
                    collectNodeMaterials(root.node_index, root.subtree, selected_materials);
                } else {
                    collectMeshMaterials(root.mesh_index, selected_materials);
                }
            }
            for (const auto texture_index : texturesForMaterials(selected_materials)) {
                loadTexture(texture_index);
            }
            for (const auto material_index : selected_materials) {
                loadMaterial(material_index);
            }
        }

        for (const auto &root : selection.roots) {
            if (root.node_index >= 0) {
                loadNode(root.node_index, root.parent_transform, root.subtree);
            } else if (root.mesh_index >= 0) {
                loadMesh(root.mesh_index, glm::mat4{1.0f});
            }
        }

        ModelTemplate m;
        for (const auto &[local_material_id, primitive] : tmp_material_primitives) {
            const auto found = resolved_materials.find(local_material_id);
            m.material_primitives.emplace_back(ModelTemplate::MaterialPrimitives{
                .material = found != resolved_materials.end() ? found->second : std_mat.standardTransparentMaterial(),
                .primitives = std::move(primitive),
            });
        }
        if (selection.material_only) {
            m.material_primitives.emplace_back(ModelTemplate::MaterialPrimitives{
                .material = resolved_materials.at(*selection.material_only),
                .primitives = {},
            });
        }

        return m;
    }
};

GltfLoader::GltfLoader() {}

ModelTemplate GltfLoader::loadGltfBinary(std::string path,
                                         std::optional<AssetFragmentRef> fragment) {
    auto extension = std::filesystem::path{path}.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (fragment && extension != ".glb") {
        throw std::runtime_error("GLB fragment reference requires a .glb file: " + path + "#" +
                                 fragment->kind + "/" + fragment->path);
    }
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
    if (!fragment) {
        rejectVatModelIfDisabled(model);
    }

    ModelTemplate model_template;
    InternalGltfLoader tmp_loader{
        GET_MODULE(MaterialContainer),
        GET_MODULE(StandardMaterialResource),
        GET_MODULE(VertBufContainer),
        model,
        path,
        std::move(fragment),
    };
    return tmp_loader.load();
}

ModelTemplate GltfLoader::loadGltf(std::string path,
                                   std::optional<AssetFragmentRef> fragment) {
    auto extension = std::filesystem::path{path}.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (fragment && extension != ".gltf") {
        throw std::runtime_error("glTF fragment reference requires a .gltf file: " + path + "#" +
                                 fragment->kind + "/" + fragment->path);
    }
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
    if (!fragment) {
        rejectVatModelIfDisabled(model);
    }

    ModelTemplate model_template;
    InternalGltfLoader tmp_loader{
        GET_MODULE(MaterialContainer),
        GET_MODULE(StandardMaterialResource),
        GET_MODULE(VertBufContainer),
        model,
        path,
        std::move(fragment),
    };
    return tmp_loader.load();
}

} // namespace Pelican
