// The tinygltf / stb implementations live in tinygltf_implementation.cpp:
// this file and gltfaccessorreader.hpp both include <tiny_gltf.h>, and the
// implementation block sits outside its include guard.
#include "../log.hpp"
#include "../build_features.hpp"
#include "../loader/engineresources.hpp"
#include "../material/material.hpp"
#include "../material/materialcontainer.hpp"
#include "../material/materialshaderconfig.hpp"
#include "../material/standardmaterialresource.hpp"
#include "../renderingpass/framegraphruntime.hpp"
#include "../renderingpass/renderingpasscontainer.hpp"
#include "../../project/gltfmateriallowering.hpp"
#include "engineprimitives.hpp"
#include "gltf.hpp"
#include "gltfaccessorreader.hpp"
#include "gltfimage.hpp"
#include "vatformat.hpp"
#include "vertbufcontainer.hpp"
#include "vrmsemantic.hpp"
#include "vrmfirstperson.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>
#include <optional>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {

using GltfInternal::GltfAccessorReader;

namespace {

std::atomic<std::uint64_t> next_morph_layout_generation{1};

std::uint64_t allocateMorphLayoutGeneration() {
    const auto generation =
        next_morph_layout_generation.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0 || generation == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("glTF morph target layout generation exhausted");
    return generation;
}

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

std::optional<std::array<double, 2>> valueVec2(const tinygltf::Value *value) {
    if (value == nullptr || !value->IsArray()) {
        return std::nullopt;
    }
    const auto &array = value->Get<tinygltf::Value::Array>();
    if (array.size() != 2) {
        return std::nullopt;
    }

    std::array<double, 2> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        const auto component = valueNumber(&array[i]);
        if (!component) {
            return std::nullopt;
        }
        result[i] = *component;
    }
    return result;
}

struct GltfBaseColorUvTransform {
    glm::vec2 offset{0.0F};
    glm::vec2 scale{1.0F};
    float rotation = 0.0F;
    NormalizedAlphaUvTransformState state =
        NormalizedAlphaUvTransformState::absent;
};

template <class TextureInfo>
GltfBaseColorUvTransform baseColorUvTransform(
    const TextureInfo &base_texture) {
    const auto extension =
        base_texture.extensions.find("KHR_texture_transform");
    if (extension == base_texture.extensions.end()) return {};
    const auto invalid = [] {
        GltfBaseColorUvTransform result;
        result.state = NormalizedAlphaUvTransformState::invalid;
        return result;
    };
    if (!extension->second.IsObject()) return invalid();
    const auto &object =
        extension->second.Get<tinygltf::Value::Object>();
    GltfBaseColorUvTransform result;
    result.state = NormalizedAlphaUvTransformState::identity;
    if (const auto *value = objectMember(object, "offset")) {
        const auto offset = valueVec2(value);
        if (!offset) return invalid();
        result.offset = {static_cast<float>((*offset)[0]),
                         static_cast<float>((*offset)[1])};
    }
    if (const auto *value = objectMember(object, "scale")) {
        const auto scale = valueVec2(value);
        if (!scale) return invalid();
        result.scale = {static_cast<float>((*scale)[0]),
                        static_cast<float>((*scale)[1])};
    }
    if (const auto *value = objectMember(object, "rotation")) {
        const auto rotation = valueNumber(value);
        if (!rotation) return invalid();
        result.rotation = static_cast<float>(*rotation);
    }
    if (const auto *value = objectMember(object, "texCoord")) {
        const auto tex_coord = valueInteger(value);
        if (!tex_coord || *tex_coord < 0) return invalid();
        if (*tex_coord != base_texture.texCoord) {
            result.state =
                NormalizedAlphaUvTransformState::non_identity;
        }
    }
    if (!std::isfinite(result.offset.x) ||
        !std::isfinite(result.offset.y) ||
        !std::isfinite(result.scale.x) ||
        !std::isfinite(result.scale.y) ||
        !std::isfinite(result.rotation)) {
        return invalid();
    }
    if (result.offset != glm::vec2{0.0F} ||
        result.scale != glm::vec2{1.0F} ||
        result.rotation != 0.0F) {
        result.state =
            NormalizedAlphaUvTransformState::non_identity;
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

std::optional<std::string> tinyGltfExtrasId(const tinygltf::Value &extras) {
    if (!extras.IsObject()) return std::nullopt;
    const auto &object = extras.Get<tinygltf::Value::Object>();
    const auto *value = objectMember(object, "id");
    if (value == nullptr) return std::nullopt;
    if (!value->IsString())
        throw std::runtime_error("glTF extras.id must be a string");
    return value->Get<std::string>();
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

size_t vatTextureBytes(const VatPrimitiveInfo &vat) {
    return static_cast<size_t>(vat.vertex_count) * vat.frame_count * 4 * sizeof(uint16_t);
}

} // namespace

struct PreparedGltf::Impl {
    tinygltf::Model model;
    std::string source_path;
    std::optional<AssetFragmentRef> fragment;
    std::string warning;
    std::string error;
    bool scene_node_instance = false;
    VrmSemanticDecodeResult vrm;
    std::optional<CanonicalModelAssetKey> asset_key;
    std::map<int, std::filesystem::path> buffer_source_files;
    // GP-5: built in memory from engineprimitives; source_path is the
    // engine:// reference and no file backs it.
    bool generated = false;
};

std::vector<std::filesystem::path> PreparedGltf::dataFiles() const {
    if (!impl) throw std::invalid_argument("cannot inspect data files of an empty prepared glTF");
    if (impl->generated) return {};
    std::set<std::filesystem::path> files{std::filesystem::weakly_canonical(impl->source_path)};
    for (const auto &[index, file] : impl->buffer_source_files) files.insert(file);
    return {files.begin(), files.end()};
}

namespace {

std::shared_ptr<PreparedGltf::Impl> prepareGltfImpl(std::string path,
                                                    std::optional<AssetFragmentRef> fragment,
                                                    bool binary,
                                                    std::optional<CanonicalModelAssetKey> asset_key) {
    auto extension = std::filesystem::path{path}.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const auto required_extension = binary ? ".glb" : ".gltf";
    if (fragment && extension != required_extension) {
        throw std::runtime_error(std::string{binary ? "GLB" : "glTF"} +
                                 " fragment reference requires a " + required_extension + " file: " +
                                 path + "#" + fragment->kind + "/" + fragment->path);
    }

    auto prepared = std::make_shared<PreparedGltf::Impl>();
    prepared->source_path = std::move(path);
    prepared->fragment = std::move(fragment);
    prepared->asset_key = std::move(asset_key);
    tinygltf::TinyGLTF loader;
    // TinyGLTF swaps/moves the file byte vectors into model buffers. Associate
    // their storage with the actual file read only during this parse, then keep
    // buffer indices and paths; no pointer identity escapes the preparation.
    std::map<const unsigned char *, std::filesystem::path> read_files;
    loader.SetFsCallbacks({
        tinygltf::FileExists, tinygltf::ExpandFilePath,
        [&read_files](std::vector<unsigned char> *bytes, std::string *error,
                   const std::string &filename, void *) {
            if (!tinygltf::ReadWholeFile(bytes, error, filename, nullptr)) return false;
            const auto path = std::filesystem::weakly_canonical(std::filesystem::path{filename});
            read_files[bytes->data()] = path;
            return true;
        }, tinygltf::WriteWholeFile, tinygltf::GetFileSizeInBytes, nullptr});
    GltfInternal::EncodedImages encoded_images;
    loader.SetImageLoader(GltfInternal::retainEncodedImage, &encoded_images);
    const auto loaded = binary
                            ? loader.LoadBinaryFromFile(&prepared->model, &prepared->error,
                                                        &prepared->warning, prepared->source_path)
                            : loader.LoadASCIIFromFile(&prepared->model, &prepared->error,
                                                       &prepared->warning, prepared->source_path);
    if (!loaded) {
        throw std::runtime_error("failed to load gltf file : " + prepared->source_path +
                                 (prepared->error.empty() ? std::string{} : " (" + prepared->error + ")"));
    }
    for (std::size_t index = 0; index < prepared->model.buffers.size(); ++index) {
        const auto &buffer = prepared->model.buffers[index];
        if (buffer.uri.empty() || buffer.uri.starts_with("data:")) continue;
        prepared->buffer_source_files.emplace(static_cast<int>(index), read_files.at(buffer.data.data()));
    }
    GltfInternal::decodeImagesInParallel(prepared->model, encoded_images);
    prepared->vrm = decodeVrmSemantic(prepared->model, prepared->source_path);
    if (!prepared->fragment) {
        rejectVatModelIfDisabled(prepared->model);
    }
    return prepared;
}

// GP-5: the generated shape as the glTF document a file holding it would
// parse to -- one buffer, one mesh with one indexed triangle primitive, one
// node, one scene, and one material (the engine's default grey, lowered like
// any glTF material onto the OpenPBR base surface). Everything after this
// point is the ordinary glTF path.
std::shared_ptr<PreparedGltf::Impl> prepareEnginePrimitiveImpl(
    std::string_view reference, std::optional<CanonicalModelAssetKey> asset_key) {
    const auto mesh = generateEnginePrimitive(reference);
    auto prepared = std::make_shared<PreparedGltf::Impl>();
    prepared->source_path = std::string{reference};
    prepared->asset_key = std::move(asset_key);
    prepared->generated = true;
    auto &model = prepared->model;
    const auto name = std::string{reference.substr(enginePrimitivePrefix.size())};

    tinygltf::Buffer buffer;
    const auto append_view = [&](const void *data, std::size_t bytes, int target) {
        const auto offset = buffer.data.size();
        const auto *begin = static_cast<const unsigned char *>(data);
        buffer.data.insert(buffer.data.end(), begin, begin + bytes);
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = offset;
        view.byteLength = bytes;
        view.target = target;
        model.bufferViews.push_back(std::move(view));
        return static_cast<int>(model.bufferViews.size() - 1);
    };
    const auto append_accessor = [&](const std::vector<float> &values, int type, int width) {
        tinygltf::Accessor accessor;
        accessor.bufferView = append_view(values.data(), values.size() * sizeof(float),
                                          TINYGLTF_TARGET_ARRAY_BUFFER);
        accessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        accessor.type = type;
        accessor.count = values.size() / static_cast<std::size_t>(width);
        model.accessors.push_back(std::move(accessor));
        return static_cast<int>(model.accessors.size() - 1);
    };

    tinygltf::Primitive primitive;
    primitive.mode = TINYGLTF_MODE_TRIANGLES;
    primitive.material = 0;
    const auto position = append_accessor(mesh.positions, TINYGLTF_TYPE_VEC3, 3);
    // glTF requires POSITION bounds.
    auto &bounds = model.accessors.at(static_cast<std::size_t>(position));
    bounds.minValues = {mesh.positions[0], mesh.positions[1], mesh.positions[2]};
    bounds.maxValues = bounds.minValues;
    for (std::size_t index = 0; index < mesh.positions.size(); ++index) {
        bounds.minValues[index % 3] = std::min<double>(bounds.minValues[index % 3], mesh.positions[index]);
        bounds.maxValues[index % 3] = std::max<double>(bounds.maxValues[index % 3], mesh.positions[index]);
    }
    primitive.attributes["POSITION"] = position;
    primitive.attributes["NORMAL"] = append_accessor(mesh.normals, TINYGLTF_TYPE_VEC3, 3);
    primitive.attributes["TEXCOORD_0"] = append_accessor(mesh.texcoords, TINYGLTF_TYPE_VEC2, 2);
    primitive.attributes["TANGENT"] = append_accessor(mesh.tangents, TINYGLTF_TYPE_VEC4, 4);
    {
        tinygltf::Accessor indices;
        indices.bufferView = append_view(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t),
                                         TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        indices.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        indices.type = TINYGLTF_TYPE_SCALAR;
        indices.count = mesh.indices.size();
        model.accessors.push_back(std::move(indices));
        primitive.indices = static_cast<int>(model.accessors.size() - 1);
    }
    buffer.name = name;
    model.buffers.push_back(std::move(buffer));

    tinygltf::Material material;
    material.name = std::string{enginePrimitiveDefaultMaterialName};
    material.pbrMetallicRoughness.baseColorFactor = {0.8, 0.8, 0.8, 1.0};
    material.pbrMetallicRoughness.metallicFactor = 0.0;
    material.pbrMetallicRoughness.roughnessFactor = 0.5;
    model.materials.push_back(std::move(material));

    tinygltf::Mesh gltf_mesh;
    gltf_mesh.name = name;
    gltf_mesh.primitives.push_back(std::move(primitive));
    model.meshes.push_back(std::move(gltf_mesh));

    tinygltf::Node node;
    node.name = name;
    node.mesh = 0;
    model.nodes.push_back(std::move(node));
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(std::move(scene));
    model.defaultScene = 0;
    model.asset.version = "2.0";
    model.asset.generator = "pelican engine primitives";
    return prepared;
}

} // namespace

namespace {

class GltfResourceSink {
  public:
    struct AddedPrimitive {
        ModelTemplate::PrimitiveRefInfo primitive;
        std::vector<MorphTargetDeltaRange> morph_ranges;
    };

    virtual ~GltfResourceSink() = default;
    virtual GlobalTextureId registerTexture(vk::Extent3D extent, const void *data,
                                            vk::Format format,
                                            vk::DeviceSize bytes) = 0;
    virtual GlobalMaterialId registerMaterial(MaterialInfo info) = 0;
    virtual AddedPrimitive addPrimitive(CommonPolygonVertData data, bool skinned) = 0;
    virtual std::shared_ptr<ModelGpuResources> finish() = 0;
};

void validateCandidatePrimitive(const CommonPolygonVertData &data, bool skinned) {
    const auto count = data.pos.size();
    if (count == 0 || count > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("invalid primitive: POSITION stream is empty or too large");
    const auto matches = [count](std::size_t size) { return size == 0 || size == count; };
    if (!matches(data.normal.size()) || !matches(data.tangent.size()) ||
        !matches(data.texcoord.size()) || !matches(data.color.size()))
        throw std::runtime_error("invalid primitive: vertex attribute counts do not match POSITION");
    if (data.indices.size() > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error("model primitive index stream is too large");
    if (skinned && (data.joint.size() != count || data.weight.size() != count))
        throw std::runtime_error(
            "invalid skinned primitive: POSITION, JOINTS_0, and WEIGHTS_0 counts must match");
    if (data.morph_targets.size() > maxMorphTargetsPerPrimitive)
        throw std::runtime_error("glTF morph target count exceeds renderer limit of " +
                                 std::to_string(maxMorphTargetsPerPrimitive));
    if (data.morph_weight_offset > maxMorphWeightsPerInstance ||
        data.morph_targets.size() >
            maxMorphWeightsPerInstance - data.morph_weight_offset)
        throw std::runtime_error("glTF morph layout exceeds per-instance weight capacity of " +
                                 std::to_string(maxMorphWeightsPerInstance));
    for (const auto &target : data.morph_targets) {
        if (!matches(target.position.size()) || !matches(target.normal.size()) ||
            !matches(target.tangent.size()))
            throw std::runtime_error(
                "invalid glTF morph target: delta accessor count does not match POSITION");
    }
    for (const auto index : data.indices) {
        if (index >= count) throw std::runtime_error("glTF primitive index exceeds POSITION count");
    }
}

class ValidationGltfResourceSink final : public GltfResourceSink {
    int next_texture_ = -1000000;
    int next_material_ = -1000000;
    uint32_t next_index_ = 0;
    uint32_t next_vertex_ = 0;
    uint32_t next_skin_vertex_ = 0;
    uint32_t next_morph_delta_ = 0;
    std::uint64_t next_geometry_allocation_id_ = 1;

  public:
    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data,
                                    vk::Format, vk::DeviceSize bytes) override {
        if (extent.width == 0 || extent.height == 0 || extent.depth == 0 ||
            data == nullptr || bytes == 0)
            throw std::runtime_error("glTF texture candidate is empty");
        return GlobalTextureId{next_texture_--};
    }

    GlobalMaterialId registerMaterial(MaterialInfo) override {
        return GlobalMaterialId{next_material_--};
    }

    AddedPrimitive addPrimitive(CommonPolygonVertData data, bool skinned) override {
        validateCandidatePrimitive(data, skinned);
        const auto attribute_presence =
            (data.normal.empty() ? 0u : modelPrimitiveAttributeNormal) |
            (data.texcoord.empty() ? 0u : modelPrimitiveAttributeUv0) |
            (data.color.empty() ? 0u : modelPrimitiveAttributeColor0);
        const auto vertex_count = static_cast<uint32_t>(data.pos.size());
        const auto index_count = data.indices.empty()
                                     ? vertex_count
                                     : static_cast<uint32_t>(data.indices.size());
        auto &vertex_offset = skinned ? next_skin_vertex_ : next_vertex_;
        if (index_count > std::numeric_limits<uint32_t>::max() - next_index_ ||
            vertex_count > std::numeric_limits<uint32_t>::max() - vertex_offset ||
            vertex_offset > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            throw std::runtime_error("glTF candidate geometry exceeds draw address space");
        ModelTemplate::PrimitiveRefInfo primitive{index_count, next_index_,
                                                  static_cast<int32_t>(vertex_offset), skinned};
        primitive.bounds_source = makePrimitiveBoundsSource(data);
        primitive.vertex_count = vertex_count;
        primitive.morph_deformed = !data.morph_targets.empty();
        primitive.geometry_allocation_id =
            next_geometry_allocation_id_++;
        primitive.ray_attribute_presence = attribute_presence;
        std::vector<MorphTargetDeltaRange> ranges;
        if (!data.morph_targets.empty()) {
            const auto count64 = static_cast<std::uint64_t>(vertex_count) *
                                 data.morph_targets.size();
            if (count64 > maxMorphDeltaRecords ||
                next_morph_delta_ > maxMorphDeltaRecords - count64)
                throw std::runtime_error("shared glTF morph delta buffer capacity exceeded");
            ranges.reserve(data.morph_targets.size());
            for (uint32_t target = 0;
                 target < static_cast<uint32_t>(data.morph_targets.size()); ++target) {
                ranges.push_back({target,
                                  next_morph_delta_ + target * vertex_count,
                                  vertex_count,
                                  data.morph_targets[target].presence_mask});
            }
            next_morph_delta_ += static_cast<uint32_t>(count64);
        }
        next_index_ += index_count;
        vertex_offset += vertex_count;
        return {primitive, std::move(ranges)};
    }

    std::shared_ptr<ModelGpuResources> finish() override { return {}; }
};

class LiveGltfResourceSink final : public GltfResourceSink {
    MaterialContainer &materials_;
    VertBufContainer &geometry_;
    std::shared_ptr<ModelGpuResources> owned_ = std::make_shared<ModelGpuResources>();
    bool committed_ = false;

  public:
    LiveGltfResourceSink(MaterialContainer &materials, VertBufContainer &geometry)
        : materials_{materials}, geometry_{geometry} {}

    ~LiveGltfResourceSink() override {
        const auto report = release();
        for (const auto &error : report.view()) {
            if (logger) {
                LOG_ERROR(logger,
                          "failed glTF candidate release: stage={} code={} index={}",
                          releaseStageName(error.stage),
                          releaseErrorCodeName(error.code), error.index);
            }
        }
    }

    ReleaseReport release() noexcept {
        ReleaseReport report;
        if (committed_ || !owned_) return report;
        report.append(materials_.releaseModelResources(
            std::move(owned_->materials), std::move(owned_->textures),
            false));
        report.append(geometry_.releaseModelGeometry(
            std::move(owned_->geometry), false));
        owned_.reset();
        return report;
    }

    GlobalTextureId registerTexture(vk::Extent3D extent, const void *data,
                                    vk::Format format, vk::DeviceSize bytes) override {
        // Reserve bookkeeping before creating the Vulkan resource so an
        // allocation failure cannot leave an untracked candidate behind.
        owned_->textures.reserve(owned_->textures.size() + 1);
        const auto id = materials_.registerTexture(extent, data, format, bytes);
        owned_->textures.push_back(id);
        return id;
    }

    GlobalMaterialId registerMaterial(MaterialInfo info) override {
        owned_->materials.reserve(owned_->materials.size() + 1);
        const auto id = materials_.registerMaterial(std::move(info));
        owned_->materials.push_back(id);
        return id;
    }

    AddedPrimitive addPrimitive(CommonPolygonVertData data, bool skinned) override {
        owned_->geometry.reserve(owned_->geometry.size() + 1);
        std::vector<MorphTargetDeltaRange> ranges;
        ranges.reserve(data.morph_targets.size());
        auto allocation = skinned
                              ? geometry_.addSkinnedPrimitiveAllocation(std::move(data))
                              : geometry_.addPrimitiveAllocation(std::move(data));
        const auto primitive = allocation.primitive;
        if (allocation.morph_delta_count != 0) {
            const auto target_count = allocation.morph_delta_count / allocation.vertex_count;
            for (uint32_t target = 0; target < target_count; ++target) {
                ranges.push_back({target,
                                  allocation.morph_delta_offset + target * allocation.vertex_count,
                                  allocation.vertex_count,
                                  0});
            }
        }
        owned_->geometry.push_back(std::move(allocation));
        return {primitive, std::move(ranges)};
    }

    std::shared_ptr<ModelGpuResources> finish() override {
        committed_ = true;
        return std::move(owned_);
    }
};

} // namespace

namespace {

// How the VRM firstPerson extension annotates the meshes of one node.
// `automatic` is the "auto" annotation, which splits a skinned primitive's
// triangles by whether they are head-related.
enum class FirstPersonAnnotation {
    automatic,
    both,
    first_person_only,
    third_person_only,
};

FirstPersonAnnotation firstPersonAnnotation(
    const std::shared_ptr<const VrmSemanticData> &vrm_semantic, int node_index) {
    if (!vrm_semantic) return FirstPersonAnnotation::both;
    std::string_view annotation = "auto";
    if (vrm_semantic->first_person) {
        for (const auto &candidate :
             vrm_semantic->first_person->mesh_annotations) {
            if (candidate.node == node_index) {
                annotation = candidate.type;
                break;
            }
        }
    }
    if (annotation == "auto") return FirstPersonAnnotation::automatic;
    if (annotation == "both") return FirstPersonAnnotation::both;
    if (annotation == "firstPersonOnly")
        return FirstPersonAnnotation::first_person_only;
    if (annotation == "thirdPersonOnly")
        return FirstPersonAnnotation::third_person_only;
    throw std::runtime_error("VRM firstPerson annotation has unknown type '" +
                             std::string{annotation} + "'");
}

// The head subtree mask, computed on first use and cached in `cache` (the
// importer's `head_related_nodes` member) because every automatic primitive of
// every mesh needs the same walk.
const std::vector<std::uint8_t> &headRelatedNodes(
    const tinygltf::Model &model,
    const std::shared_ptr<const VrmSemanticData> &vrm_semantic,
    std::optional<std::vector<std::uint8_t>> &cache) {
    if (cache) return *cache;
    std::vector<std::uint8_t> related(model.nodes.size());
    if (vrm_semantic) {
        const auto head = std::find_if(
            vrm_semantic->human_bones.begin(),
            vrm_semantic->human_bones.end(),
            [](const VrmHumanBone &bone) { return bone.name == "head"; });
        if (head != vrm_semantic->human_bones.end() && head->node >= 0) {
            std::vector<int> pending{head->node};
            while (!pending.empty()) {
                const auto node = pending.back();
                pending.pop_back();
                if (node < 0 || node >= static_cast<int>(model.nodes.size()))
                    throw std::runtime_error(
                        "VRM head hierarchy references an invalid glTF node");
                if (related[static_cast<std::size_t>(node)] != 0) continue;
                related[static_cast<std::size_t>(node)] = 1;
                const auto &children = model.nodes[node].children;
                pending.insert(pending.end(), children.begin(), children.end());
            }
        }
    }
    cache = std::move(related);
    return *cache;
}

void transformVertexData(CommonPolygonVertData &data, const glm::mat4 &transform) {
    for (auto &position : data.pos) {
        position = glm::vec3{transform * glm::vec4{position, 1.0f}};
    }

    const auto normal_transform = glm::transpose(glm::inverse(glm::mat3{transform}));
    for (auto &normal : data.normal) {
        const auto transformed = normal_transform * normal;
        normal = data.morph_targets.empty() &&
                         glm::dot(transformed, transformed) > 0.0f
                     ? glm::normalize(transformed)
                     : transformed;
    }
    for (auto &tangent : data.tangent) {
        const auto unnormalized =
            normal_transform * glm::vec3{tangent};
        const auto transformed =
            data.morph_targets.empty() &&
                    glm::dot(unnormalized, unnormalized) > 0.0f
                ? glm::normalize(unnormalized)
                : unnormalized;
        tangent = glm::vec4{transformed, tangent.w};
    }
    const auto position_transform = glm::mat3{transform};
    for (auto &target : data.morph_targets) {
        for (auto &position : target.position)
            position = position_transform * position;
        for (auto &normal : target.normal)
            normal = normal_transform * normal;
        for (auto &tangent : target.tangent)
            tangent = normal_transform * tangent;
    }
}

std::vector<glm::vec3> readMorphDeltaAccessor(
    const tinygltf::Model &model, const std::string &source_path,
    int accessor_index, std::size_t expected_count,
    const std::string &context) {
    if (accessor_index < 0 ||
        accessor_index >= static_cast<int>(model.accessors.size()))
        throw std::runtime_error(context + " references an invalid accessor index");
    const auto &accessor = model.accessors[accessor_index];
    if (accessor.type != TINYGLTF_TYPE_VEC3 ||
        accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
        accessor.normalized)
        throw std::runtime_error(
            context + " accessor must be non-normalized FLOAT VEC3");
    if (accessor.count != expected_count)
        throw std::runtime_error(
            context + " accessor count does not match primitive POSITION count");
    if (accessor.sparse.isSparse)
        throw std::runtime_error(
            context + " uses a sparse accessor, unsupported by morph target v1");
    if (accessor.bufferView < 0 ||
        accessor.bufferView >= static_cast<int>(model.bufferViews.size()))
        throw std::runtime_error(context + " accessor has no valid bufferView");
    const auto &view = model.bufferViews[accessor.bufferView];
    if (view.buffer < 0 || view.buffer >= static_cast<int>(model.buffers.size()))
        throw std::runtime_error(context + " accessor bufferView has an invalid buffer");
    const auto values = GltfAccessorReader{model, source_path}
                            .getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(
                                accessor_index);
    for (const auto &value : values) {
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z))
            throw std::runtime_error(context + " accessor contains a non-finite delta");
    }
    return values;
}

// The single place where the mesh path depends on PELICAN_WITH_VAT.
//
// With VAT compiled in, `parseVatPrimitive` decodes the pelican.vat extras.
// With VAT compiled out there is no such thing as a VAT primitive: a GLB that
// carries the extras is rejected here, and every caller below then sees an
// empty optional, so the VAT branches stay present and unreachable instead of
// disappearing. That is what keeps the `else if` chain, the occurrence
// identity, and the material selection one shape in both configurations.
#if PELICAN_WITH_VAT

std::vector<VatBufferViewInfo> vatBufferViewInfos(const tinygltf::Model &model) {
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

std::optional<VatPrimitiveInfo> parseVatPrimitive(const tinygltf::Model &model,
                                                  const VatPrimitiveMeta &vat_meta,
                                                  uint32_t vertex_count) {
    return parseVatPrimitiveExtras(vat_meta, vertex_count,
                                   vatBufferViewInfos(model));
}

#else

std::optional<VatPrimitiveInfo> parseVatPrimitive(const tinygltf::Model &,
                                                  const VatPrimitiveMeta &vat_meta,
                                                  uint32_t) {
    if (vat_meta.present) {
        throwBuildFeatureDisabled("PELICAN_WITH_VAT",
                                  "GLB contains pelican.vat primitive extras");
    }
    return std::nullopt;
}

#endif

// The importer's cross-method material state, named at the one place the
// per-primitive path writes it. Generated (skinned / VAT) material ids are
// handed out from `next_generated_material` downwards, and every one of them
// must land in both `resolved_materials` and `generated_material_sources`;
// `finalizeOccurrenceIdentities` reads the pair back.
struct GeneratedMaterialRegistry {
    using ModelLocalMaterialId = int;

    GltfResourceSink &resources;
    StandardMaterialResource &std_mat;
    const tinygltf::Model &model;
    const std::vector<MaterialInfo> &material_infos;
    ModelLocalMaterialId &next_generated_material;
    std::unordered_map<ModelLocalMaterialId, GlobalMaterialId> &resolved_materials;
    std::unordered_map<ModelLocalMaterialId, std::uint32_t> &generated_material_sources;
    std::unordered_map<ModelLocalMaterialId, ModelLocalMaterialId> &skinned_material_variants;

    MaterialInfo materialInfoForPrimitive(int local_material_id) const {
        if (local_material_id >= 0) {
            return material_infos.at(local_material_id);
        }

        return std_mat.builtinMaterialInfo("default", std_mat.whiteTexture());
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
        return resources.registerTexture(vk::Extent3D{vertex_count, frame_count, 1}, data,
                                         vk::Format::eR16G16B16A16Sfloat, required_bytes);
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
        resolved_materials[generated_material] = resources.registerMaterial(material_info);
        generated_material_sources[generated_material] =
            local_material_id >= 0 ? static_cast<std::uint32_t>(local_material_id)
                                   : noSourceMaterialIndex;
        return generated_material;
    }

    ModelLocalMaterialId skinnedMaterial(int local_material_id) {
        if (const auto found = skinned_material_variants.find(local_material_id);
            found != skinned_material_variants.end()) return found->second;
        auto info = local_material_id >= 0
                        ? material_infos.at(local_material_id)
                        : std_mat.builtinMaterialInfo("skinned_fallback",
                                                      std_mat.whiteTexture());
        info.vert_shader = std_mat.skinnedVertShader();
        info.skinned = true;
        const auto generated = next_generated_material--;
        resolved_materials[generated] = resources.registerMaterial(std::move(info));
        generated_material_sources[generated] =
            local_material_id >= 0 ? static_cast<std::uint32_t>(local_material_id)
                                   : noSourceMaterialIndex;
        skinned_material_variants[local_material_id] = generated;
        return generated;
    }
};

// Read-only, and fixed for the whole mesh.
struct PrimitiveSourceContext {
    const tinygltf::Model &model;
    const std::string &source_path;
    const tinygltf::Mesh &mesh;
    int mesh_index;
    int node_index;
    const glm::mat4 &world_transform;
    const std::vector<std::string> &node_name_path;
    const std::vector<std::uint32_t> &parent_index_path;
    const std::optional<CanonicalModelAssetKey> &asset_key;
};

// The mesh-level skin decision. `joint_offset` is the palette base chosen by
// `selectSkin`, which is why `skin_joint_offsets` stays on the importer: the
// choice is made once per mesh, before any primitive is read.
struct PrimitiveSkinContext {
    bool skinned;
    int skin_index;
    std::uint32_t joint_offset;
};

// The mesh-level morph layout, already appended to the model default weights.
struct PrimitiveMorphContext {
    std::size_t target_count;
    std::uint32_t weight_offset;
};

// VRM first-person state: the semantic document plus the importer's lazily
// built head-subtree mask, which this path may be the one to fill.
struct PrimitiveVrmContext {
    const std::shared_ptr<const VrmSemanticData> &vrm_semantic;
    std::optional<std::vector<std::uint8_t>> &head_related_nodes;
};

// What one glTF primitive contributes, in publication order. The importer
// appends `material_primitives` to `tmp_material_primitives` and
// `morph_layouts` to `morph_target_layout->primitives`; neither is read back
// while the next primitive is built, so collecting them here and appending
// after the primitive keeps both orders.
struct PrimitiveBuildResult {
    std::vector<std::pair<GeneratedMaterialRegistry::ModelLocalMaterialId,
                          ModelTemplate::PrimitiveRefInfo>>
        material_primitives;
    std::vector<MorphPrimitiveLayout> morph_layouts;
};

// Decodes one primitive, splits it into view variants, hands the variants to
// the resource sink, and names the material each one draws with. `collision`,
// when non-null, is the collision mesh of the owning node and is appended to.
PrimitiveBuildResult buildPrimitive(const tinygltf::Primitive &primitive,
                                    std::size_t primitive_index,
                                    const PrimitiveSourceContext &source,
                                    const PrimitiveSkinContext &skin,
                                    const PrimitiveMorphContext &morph,
                                    const PrimitiveVrmContext &vrm,
                                    GeneratedMaterialRegistry &materials,
                                    ModelCollisionMesh *collision) {
    const auto &model = source.model;
    const auto &mesh = source.mesh;
    GltfAccessorReader reader{model, source.source_path};
    PrimitiveBuildResult result;
    CommonPolygonVertData dat;

    if (primitive.indices >= 0)
        dat.indices = reader.getDataFromAccessor<TINYGLTF_TYPE_SCALAR, uint32_t>(primitive.indices);
    if (auto it = primitive.attributes.find("POSITION"); it != primitive.attributes.end())
        dat.pos = reader.getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(it->second);
    if (auto it = primitive.attributes.find("NORMAL"); it != primitive.attributes.end())
        dat.normal = reader.getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(it->second);
    if (auto it = primitive.attributes.find("TANGENT"); it != primitive.attributes.end())
        dat.tangent = reader.getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::vec4>(it->second);
    if (auto it = primitive.attributes.find("TEXCOORD_0"); it != primitive.attributes.end())
        dat.texcoord = reader.getDataFromAccessor<TINYGLTF_TYPE_VEC2, glm::vec2>(it->second);
    if (auto it = primitive.attributes.find("COLOR_0"); it != primitive.attributes.end()) {
        if (it->second < 0 ||
            it->second >=
                static_cast<int>(
                    model.accessors.size())) {
            throw std::runtime_error(
                "glTF COLOR_0 accessor index is out of range");
        }
        const auto &accessor =
            model.accessors.at(
                static_cast<std::size_t>(
                    it->second));
        if (accessor.type ==
            TINYGLTF_TYPE_VEC3) {
            const auto &tmp_color =
                reader.getDataFromAccessor<
                    TINYGLTF_TYPE_VEC3,
                    glm::vec3>(it->second);
            dat.color.resize(tmp_color.size());
            std::transform(
                tmp_color.begin(), tmp_color.end(),
                dat.color.begin(),
                [](glm::vec3 value) {
                    return glm::vec4{value, 1.0F};
                });
        } else if (
            accessor.type ==
            TINYGLTF_TYPE_VEC4) {
            dat.color =
                reader.getDataFromAccessor<
                    TINYGLTF_TYPE_VEC4,
                    glm::vec4>(it->second);
        } else {
            throw std::runtime_error(
                "glTF COLOR_0 accessor must be VEC3 or VEC4");
        }
    }
    if (auto it = primitive.attributes.find("JOINTS_0"); it != primitive.attributes.end())
        dat.joint = reader.getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::i16vec4>(it->second);
    if (auto it = primitive.attributes.find("WEIGHTS_0"); it != primitive.attributes.end())
        dat.weight = reader.getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::vec4>(it->second);

    dat.morph_weight_offset = morph.weight_offset;
    dat.morph_targets.reserve(morph.target_count);
    for (std::size_t target_index = 0;
         target_index < morph.target_count; ++target_index) {
        const auto &target = primitive.targets[target_index];
        MorphTargetVertexData decoded;
        const auto context =
            "glTF mesh '" +
            (mesh.name.empty() ? std::to_string(source.mesh_index) : mesh.name) +
            "' primitive " + std::to_string(primitive_index) + " target " +
            std::to_string(target_index);
        for (const auto &[semantic, accessor] : target) {
            if (semantic == "POSITION") {
                decoded.position =
                    readMorphDeltaAccessor(model, source.source_path, accessor,
                                           dat.pos.size(), context + " POSITION");
                decoded.presence_mask |= morphPositionPresent;
            } else if (semantic == "NORMAL") {
                decoded.normal =
                    readMorphDeltaAccessor(model, source.source_path, accessor,
                                           dat.pos.size(), context + " NORMAL");
                decoded.presence_mask |= morphNormalPresent;
            } else if (semantic == "TANGENT") {
                decoded.tangent =
                    readMorphDeltaAccessor(model, source.source_path, accessor,
                                           dat.pos.size(), context + " TANGENT");
                decoded.presence_mask |= morphTangentPresent;
            } else {
                throw std::runtime_error(context +
                                         " has unsupported attribute '" +
                                         semantic + "'");
            }
        }
        if (decoded.presence_mask == 0)
            throw std::runtime_error(context + " contains no supported delta attribute");
        dat.morph_targets.push_back(std::move(decoded));
    }

    const auto vat_meta = tinyGltfValueToVatMeta(primitive.extras);
    const auto primitive_mode =
        primitive.mode < 0 ? TINYGLTF_MODE_TRIANGLES : primitive.mode;
    const auto source_primitive_digest = primitiveContentDigestV1(
        static_cast<std::uint8_t>(primitive_mode), dat);
    const auto primitive_explicit_id =
        tinyGltfExtrasId(primitive.extras);
    const auto vat_info = parseVatPrimitive(
        model, vat_meta, static_cast<uint32_t>(dat.pos.size()));
    if (vat_info && primitive_mode != TINYGLTF_MODE_TRIANGLES) {
        throw std::runtime_error("pelican.vat only supports TRIANGLES topology");
    }
    if (vat_info && !dat.morph_targets.empty()) {
        throw std::runtime_error("glTF morph targets and pelican.vat cannot share one primitive");
    }

    const auto annotation =
        firstPersonAnnotation(vrm.vrm_semantic, source.node_index);
    std::optional<VrmAutoTriangleSplit> auto_split;
    if (annotation == FirstPersonAnnotation::automatic && skin.skin_index >= 0) {
        if (primitive_mode != TINYGLTF_MODE_TRIANGLES)
            throw std::runtime_error(
                "VRM firstPerson auto requires TRIANGLES topology");
        auto_split = splitVrmAutoTriangles(VrmAutoTriangleSplitInput{
            .indices = dat.indices,
            .vertex_count = static_cast<std::uint32_t>(dat.pos.size()),
            .joints = dat.joint,
            .weights = dat.weight,
            .skin_joint_nodes = model.skins.at(skin.skin_index).joints,
            .head_related_nodes = headRelatedNodes(model, vrm.vrm_semantic,
                                                   vrm.head_related_nodes),
        });
    }

    if (skin.skinned && (dat.joint.empty() || dat.weight.empty())) {
        throw std::runtime_error("glTF skinned primitive requires JOINTS_0 and WEIGHTS_0");
    }
    if (skin.skinned) {
        const auto joint_count = model.skins.at(skin.skin_index).joints.size();
        for (auto &joints : dat.joint) {
            for (int component = 0; component < 4; ++component) {
                if (joints[component] < 0 || static_cast<std::size_t>(joints[component]) >= joint_count) {
                    throw std::runtime_error("glTF JOINTS_0 index exceeds its skin joint array");
                }
                joints[component] = static_cast<std::int16_t>(joints[component] + skin.joint_offset);
            }
        }
    }
    if (!skin.skinned) transformVertexData(dat, source.world_transform);
    if (collision) {
        if ((primitive.mode != -1 && primitive.mode != TINYGLTF_MODE_TRIANGLES) ||
            !primitive.targets.empty() || tinyGltfValueToVatMeta(primitive.extras).present)
            throw std::runtime_error("Static collision meshes require undeformed TRIANGLES primitives");
        if (dat.pos.empty() || dat.pos.size() > std::numeric_limits<std::uint32_t>::max() - collision->positions.size())
            throw std::runtime_error("Collision mesh has no positions or too many vertices");
        const auto count = dat.indices.empty() ? dat.pos.size() : dat.indices.size();
        if (count % 3 != 0)
            throw std::runtime_error("Collision mesh triangle index count must be divisible by three");
        const auto base = static_cast<std::uint32_t>(collision->positions.size());
        for (const auto &position : dat.pos) {
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
                throw std::runtime_error("Collision mesh positions must be finite");
            collision->positions.push_back(position);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = dat.indices.empty() ? static_cast<std::uint32_t>(i) : dat.indices[i];
            if (index >= dat.pos.size()) throw std::runtime_error("Collision mesh index is out of range");
            collision->indices.push_back(base + index);
        }
    }
    std::vector<std::uint32_t> morph_presence;
    morph_presence.reserve(dat.morph_targets.size());
    for (const auto &target : dat.morph_targets)
        morph_presence.push_back(target.presence_mask);

    struct PrimitiveVariant {
        CommonPolygonVertData data;
        PrimitiveViewVisibility visibility =
            PrimitiveViewVisibility::both;
    };
    std::vector<PrimitiveVariant> variants;
    if (auto_split &&
        !auto_split->third_person_only_indices.empty()) {
        if (!auto_split->both_indices.empty()) {
            auto body = dat;
            body.indices = std::move(auto_split->both_indices);
            variants.push_back(
                {std::move(body), PrimitiveViewVisibility::both});
        }
        dat.indices =
            std::move(auto_split->third_person_only_indices);
        variants.push_back({std::move(dat),
                            PrimitiveViewVisibility::third_person_only});
    } else {
        auto visibility = PrimitiveViewVisibility::both;
        if (annotation == FirstPersonAnnotation::first_person_only)
            visibility = PrimitiveViewVisibility::first_person_only;
        else if (annotation ==
                 FirstPersonAnnotation::third_person_only)
            visibility = PrimitiveViewVisibility::third_person_only;
        variants.push_back({std::move(dat), visibility});
    }

    for (auto &variant : variants) {
        auto added = materials.resources.addPrimitive(std::move(variant.data),
                                                      skin.skinned);
        auto primitive_info = added.primitive;
        primitive_info.mesh_index = static_cast<std::uint32_t>(source.mesh_index);
        primitive_info.primitive_index =
            static_cast<std::uint32_t>(primitive_index);
        primitive_info.node_index =
            source.node_index < 0
                ? noSourceNodeIndex
                : static_cast<std::uint32_t>(source.node_index);
        primitive_info.view_visibility = variant.visibility;
        ImporterVariant importer_variant{};
        if (auto_split) {
            importer_variant.kind =
                variant.visibility == PrimitiveViewVisibility::both
                    ? ImporterVariantV1::vrm_split_both
                    : ImporterVariantV1::vrm_split_third_person_only;
        } else if (vat_info) {
            importer_variant.kind = ImporterVariantV1::vat_deformed;
        } else if (!primitive.targets.empty()) {
            importer_variant.kind = ImporterVariantV1::morph_expanded;
        } else if (skin.skinned) {
            importer_variant.kind = ImporterVariantV1::skinned;
        }
        primitive_info.occurrence =
            std::make_shared<ModelOccurrenceIdentityInfo>(
                ModelOccurrenceIdentityInfo{
                    .asset = source.asset_key,
                    .node_name_path = source.node_name_path,
                    .diagnostic_parent_path = source.parent_index_path,
                    .node_explicit_id =
                        source.node_index < 0
                            ? std::optional<std::string>{}
                            : tinyGltfExtrasId(
                                  model.nodes.at(source.node_index).extras),
                    .primitive_explicit_id = primitive_explicit_id,
                    .primitive_content_digest =
                        source_primitive_digest,
                    .variant = importer_variant,
                });
        // `vat_info` is empty in every build without VAT, so this leaves the
        // member at its false default there and the ray-query classification
        // is unchanged.
        primitive_info.vat_deformed = vat_info.has_value();
        if (vat_info) {
            primitive_info.bounds_source =
                std::make_shared<const ModelPrimitiveBoundsSource>(
                    ModelPrimitiveBoundsSource{
                        .base = ModelPrimitiveBounds{
                            vat_info->bounds_min,
                            vat_info->bounds_max,
                        },
                    });
        }
        if (!added.morph_ranges.empty()) {
            for (std::size_t target = 0;
                 target < added.morph_ranges.size(); ++target)
                added.morph_ranges[target].presence_mask =
                    morph_presence[target];
            result.morph_layouts.push_back(
                MorphPrimitiveLayout{
                    .node_index =
                        source.node_index < 0
                            ? noMorphNode
                            : static_cast<std::uint32_t>(source.node_index),
                    .mesh_index = static_cast<std::uint32_t>(source.mesh_index),
                    .primitive_index =
                        static_cast<std::uint32_t>(primitive_index),
                    .weight_offset = morph.weight_offset,
                    .vertex_offset = static_cast<std::uint32_t>(
                        primitive_info.vert_offset),
                    .skinned = skin.skinned,
                    .delta_ranges = std::move(added.morph_ranges),
                });
        }
        if (skin.skinned && vat_info) {
            throw std::runtime_error("glTF skeletal skinning and pelican.vat cannot share one primitive");
        }
        const auto material_id =
            vat_info ? materials.registerVatMaterial(primitive.material,
                                                     *vat_info, primitive_info)
                     : (skin.skinned
                            ? materials.skinnedMaterial(primitive.material)
                            : primitive.material);
        result.material_primitives.emplace_back(material_id,
                                                std::move(primitive_info));
    }
    return result;
}

} // namespace

struct InternalGltfLoader {
    using ModelLocalMaterialId = int;

    GltfResourceSink &resources;
    StandardMaterialResource &std_mat;
    tinygltf::Model &model;
    std::string source_path;
    std::optional<std::string> authored_reference;
    std::optional<CanonicalModelAssetKey> asset_key;
    std::optional<AssetFragmentRef> fragment;
    bool scene_node_instance = false;
    std::shared_ptr<const VrmSemanticData> vrm_semantic;
    bool compile_surface_shaders = false;
    std::vector<std::string>
        material_shader_defines;
    MaterialSurfaceCatalog gltf_surfaces;
    std::vector<std::optional<GlobalMaterialId>> material_map;
    std::vector<MaterialInfo> material_infos;
    std::vector<std::optional<MaterialVariantRouting>>
        material_routings;
    std::vector<std::optional<GlobalTextureId>> texture_map;
    std::unordered_map<ModelLocalMaterialId, GlobalMaterialId> resolved_materials;
    std::unordered_map<ModelLocalMaterialId, std::uint32_t> generated_material_sources;
    std::unordered_map<ModelLocalMaterialId, ModelLocalMaterialId> skinned_material_variants;
    std::unordered_map<ModelLocalMaterialId, std::vector<ModelTemplate::PrimitiveRefInfo>> tmp_material_primitives;
    std::vector<ModelCollisionMesh> collision_meshes;
    ModelLocalMaterialId next_generated_material = -2;
    std::unordered_map<int, std::uint32_t> skin_joint_offsets;
    std::shared_ptr<SkeletalModelData> skeletal_data;
    std::shared_ptr<MorphTargetLayout> morph_target_layout =
        std::make_shared<MorphTargetLayout>();
    std::optional<std::vector<std::uint8_t>> head_related_nodes;

    struct NodeOccurrence {
        int node_index = -1;
        std::string full_path;
        std::vector<std::string> name_path;
        std::vector<std::uint32_t> parent_index_path;
        glm::mat4 parent_transform{1.0f};
    };

    struct FragmentCandidate {
        int object_index = -1;
        int node_index = -1;
        std::string full_path;
        std::vector<std::string> aliases;
        std::vector<std::string> name_path;
        std::vector<std::uint32_t> parent_index_path;
        glm::mat4 parent_transform{1.0f};
    };

    struct RootSelection {
        int node_index = -1;
        int mesh_index = -1;
        glm::mat4 parent_transform{1.0f};
        bool subtree = true;
        bool apply_node_transform = true;
        std::vector<std::string> name_path;
        std::vector<std::uint32_t> parent_index_path;
    };

    struct LoadSelection {
        bool whole_model = false;
        std::vector<RootSelection> roots;
        std::optional<int> material_only;
        std::optional<int> animation_only;
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
        return resources.registerTexture(vk::Extent3D{4, 4, 1}, data.data(),
                                         vk::Format::eR8G8B8A8Unorm, data.size());
    }

    GlobalTextureId metallicRoughnessTextureForMaterial(const tinygltf::Material &material) {
        const auto texture_index = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if (texture_index >= 0) {
            return texture_map.at(texture_index).value();
        }

        return registerSolidTexture(
            255, 255, 255);
    }

    GlobalTextureId occlusionTextureForMaterial(
        const tinygltf::Material &material) const {
        const auto texture_index = material.occlusionTexture.index;
        return texture_index >= 0
                   ? texture_map.at(texture_index).value()
                   : std_mat.occlusionDefaultTexture();
    }

    GlobalTextureId emissiveTextureForMaterial(const tinygltf::Material &material) {
        const auto texture_index = material.emissiveTexture.index;
        if (texture_index >= 0) {
            return texture_map.at(texture_index).value();
        }

        return std_mat.whiteTexture();
    }

    const SurfaceFormatDocument &gltfSurface(
        std::string_view reference) {
        const auto existing =
            gltf_surfaces.find(
                std::string{reference});
        if (existing != gltf_surfaces.end()) {
            return existing->second;
        }
        constexpr std::string_view engine_prefix =
            "engine://";
        if (!reference.starts_with(
                engine_prefix)) {
            throw std::runtime_error(
                "glTF lowering selected non-engine surface '" +
                std::string{reference} + "'");
        }
        const auto source =
            engineResourceOrThrow(
                reference.substr(
                    engine_prefix.size()));
        auto [inserted, _] =
            gltf_surfaces.emplace(
                std::string{reference},
                parseSurfaceFormat(
                    source, reference));
        return inserted->second;
    }

    template <int expected_type, class T> std::vector<T> getDataFromAccessor(int accessor_index) {
        return GltfAccessorReader{model, source_path}.getDataFromAccessor<expected_type, T>(accessor_index);
    }

    std::size_t meshMorphTargetCount(int mesh_index) const {
        const auto &mesh = model.meshes.at(mesh_index);
        std::optional<std::size_t> count;
        for (std::size_t primitive_index = 0;
             primitive_index < mesh.primitives.size(); ++primitive_index) {
            const auto current = mesh.primitives[primitive_index].targets.size();
            if (!count) count = current;
            if (*count != current)
                throw std::runtime_error(
                    "glTF mesh '" +
                    (mesh.name.empty() ? std::to_string(mesh_index) : mesh.name) +
                    "' has inconsistent morph target counts across primitives");
        }
        const auto result = count.value_or(0);
        if (result > maxMorphTargetsPerPrimitive)
            throw std::runtime_error(
                "glTF mesh '" +
                (mesh.name.empty() ? std::to_string(mesh_index) : mesh.name) +
                "' morph target count exceeds renderer limit of " +
                std::to_string(maxMorphTargetsPerPrimitive));
        if (!mesh.weights.empty() && mesh.weights.size() != result)
            throw std::runtime_error(
                "glTF mesh '" +
                (mesh.name.empty() ? std::to_string(mesh_index) : mesh.name) +
                "' weights count does not match morph target count");
        return result;
    }

    std::uint32_t appendMorphDefaults(int mesh_index, int node_index,
                                      std::size_t target_count) {
        if (target_count == 0) return 0;
        if (morph_target_layout->default_weights.size() >
                maxMorphWeightsPerInstance ||
            target_count > maxMorphWeightsPerInstance -
                               morph_target_layout->default_weights.size())
            throw std::runtime_error(
                "glTF model morph layout exceeds per-instance weight capacity of " +
                std::to_string(maxMorphWeightsPerInstance));
        const auto &mesh = model.meshes.at(mesh_index);
        const std::vector<double> *source = &mesh.weights;
        if (node_index >= 0 && !model.nodes.at(node_index).weights.empty()) {
            source = &model.nodes.at(node_index).weights;
            if (source->size() != target_count)
                throw std::runtime_error(
                    "glTF node '" +
                    (model.nodes.at(node_index).name.empty()
                         ? std::to_string(node_index)
                         : model.nodes.at(node_index).name) +
                    "' weights count does not match mesh morph target count");
        }
        const auto offset =
            static_cast<std::uint32_t>(morph_target_layout->default_weights.size());
        for (std::size_t target = 0; target < target_count; ++target) {
            const auto value = source->empty() ? 0.0 : source->at(target);
            if (!std::isfinite(value))
                throw std::runtime_error("glTF morph default weight is non-finite at mesh " +
                                         std::to_string(mesh_index) + " target " +
                                         std::to_string(target));
            const auto converted = static_cast<float>(value);
            if (!std::isfinite(converted))
                throw std::runtime_error("glTF morph default weight exceeds FLOAT range at mesh " +
                                         std::to_string(mesh_index) + " target " +
                                         std::to_string(target));
            morph_target_layout->default_weights.push_back(converted);
        }
        return offset;
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

    SkeletonNodeRestPose nodeRestPose(const tinygltf::Node &node) {
        SkeletonNodeRestPose pose;
        pose.name = node.name;
        if (node.matrix.size() == 16) {
            const auto matrix = nodeTransform(node);
            pose.translation = glm::vec3{matrix[3]};
            pose.scale = {glm::length(glm::vec3{matrix[0]}), glm::length(glm::vec3{matrix[1]}),
                          glm::length(glm::vec3{matrix[2]})};
            glm::mat3 rotation{matrix};
            for (int column = 0; column < 3; ++column) {
                if (pose.scale[column] != 0.0f) rotation[column] /= pose.scale[column];
            }
            pose.rotation = glm::normalize(glm::quat_cast(rotation));
            return pose;
        }
        if (node.translation.size() == 3) {
            pose.translation = {static_cast<float>(node.translation[0]), static_cast<float>(node.translation[1]),
                                static_cast<float>(node.translation[2])};
        }
        if (node.rotation.size() == 4) {
            pose.rotation = glm::normalize(glm::quat{static_cast<float>(node.rotation[3]),
                                                     static_cast<float>(node.rotation[0]),
                                                     static_cast<float>(node.rotation[1]),
                                                     static_cast<float>(node.rotation[2])});
        }
        if (node.scale.size() == 3) {
            pose.scale = {static_cast<float>(node.scale[0]), static_cast<float>(node.scale[1]),
                          static_cast<float>(node.scale[2])};
        }
        return pose;
    }

    SkeletalAnimationClip loadAnimationClip(int animation_index) {
        const auto &animation = model.animations.at(animation_index);
        const auto animation_name = animation.name.empty()
                                        ? std::string{"<animation "} + std::to_string(animation_index) + ">"
                                        : animation.name;
        SkeletalAnimationClip result;
        result.name = animation.name;
        bool has_keys = false;
        for (const auto &channel : animation.channels) {
            if (channel.sampler < 0 || channel.sampler >= static_cast<int>(animation.samplers.size())) {
                throw std::runtime_error("glTF animation '" + animation_name + "' has an invalid sampler");
            }
            const auto &sampler = animation.samplers[channel.sampler];
            if (sampler.interpolation == "CUBICSPLINE") {
                throw std::runtime_error("glTF animation '" + animation_name +
                                         "' uses unsupported CUBICSPLINE interpolation (v1 supports LINEAR/STEP)");
            }
            if (!sampler.interpolation.empty() && sampler.interpolation != "LINEAR" &&
                sampler.interpolation != "STEP") {
                throw std::runtime_error("glTF animation '" + animation_name +
                                         "' uses unsupported interpolation '" + sampler.interpolation + "'");
            }
            SkeletalAnimationChannel loaded;
            loaded.node = channel.target_node;
            loaded.interpolation = sampler.interpolation == "STEP" ? AnimationInterpolation::step
                                                                     : AnimationInterpolation::linear;
            loaded.times = getDataFromAccessor<TINYGLTF_TYPE_SCALAR, float>(sampler.input);
            if (channel.target_path == "translation") {
                loaded.path = AnimationPath::translation;
                const auto values = getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(sampler.output);
                loaded.values.reserve(values.size());
                for (const auto value : values) loaded.values.emplace_back(value, 0.0f);
            } else if (channel.target_path == "scale") {
                loaded.path = AnimationPath::scale;
                const auto values = getDataFromAccessor<TINYGLTF_TYPE_VEC3, glm::vec3>(sampler.output);
                loaded.values.reserve(values.size());
                for (const auto value : values) loaded.values.emplace_back(value, 0.0f);
            } else if (channel.target_path == "rotation") {
                loaded.path = AnimationPath::rotation;
                loaded.values = getDataFromAccessor<TINYGLTF_TYPE_VEC4, glm::vec4>(sampler.output);
            } else {
                throw std::runtime_error("glTF animation '" + animation_name + "' targets unsupported path '" +
                                         channel.target_path + "' (morph targets are outside clip v1)");
            }
            if (loaded.times.empty() || loaded.times.size() != loaded.values.size()) {
                throw std::runtime_error("glTF animation '" + animation_name + "' has mismatched keyframe counts");
            }
            const auto [minimum, maximum] = std::minmax_element(loaded.times.begin(), loaded.times.end());
            if (!has_keys) {
                result.start = *minimum;
                result.end = *maximum;
                has_keys = true;
            } else {
                result.start = std::min(result.start, *minimum);
                result.end = std::max(result.end, *maximum);
            }
            result.channels.push_back(std::move(loaded));
        }
        return result;
    }

    bool skinFitsPalette(int skin_index) const {
        if (skin_index < 0 || skin_index >= static_cast<int>(model.skins.size())) return false;
        if (skin_joint_offsets.contains(skin_index)) return true;
        const auto used = skeletal_data ? skeletal_data->joint_nodes.size() : std::size_t{0};
        return used + model.skins[skin_index].joints.size() <= maxSkinJoints;
    }

    std::uint32_t selectSkin(int skin_index, std::optional<int> animation_only = std::nullopt) {
        if (skin_index < 0 || skin_index >= static_cast<int>(model.skins.size())) {
            throw std::runtime_error("glTF mesh node references an invalid skin");
        }
        if (const auto found = skin_joint_offsets.find(skin_index); found != skin_joint_offsets.end()) {
            return found->second;
        }
        if (!skeletal_data) {
            skeletal_data = std::make_shared<SkeletalModelData>();
            skeletal_data->source_path = source_path;
            skeletal_data->nodes.reserve(model.nodes.size());
            for (const auto &node : model.nodes) skeletal_data->nodes.push_back(nodeRestPose(node));
            for (int parent = 0; parent < static_cast<int>(model.nodes.size()); ++parent) {
                for (const auto child : model.nodes[parent].children) {
                    if (child >= 0 && child < static_cast<int>(skeletal_data->nodes.size())) {
                        skeletal_data->nodes[child].parent = parent;
                    }
                }
            }
            for (int i = 0; i < static_cast<int>(model.animations.size()); ++i) {
                if (!animation_only || *animation_only == i) skeletal_data->clips.push_back(loadAnimationClip(i));
            }
        }
        const auto &skin = model.skins[skin_index];
        const auto offset = static_cast<std::uint32_t>(skeletal_data->joint_nodes.size());
        std::vector<glm::mat4> inverse_bind_matrices;
        if (skin.inverseBindMatrices >= 0) {
            inverse_bind_matrices =
                getDataFromAccessor<TINYGLTF_TYPE_MAT4, glm::mat4>(skin.inverseBindMatrices);
        } else {
            inverse_bind_matrices.assign(skin.joints.size(), glm::mat4{1.0f});
        }
        if (skin.joints.size() != inverse_bind_matrices.size()) {
            throw std::runtime_error("glTF skin joint/inverseBindMatrices count mismatch");
        }
        if (offset + skin.joints.size() > maxSkinJoints) {
            throw std::runtime_error("glTF skins exceed the v1 combined joint palette limit of 128");
        }
        skeletal_data->joint_nodes.insert(skeletal_data->joint_nodes.end(), skin.joints.begin(), skin.joints.end());
        skeletal_data->inverse_bind_matrices.insert(skeletal_data->inverse_bind_matrices.end(),
                                                    inverse_bind_matrices.begin(), inverse_bind_matrices.end());
        skeletal_data->skin_bindings.push_back({skin.name, offset, static_cast<std::uint32_t>(skin.joints.size())});
        skin_joint_offsets.emplace(skin_index, offset);
        return offset;
    }

    std::string fragmentReference() const {
        if (!fragment) {
            return source_path;
        }
        return source_path + "#" + fragment->kind + "/" + fragment->path;
    }

    void collectNodeOccurrence(int node_index, const glm::mat4 &parent_transform,
                               const std::string &parent_path,
                               const std::vector<std::string> &parent_name_path,
                               const std::vector<std::uint32_t> &parent_index_path,
                               std::vector<NodeOccurrence> &occurrences,
                               std::set<std::pair<int, std::string>> &seen) {
        if (node_index < 0 || node_index >= static_cast<int>(model.nodes.size())) {
            return;
        }
        const auto &node = model.nodes[node_index];
        const auto full_path = parent_path.empty()
                                   ? node.name
                                   : node.name.empty() ? parent_path : parent_path + "/" + node.name;
        auto name_path = parent_name_path;
        name_path.push_back(node.name);
        if (seen.emplace(node_index, full_path).second) {
            occurrences.push_back(NodeOccurrence{node_index, full_path,
                                                  name_path, parent_index_path,
                                                  parent_transform});
        }
        const auto world_transform = parent_transform * nodeTransform(node);
        auto child_index_path = parent_index_path;
        child_index_path.push_back(static_cast<std::uint32_t>(node_index));
        for (const auto child : node.children) {
            collectNodeOccurrence(child, world_transform, full_path, name_path,
                                  child_index_path, occurrences, seen);
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
                collectNodeOccurrence(root, glm::mat4{1.0f}, {}, {}, {},
                                      occurrences, seen);
            }
        }
        for (int i = 0; i < static_cast<int>(model.nodes.size()); ++i) {
            if (!is_child[i]) {
                collectNodeOccurrence(i, glm::mat4{1.0f}, {}, {}, {},
                                      occurrences, seen);
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
                    occurrence.name_path,
                    occurrence.parent_index_path,
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
                    occurrence.name_path,
                    occurrence.parent_index_path,
                    occurrence.parent_transform,
                });
            }
            for (int i = 0; i < static_cast<int>(model.meshes.size()); ++i) {
                if (!referenced[i] && !model.meshes[i].name.empty()) {
                    candidates.push_back(FragmentCandidate{
                        i, -1, model.meshes[i].name, {model.meshes[i].name},
                        {}, {}, glm::mat4{1.0f}});
                }
            }
            return candidates;
        }

        const auto append_named = [&](const auto &objects) {
            for (int i = 0; i < static_cast<int>(objects.size()); ++i) {
                if (!objects[i].name.empty()) {
                    candidates.push_back(FragmentCandidate{
                        i, -1, objects[i].name, {objects[i].name}, {}, {},
                        glm::mat4{1.0f}});
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
            if (model.scenes.empty()) {
                std::vector<bool> is_child(model.nodes.size(), false);
                for (const auto &node : model.nodes) {
                    for (const auto child : node.children) {
                        if (child >= 0 &&
                            child < static_cast<int>(is_child.size())) {
                            is_child[static_cast<std::size_t>(child)] = true;
                        }
                    }
                }
                for (int node = 0; node < static_cast<int>(model.nodes.size());
                     ++node) {
                    if (!is_child[static_cast<std::size_t>(node)]) {
                        selection.roots.push_back(
                            RootSelection{node, -1, glm::mat4{1.0f}, true, true,
                                          {model.nodes[node].name}, {}});
                    }
                }
                return selection;
            }
            const auto scene_index = model.defaultScene < 0 ? 0 : model.defaultScene;
            if (scene_index >= static_cast<int>(model.scenes.size())) {
                throw std::runtime_error("glTF default scene index is out of range in '" +
                                         source_path + "'");
            }
            for (const auto node : model.scenes.at(
                     static_cast<std::size_t>(scene_index)).nodes) {
                selection.roots.push_back(
                    RootSelection{node, -1, glm::mat4{1.0f}, true, true,
                                  {model.nodes[node].name}, {}});
            }
            return selection;
        }

        const auto selected = resolveFragmentCandidate(*fragment);
        LoadSelection selection;
        if (fragment->kind == "node") {
            selection.roots.push_back(scene_node_instance
                                          ? RootSelection{selected.node_index, -1, glm::mat4{1.0f}, false, false,
                                                          selected.name_path, selected.parent_index_path}
                                          : RootSelection{selected.node_index, -1, selected.parent_transform, true,
                                                          true, selected.name_path,
                                                          selected.parent_index_path});
        } else if (fragment->kind == "mesh") {
            selection.roots.push_back(
                RootSelection{selected.node_index, selected.object_index,
                              selected.parent_transform, false, true,
                              selected.name_path, selected.parent_index_path});
        } else if (fragment->kind == "material") {
            selection.material_only = selected.object_index;
        } else if (fragment->kind == "animation") {
            selection.animation_only = selected.object_index;
        }
        // An animation fragment carries CPU clip/skin data only and deliberately creates no
        // unrelated mesh, material, texture, or ECS bone resources.
        return selection;
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
        if (texture_index < 0 ||
            texture_index >=
                static_cast<int>(model.textures.size())) {
            throw std::runtime_error(
                "glTF material references an invalid texture index in '" +
                source_path + "'");
        }
        const auto &texture =
            model.textures[static_cast<std::size_t>(texture_index)];
        if (texture.source < 0 ||
            texture.source >=
                static_cast<int>(model.images.size())) {
            throw std::runtime_error(
                "glTF texture " + std::to_string(texture_index) +
                " has a missing, unsupported, or out-of-range image source in '" +
                source_path + "'");
        }
        const auto &image =
            model.images[static_cast<std::size_t>(texture.source)];
        GltfInternal::validateRgba8Image(
            image, source_path + "#image/" +
                       std::to_string(texture.source));
        texture_map.at(texture_index) = resources.registerTexture(
            vk::Extent3D{
                static_cast<uint32_t>(image.width),
                static_cast<uint32_t>(image.height),
                1,
            },
            image.image.data(), vk::Format::eR8G8B8A8Unorm,
            image.image.size());
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
        const auto occlusion_texture = occlusionTextureForMaterial(material);
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

        const auto effective_name =
            material.name.empty()
                ? source_path + "#material/" +
                      std::to_string(material_index)
                : material.name;
        MaterialBase base;
        base.base_color_factor = {
            vectorValueOr(base_factor, 0, 1.0),
            vectorValueOr(base_factor, 1, 1.0),
            vectorValueOr(base_factor, 2, 1.0),
            vectorValueOr(base_factor, 3, 1.0),
        };
        base.metallic_factor =
            material.pbrMetallicRoughness
                .metallicFactor;
        base.roughness_factor =
            material.pbrMetallicRoughness
                .roughnessFactor;
        base.emissive_factor = {
            static_cast<double>(
                emissiveFactor(0) *
                emissive_strength),
            static_cast<double>(
                emissiveFactor(1) *
                emissive_strength),
            static_cast<double>(
                emissiveFactor(2) *
                emissive_strength),
        };
        auto definition =
            makeGltfCoreMaterialDefinition({
                .name = effective_name,
                .base = std::move(base),
                .alpha_mode =
                    material.alphaMode,
                .alpha_cutoff =
                    material.alphaCutoff,
                .double_sided =
                    material.doubleSided,
            });
        const auto &surface =
            gltfSurface(
                *definition.surface);
        const auto lowered =
            lowerMaterial(
                definition, surface,
                projectMaterialLoweringCapabilities());
        const auto fragment_shader =
            compile_surface_shaders
                ? std_mat.gltfFragmentShader(
                      surface,
                      *definition.surface,
                      lowered,
                      material_shader_defines)
                : std_mat.standardFragShader();

        MaterialInfo material_info{
            .vert_shader = std_mat.standardVertShader(),
            .frag_shader = fragment_shader,
            .base_color_texture = base_color_texture,
            .metallic_roughness_texture = metallic_roughness_texture,
            .normal_texture = normal_texture,
            .emissive_texture = emissive_texture,
            .occlusion_texture = occlusion_texture,
            .base_color_texture_identity =
                base_color_texture_index >= 0
                    ? "gltf-texture/" +
                          std::to_string(base_color_texture_index)
                    : std::string{},
            .alpha_mode = definition.routing->alpha_mode,
            .alpha_cutoff =
                static_cast<float>(material.alphaCutoff),
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
        applyLoweredMaterialForRoute(
            material_info, lowered);
        material_info.name =
            material.name.empty()
                ? "material[" +
                      std::to_string(material_index) + "]"
                : material.name;
        material_info.source_asset =
            authored_reference.value_or(std::string{});
        material_info.source_material_index =
            static_cast<std::uint32_t>(material_index);
        const auto uv_transform = baseColorUvTransform(
            material.pbrMetallicRoughness.baseColorTexture);
        // Only the typed project/path-resolver route carries authored
        // provenance. Inspection and legacy direct loads intentionally leave
        // this empty even when their physical path happens to look relative.
        if (authored_reference) {
            material_info.normalized_alpha_ir =
                normalizeGltfBaseColorAlpha({
                    .authored_reference = *authored_reference,
                    .material_index =
                        static_cast<std::uint32_t>(material_index),
                    .texture_index =
                        base_color_texture_index >= 0
                            ? std::optional<std::uint32_t>{
                                  static_cast<std::uint32_t>(
                                      base_color_texture_index)}
                            : std::nullopt,
                    .alpha_mode = definition.routing->alpha_mode,
                    .cutoff = static_cast<float>(material.alphaCutoff),
                    .base_factor_a = material_info.base_color_factor.w,
                    .uv_offset_scale = {
                        uv_transform.offset.x,
                        uv_transform.offset.y,
                        uv_transform.scale.x,
                        uv_transform.scale.y,
                    },
                    .uv_rotation = uv_transform.rotation,
                    .uv_transform_state = uv_transform.state,
                });
        }
        material_infos.at(material_index) =
            std::move(material_info);
        material_routings.at(material_index) =
            lowered.routing;
        material_map.at(material_index) =
            resources.registerMaterial(material_infos.at(material_index));
        resolved_materials[material_index] = material_map.at(material_index).value();
    }

    std::shared_ptr<const SourceMaterialInitialValueTable>
    sourceMaterialInitialValues() {
        auto table = std::make_shared<SourceMaterialInitialValueTable>();
        table->values.reserve(model.materials.size());
        for (std::size_t material_index = 0; material_index < model.materials.size();
             ++material_index) {
            const auto &material = model.materials[material_index];
            const auto &base = material.pbrMetallicRoughness.baseColorFactor;
            const auto &base_texture =
                material.pbrMetallicRoughness.baseColorTexture;
            const auto uv_transform =
                baseColorUvTransform(base_texture);
            float emissive_strength = 1.0f;
            if (const auto extension =
                    material.extensions.find("KHR_materials_emissive_strength");
                extension != material.extensions.end() && extension->second.IsObject()) {
                const auto &object = extension->second.Get<tinygltf::Value::Object>();
                if (const auto strength =
                        valueNumber(objectMember(object, "emissiveStrength"))) {
                    emissive_strength = static_cast<float>(*strength);
                }
            }
            table->values.push_back(SourceMaterialInitialValues{
                .source_material_index = static_cast<std::uint32_t>(material_index),
                .base_color_factor = glm::vec4{
                    static_cast<float>(vectorValueOr(base, 0, 1.0)),
                    static_cast<float>(vectorValueOr(base, 1, 1.0)),
                    static_cast<float>(vectorValueOr(base, 2, 1.0)),
                    static_cast<float>(vectorValueOr(base, 3, 1.0)),
                },
                .emissive_factor = glm::vec4{
                    static_cast<float>(vectorValueOr(material.emissiveFactor, 0, 0.0)) *
                        emissive_strength,
                    static_cast<float>(vectorValueOr(material.emissiveFactor, 1, 0.0)) *
                        emissive_strength,
                    static_cast<float>(vectorValueOr(material.emissiveFactor, 2, 0.0)) *
                        emissive_strength,
                    1.0f,
                },
                .uv_offset = uv_transform.offset,
                .uv_scale = uv_transform.scale,
                .uv_rotation = uv_transform.rotation,
            });
        }
        return table;
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
            append(material.occlusionTexture.index);
        }
        return textures;
    }

    GeneratedMaterialRegistry generatedMaterials() {
        return GeneratedMaterialRegistry{
            .resources = resources,
            .std_mat = std_mat,
            .model = model,
            .material_infos = material_infos,
            .next_generated_material = next_generated_material,
            .resolved_materials = resolved_materials,
            .generated_material_sources = generated_material_sources,
            .skinned_material_variants = skinned_material_variants,
        };
    }

    void loadMesh(int mesh_index, const glm::mat4 &world_transform,
                  int node_index = -1,
                  std::vector<std::string> node_name_path = {},
                  std::vector<std::uint32_t> parent_index_path = {}) {
        const auto &mesh = model.meshes.at(mesh_index);
        ModelCollisionMesh *collision = nullptr;
        if (node_index >= 0) {
            const auto &node = model.nodes.at(node_index);
            if (node.extras.IsObject()) {
                const auto *declaration = objectMember(node.extras.Get<tinygltf::Value::Object>(), "pelican.collision");
                if (declaration) {
                    if (!declaration->IsObject())
                        throw std::runtime_error("pelican.collision must be an object with shape mesh");
                    const auto &fields = declaration->Get<tinygltf::Value::Object>();
                    const auto *shape = objectMember(fields, "shape");
                    if (fields.size() != 1 || !shape || !shape->IsString() || shape->Get<std::string>() != "mesh")
                        throw std::runtime_error("pelican.collision supports only {shape: mesh}");
                    if (node.skin >= 0)
                        throw std::runtime_error("Static collision meshes cannot use skin deformation");
                    collision_meshes.push_back({static_cast<std::uint32_t>(node_index),
                        node_name_path, parent_index_path, {}, {}});
                    collision = &collision_meshes.back();
                }
            }
        }
        const auto morph_target_count = meshMorphTargetCount(mesh_index);
        const auto morph_weight_offset =
            appendMorphDefaults(mesh_index, node_index, morph_target_count);
        const auto skin_index =
            node_index >= 0 ? model.nodes.at(node_index).skin : -1;
        bool skinned = skin_index >= 0;
        if (skinned && !skinFitsPalette(skin_index)) {
            // Assets whose skins exceed the v1 palette must still load (pre-WP38
            // parity: skins were ignored entirely). Only explicit animation use
            // of such a skin is an error (selectSkin still throws there).
            LOG_WARNING(logger,
                        "gltf \"{}\": skin {} exceeds the v1 joint palette limit of {}; mesh {} loads without skinning",
                        source_path, skin_index, maxSkinJoints, mesh_index);
            skinned = false;
        }
        const auto joint_offset = skinned ? selectSkin(skin_index) : 0u;

        const PrimitiveSourceContext source{model,
                                            source_path,
                                            mesh,
                                            mesh_index,
                                            node_index,
                                            world_transform,
                                            node_name_path,
                                            parent_index_path,
                                            asset_key};
        const PrimitiveSkinContext skin{skinned, skin_index, joint_offset};
        const PrimitiveMorphContext morph{morph_target_count, morph_weight_offset};
        const PrimitiveVrmContext vrm{vrm_semantic, head_related_nodes};
        auto materials = generatedMaterials();

        for (std::size_t primitive_index = 0; primitive_index < mesh.primitives.size();
             ++primitive_index) {
            auto built = buildPrimitive(mesh.primitives[primitive_index],
                                        primitive_index, source, skin, morph, vrm,
                                        materials, collision);
            for (auto &[material_id, primitive_info] : built.material_primitives)
                tmp_material_primitives[material_id].emplace_back(
                    std::move(primitive_info));
            for (auto &layout : built.morph_layouts)
                morph_target_layout->primitives.push_back(std::move(layout));
        }
    }

    void loadNode(int node_index, const glm::mat4 &parent_transform,
                  bool subtree = true, bool apply_node_transform = true,
                  std::vector<std::string> name_path = {},
                  std::vector<std::uint32_t> parent_index_path = {}) {
        const auto &node = model.nodes.at(node_index);
        if (node.mesh < 0 && node.extras.IsObject() &&
            objectMember(node.extras.Get<tinygltf::Value::Object>(), "pelican.collision"))
            throw std::runtime_error("pelican.collision requires a mesh on the declaring node");
        if (name_path.empty()) name_path.push_back(node.name);
        const auto world_transform = apply_node_transform ? parent_transform * nodeTransform(node) : parent_transform;
        if (subtree) {
            for (const auto child_index : node.children) {
                auto child_name_path = name_path;
                child_name_path.push_back(model.nodes.at(child_index).name);
                auto child_index_path = parent_index_path;
                child_index_path.push_back(
                    static_cast<std::uint32_t>(node_index));
                loadNode(child_index, world_transform, true, true,
                         std::move(child_name_path),
                         std::move(child_index_path));
            }
        }
        if (node.mesh >= 0) {
            loadMesh(node.mesh, world_transform, node_index,
                     std::move(name_path),
                     std::move(parent_index_path));
        }
    }

    void finalizeOccurrenceIdentities(ModelTemplate &loaded) {
        using Info = ModelOccurrenceIdentityInfo;
        struct SourcePrimitiveInfo {
            std::uint32_t diagnostic_primitive_index = 0;
            std::shared_ptr<Info> info;
        };
        std::map<std::string, std::vector<SourcePrimitiveInfo>> by_node;
        std::map<std::vector<std::string>, std::set<std::string>> path_nodes;
        for (auto &material : loaded.material_primitives) {
            for (auto &primitive : material.primitives) {
                auto info = primitive.occurrence;
                if (!info) continue;
                std::string node_key = std::to_string(primitive.node_index);
                for (const auto index : info->diagnostic_parent_path)
                    node_key += ":" + std::to_string(index);
                by_node[node_key].push_back(
                    {primitive.primitive_index, info});
                if (!info->node_name_path.empty() &&
                    std::none_of(info->node_name_path.begin(),
                                 info->node_name_path.end(),
                                 [](const auto &part) { return part.empty(); })) {
                    path_nodes[info->node_name_path].insert(node_key);
                }
            }
        }
        for (auto &[_, records] : by_node) {
            std::map<std::uint32_t, OccurrenceDigest> source_primitives;
            for (const auto &record : records) {
                const auto [found, inserted] = source_primitives.emplace(
                    record.diagnostic_primitive_index,
                    record.info->primitive_content_digest);
                if (!inserted &&
                    found->second != record.info->primitive_content_digest)
                    throw std::logic_error(
                        "one source primitive produced inconsistent occurrence digests");
            }
            std::vector<OccurrenceDigest> primitive_digests;
            primitive_digests.reserve(source_primitives.size());
            for (const auto &[_, digest] : source_primitives)
                primitive_digests.push_back(digest);
            const auto node_digest = nodeContentDigestV1(primitive_digests);
            for (auto &record : records)
                record.info->node_content_digest = node_digest;
        }

        std::vector<std::shared_ptr<Info>> catalog;
        for (auto &material : loaded.material_primitives) {
            for (auto &primitive : material.primitives) {
                auto &info = *primitive.occurrence;
                const bool unique_name_path =
                    path_nodes.contains(info.node_name_path) &&
                    path_nodes.at(info.node_name_path).size() == 1;
                auto result = occurrenceIdV1(OccurrenceIdentityInput{
                    .asset = info.asset,
                    .node_explicit_id = info.node_explicit_id,
                    .node_name_path = info.node_name_path,
                    .node_name_path_unique = unique_name_path,
                    .node_content_digest = info.node_content_digest,
                    .primitive_explicit_id = info.primitive_explicit_id,
                    .primitive_content_digest = info.primitive_content_digest,
                    .variant = info.variant,
                });
                info.occurrence_id = std::move(result.occurrence_id);
                info.error = std::move(result.error);
                catalog.push_back(primitive.occurrence);
            }
        }
        detectOccurrenceIdCollisions(catalog);
    }

    ModelTemplate load() {
        material_map.resize(model.materials.size());
        material_infos.resize(model.materials.size());
        material_routings.resize(model.materials.size());
        texture_map.resize(model.textures.size());

        const auto selection = selectLoad();
        if (selection.animation_only) {
            if (!model.skins.empty()) {
                selectSkin(0, selection.animation_only);
            } else {
                skeletal_data = std::make_shared<SkeletalModelData>();
                skeletal_data->source_path = source_path;
                skeletal_data->nodes.reserve(model.nodes.size());
                for (const auto &node : model.nodes) skeletal_data->nodes.push_back(nodeRestPose(node));
                for (int parent = 0; parent < static_cast<int>(model.nodes.size()); ++parent) {
                    for (const auto child : model.nodes[parent].children) {
                        if (child >= 0 && child < static_cast<int>(skeletal_data->nodes.size()))
                            skeletal_data->nodes[child].parent = parent;
                    }
                }
                skeletal_data->clips.push_back(loadAnimationClip(*selection.animation_only));
            }
        }
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
                loadNode(root.node_index, root.parent_transform, root.subtree,
                         root.apply_node_transform, root.name_path,
                         root.parent_index_path);
            } else if (root.mesh_index >= 0) {
                loadMesh(root.mesh_index, glm::mat4{1.0f});
            }
        }

        ModelTemplate m;
        if (!collision_meshes.empty())
            m.collision_meshes = std::make_shared<const std::vector<ModelCollisionMesh>>(std::move(collision_meshes));
        for (const auto &[local_material_id, primitive] : tmp_material_primitives) {
            const auto found = resolved_materials.find(local_material_id);
            const auto generated_source =
                generated_material_sources.find(local_material_id);
            m.material_primitives.emplace_back(ModelTemplate::MaterialPrimitives{
                .material = found != resolved_materials.end() ? found->second : std_mat.standardTransparentMaterial(),
                .primitives = std::move(primitive),
                .source_material_index =
                    local_material_id >= 0
                        ? static_cast<std::uint32_t>(local_material_id)
                        : (generated_source != generated_material_sources.end()
                               ? generated_source->second
                               : std::optional<std::uint32_t>{}),
            });
        }
        for (std::size_t material_index = 0; material_index < material_map.size();
             ++material_index) {
            if (!material_map[material_index]) continue;
            m.named_materials.push_back(ModelTemplate::NamedMaterial{
                .name = model.materials[material_index].name,
                .material = *material_map[material_index],
                .routing =
                    material_routings[material_index],
            });
        }
        if (selection.material_only) {
            m.material_primitives.emplace_back(ModelTemplate::MaterialPrimitives{
                .material = resolved_materials.at(*selection.material_only),
                .primitives = {},
                .source_material_index =
                    static_cast<std::uint32_t>(*selection.material_only),
            });
        }
        m.skeletal = skeletal_data;
        if (!morph_target_layout->default_weights.empty()) {
            morph_target_layout->generation = allocateMorphLayoutGeneration();
            m.morph_targets = std::move(morph_target_layout);
        }
        m.material_initial_values = sourceMaterialInitialValues();
        m.vrm_semantic = vrm_semantic;
        m.gpu_resources = resources.finish();

        finalizeOccurrenceIdentities(m);

        return m;
    }
};

GltfLoader::GltfLoader() {}

PreparedGltf GltfLoader::prepareGltfBinary(std::string path,
                                           std::optional<AssetFragmentRef> fragment,
                                           std::optional<CanonicalModelAssetKey> asset_key) const {
    return PreparedGltf{prepareGltfImpl(std::move(path), std::move(fragment),
                                        true, std::move(asset_key))};
}

PreparedGltf GltfLoader::prepareGltf(std::string path,
                                     std::optional<AssetFragmentRef> fragment,
                                     std::optional<CanonicalModelAssetKey> asset_key) const {
    return PreparedGltf{prepareGltfImpl(std::move(path), std::move(fragment),
                                        false, std::move(asset_key))};
}

PreparedGltf GltfLoader::prepareEnginePrimitive(std::string_view reference,
                                              std::optional<CanonicalModelAssetKey> asset_key) const {
    return PreparedGltf{prepareEnginePrimitiveImpl(reference, std::move(asset_key))};
}

PreparedGltf GltfLoader::prepareGltfBinarySceneNode(std::string path,
                                                     AssetFragmentRef fragment,
                                                     std::optional<CanonicalModelAssetKey> asset_key) const {
    if (fragment.kind != "node") {
        throw std::runtime_error("scene node model reference requires #node fragment: " + path + "#" +
                                 fragment.kind + "/" + fragment.path);
    }
    auto prepared = prepareGltfBinary(std::move(path), std::move(fragment),
                                      std::move(asset_key));
    prepared.impl->scene_node_instance = true;
    return prepared;
}

ModelTemplate GltfLoader::commit(PreparedGltf prepared,
                                 std::optional<GltfAuthoredAssetReference>
                                     authored_reference) const {
    if (!prepared.impl) {
        throw std::runtime_error("cannot commit an empty prepared glTF");
    }
    if (!prepared.impl->warning.empty()) {
        LOG_WARNING(logger, "loading gltf file \"{}\" : {}", prepared.impl->source_path,
                    prepared.impl->warning);
    }
    if (!prepared.impl->error.empty()) {
        LOG_ERROR(logger, "loading gltf file \"{}\" : {}", prepared.impl->source_path,
                  prepared.impl->error);
    }
    if (logger != nullptr) {
        for (const auto &diagnostic : prepared.impl->vrm.diagnostics) {
            if (diagnostic.severity == VrmDiagnosticSeverity::info) {
                LOG_INFO(logger, "{}", diagnostic.message);
            } else {
                LOG_WARNING(logger, "{}", diagnostic.message);
            }
        }
    }
    // Complete a side-effect-free traversal first. Fragment ambiguity,
    // accessor/rig errors, and malformed vertex streams therefore fail before
    // a Vulkan object or mega-buffer range is touched.
    (void)inspect(prepared);
    LiveGltfResourceSink resources{GET_MODULE(MaterialContainer),
                                   GET_MODULE(VertBufContainer)};
    const auto authored_value =
        authored_reference
            ? std::optional<std::string>{authored_reference->value}
            : std::nullopt;
    InternalGltfLoader loader{
        resources,
        GET_MODULE(StandardMaterialResource),
        prepared.impl->model,
        prepared.impl->source_path,
        authored_value,
        prepared.impl->asset_key,
        prepared.impl->fragment,
        prepared.impl->scene_node_instance,
        prepared.impl->vrm.semantic,
        true,
        activeMaterialShaderDefines(),
    };
    try {
        auto model = loader.load();
        model.authored_asset_reference =
            authored_value.value_or(std::string{});
        return model;
    } catch (const std::exception &error) {
        auto report = resources.release();
        if (!report.empty()) {
            throw ModelResourceReleaseException{error.what(), report};
        }
        throw;
    } catch (...) {
        auto report = resources.release();
        if (!report.empty()) {
            throw ModelResourceReleaseException{
                "non-standard glTF commit failure", report};
        }
        throw;
    }
}

void GltfLoader::validateMeshNode(const PreparedGltf &prepared, std::uint32_t node, std::uint32_t mesh) const {
    if (!prepared.impl || node >= prepared.impl->model.nodes.size() ||
        prepared.impl->model.nodes[node].mesh != static_cast<int>(mesh))
        throw std::invalid_argument("deformed attribute node does not reference its source mesh");
}

GltfAttributeData GltfLoader::readAttribute(const PreparedGltf &prepared,
    std::uint32_t mesh, std::uint32_t primitive, std::string_view semantic,
    ShaderResourceBufferElement element, std::optional<std::uint32_t> morph_target) const {
    if (!prepared.impl) throw std::invalid_argument("cannot read an attribute from an empty prepared glTF");
    const auto &source = prepared.impl->model.meshes.at(mesh).primitives.at(primitive);
    const auto &attributes = morph_target ? source.targets.at(*morph_target) : source.attributes;
    const auto accessor = attributes.find(std::string{semantic});
    if (accessor == attributes.end())
        throw std::invalid_argument("glTF primitive has no attribute: " + std::string{semantic});
    auto result = readAccessor(prepared, accessor->second, element);
    if (morph_target) {
        if (semantic != "POSITION" && semantic != "NORMAL" && semantic != "TANGENT")
            throw std::invalid_argument("unsupported glTF morph target semantic");
        if (element != ShaderResourceBufferElement::vec3)
            throw std::invalid_argument("glTF morph target attributes require vec3 elements");
        const auto base = source.attributes.find(std::string{semantic});
        if (base == source.attributes.end() ||
            result.count != prepared.impl->model.accessors.at(base->second).count)
            throw std::invalid_argument("glTF morph target count must match its base attribute");
    }
    return result;
}

GltfAttributeData GltfLoader::readIndices(const PreparedGltf &prepared,
    std::uint32_t mesh, std::uint32_t primitive) const {
    if (!prepared.impl) throw std::invalid_argument("cannot read indices from an empty prepared glTF");
    const auto &source = prepared.impl->model.meshes.at(mesh).primitives.at(primitive);
    if (source.indices < 0) throw std::invalid_argument("glTF primitive is not indexed");
    auto result = readAccessor(prepared, source.indices, ShaderResourceBufferElement::unsigned_integer);
    const auto position = source.attributes.find("POSITION");
    if (position == source.attributes.end()) throw std::invalid_argument("indexed glTF primitive has no POSITION attribute");
    const auto count = prepared.impl->model.accessors.at(position->second).count;
    for (std::size_t i = 0; i < result.count; ++i) {
        std::uint32_t index;
        std::memcpy(&index, result.bytes.data() + i * sizeof(index), sizeof(index));
        if (index >= count) throw std::invalid_argument("glTF index exceeds vertex count");
    }
    return result;
}

GltfAttributeData GltfLoader::readAccessor(const PreparedGltf &prepared, int accessor,
    ShaderResourceBufferElement element) const {
    const auto component = prepared.impl->model.accessors.at(accessor).componentType;
    const bool signed_output = element == ShaderResourceBufferElement::integer ||
        element == ShaderResourceBufferElement::ivec2 || element == ShaderResourceBufferElement::ivec3 ||
        element == ShaderResourceBufferElement::ivec4;
    const bool unsigned_output = element == ShaderResourceBufferElement::unsigned_integer ||
        element == ShaderResourceBufferElement::uvec2 || element == ShaderResourceBufferElement::uvec3 ||
        element == ShaderResourceBufferElement::uvec4;
    if ((signed_output && component != TINYGLTF_COMPONENT_TYPE_BYTE &&
            component != TINYGLTF_COMPONENT_TYPE_SHORT && component != TINYGLTF_COMPONENT_TYPE_INT) ||
        (unsigned_output && component != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
            component != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT && component != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
        throw std::invalid_argument("integer graph attributes require integer source components with matching signedness");
    GltfAccessorReader loader{prepared.impl->model, prepared.impl->source_path};
    const auto pack = [&]<int type, class T>() {
        const auto values = loader.getDataFromAccessor<type, T>(accessor);
        const auto stride = shaderResourceBufferElementStride(element);
        if (values.size() > (std::numeric_limits<std::size_t>::max)() / stride)
            throw std::overflow_error("glTF attribute byte size overflows");
        GltfAttributeData result{element, values.size(), std::vector<std::byte>(values.size() * stride)};
        if (!prepared.impl->generated)
            result.source_files.push_back(std::filesystem::weakly_canonical(prepared.impl->source_path));
        const auto view = prepared.impl->model.accessors.at(accessor).bufferView;
        const auto buffer = prepared.impl->model.bufferViews.at(view).buffer;
        if (const auto found = prepared.impl->buffer_source_files.find(buffer);
            found != prepared.impl->buffer_source_files.end()) result.source_files.push_back(found->second);
        for (std::size_t i = 0; i < values.size(); ++i)
            std::memcpy(result.bytes.data() + i * stride, &values[i], sizeof(T));
        return result;
    };
    switch (element) {
    case ShaderResourceBufferElement::floating: return pack.template operator()<TINYGLTF_TYPE_SCALAR, float>();
    case ShaderResourceBufferElement::vec2: return pack.template operator()<TINYGLTF_TYPE_VEC2, glm::vec2>();
    case ShaderResourceBufferElement::vec3: return pack.template operator()<TINYGLTF_TYPE_VEC3, glm::vec3>();
    case ShaderResourceBufferElement::vec4: return pack.template operator()<TINYGLTF_TYPE_VEC4, glm::vec4>();
    case ShaderResourceBufferElement::integer: return pack.template operator()<TINYGLTF_TYPE_SCALAR, std::int32_t>();
    case ShaderResourceBufferElement::ivec2: return pack.template operator()<TINYGLTF_TYPE_VEC2, glm::ivec2>();
    case ShaderResourceBufferElement::ivec3: return pack.template operator()<TINYGLTF_TYPE_VEC3, glm::ivec3>();
    case ShaderResourceBufferElement::ivec4: return pack.template operator()<TINYGLTF_TYPE_VEC4, glm::ivec4>();
    case ShaderResourceBufferElement::unsigned_integer: return pack.template operator()<TINYGLTF_TYPE_SCALAR, std::uint32_t>();
    case ShaderResourceBufferElement::uvec2: return pack.template operator()<TINYGLTF_TYPE_VEC2, glm::uvec2>();
    case ShaderResourceBufferElement::uvec3: return pack.template operator()<TINYGLTF_TYPE_VEC3, glm::uvec3>();
    case ShaderResourceBufferElement::uvec4: return pack.template operator()<TINYGLTF_TYPE_VEC4, glm::uvec4>();
    case ShaderResourceBufferElement::mat4: return pack.template operator()<TINYGLTF_TYPE_MAT4, glm::mat4>();
    }
    throw std::invalid_argument("unknown glTF attribute element type");
}

ModelTemplate GltfLoader::inspect(const PreparedGltf &prepared) const {
    if (!prepared.impl) throw std::runtime_error("cannot inspect an empty prepared glTF");
    ValidationGltfResourceSink resources;
    InternalGltfLoader loader{
        resources,
        GET_MODULE(StandardMaterialResource),
        prepared.impl->model,
        prepared.impl->source_path,
        std::nullopt,
        prepared.impl->asset_key,
        prepared.impl->fragment,
        prepared.impl->scene_node_instance,
        prepared.impl->vrm.semantic,
        false,
        {},
    };
    return loader.load();
}

ModelTemplate GltfLoader::loadGltfBinary(std::string path,
                                         std::optional<AssetFragmentRef> fragment) {
    return commit(
        prepareGltfBinary(std::move(path), std::move(fragment)));
}

ModelTemplate GltfLoader::loadGltfBinarySceneNode(std::string path, AssetFragmentRef fragment) {
    auto prepared = prepareGltfBinarySceneNode(std::move(path), std::move(fragment));
    return commit(std::move(prepared));
}

ModelTemplate GltfLoader::loadGltf(std::string path,
                                   std::optional<AssetFragmentRef> fragment) {
    return commit(prepareGltf(std::move(path), std::move(fragment)));
}

ReleaseReport releaseModelGpuResources(ModelTemplate &model,
                                       bool deferred) noexcept {
    ReleaseReport report;
    auto resources = std::move(model.gpu_resources);
    model.gpu_resources.reset();
    if (!resources || std::exchange(resources->released, true)) return report;
    if (auto *materials = FastModuleContainer::tryGet<MaterialContainer>()) {
        report.append(materials->releaseModelResources(
            std::move(resources->materials), std::move(resources->textures),
            deferred));
    } else {
        for (const auto material : resources->materials) {
            report.add({ReleaseStage::material,
                        ReleaseErrorCode::owner_unavailable,
                        static_cast<std::uint32_t>(material.value)});
        }
        for (const auto texture : resources->textures) {
            report.add({ReleaseStage::texture,
                        ReleaseErrorCode::owner_unavailable,
                        static_cast<std::uint32_t>(texture.value)});
        }
    }
    if (auto *geometry = FastModuleContainer::tryGet<VertBufContainer>()) {
        report.append(geometry->releaseModelGeometry(
            std::move(resources->geometry), deferred));
    } else {
        for (std::uint32_t index = 0; index < resources->geometry.size();
             ++index) {
            report.add({ReleaseStage::vertbuf_index,
                        ReleaseErrorCode::owner_unavailable, index});
        }
    }
    return report;
}

} // namespace Pelican
