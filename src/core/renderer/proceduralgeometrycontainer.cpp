#include "proceduralgeometrycontainer.hpp"
#include "../loader/editorprojectiontransaction.hpp"

#include "proceduralgeometrycomponent.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../loader/scene.hpp"
#include "../loader/sceneload.hpp"
#include "../loader/resolvedscene.hpp"
#include "../vkcore/buf.hpp"
#include "../vkcore/core.hpp"
#include "../../project/assetdataformat.hpp"
#include <components/predefined.hpp>

#include <algorithm>
#include <bit>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vk_mem_alloc.hpp>

namespace Pelican {
namespace {

std::uint64_t fnv1a(std::string_view text, std::uint64_t seed =
                    14695981039346656037ull) noexcept {
    auto result = seed;
    for (const auto byte : text) {
        result ^= static_cast<unsigned char>(byte);
        result *= 1099511628211ull;
    }
    return result;
}

std::uint64_t appendF32(std::uint64_t hash, float value) noexcept {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (bits >> shift) & 0xffu;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string componentOccurrence(const ResolvedComponent &component) {
    if (const auto *generated = component.origin.generated()) {
        return generated->stable_generated_id;
    }
    return std::to_string(component.authoring_component_index);
}

glm::mat4 worldMatrix(const TransformComponent *transform) {
    if (transform == nullptr) return glm::mat4{1.0F};
    return glm::translate(glm::mat4{1.0F}, transform->pos) *
           glm::mat4_cast(transform->rotation) *
           glm::scale(glm::mat4{1.0F}, transform->scale);
}

} // namespace

struct ProceduralGeometryContainer::State {
    struct Asset {
        AssetDataProceduralGeometryDeclaration declaration;
        std::shared_ptr<BufferWrapper> aabb_buffer;
        std::uint64_t buffer_address = 0;
        std::uint64_t allocation_id = 0;
        std::uint64_t asset_identity = 0;
    };
    struct Instance {
        std::uint64_t authoring_object_id = 0;
        std::string occurrence;
        std::string asset;
    };

    std::uint64_t allocation_generation = 0;
    std::map<std::string, Asset, std::less<>> assets;
    std::vector<Instance> instances;
};

ProceduralGeometryContainer::ProceduralGeometryContainer()
    : active_{std::make_shared<State>()} {}
ProceduralGeometryContainer::~ProceduralGeometryContainer() = default;

ProceduralGeometryContainer::PreparedSceneState
ProceduralGeometryContainer::prepareScene(
    const AssetDataFormatDocument &assets, const ResolvedSceneView &scene,
    VulkanManageCore *vkcore, std::uint64_t allocation_generation,
    SceneLoadPhaseTrace &phase_trace) {
    phase_trace.record(SceneLoadPhase::prepare);
    auto candidate = std::make_shared<State>();
    candidate->allocation_generation = allocation_generation;
    retire_queue_.reserve(retire_queue_.size() + 1);

    std::map<std::string, const AssetDataProceduralGeometryDeclaration *,
             std::less<>> declarations;
    for (const auto &asset : assets.procedural_geometry) {
        declarations.emplace(asset.name, &asset);
    }
    for (const auto &object : scene.objects) {
        for (const auto &component : object.components) {
            if (component.name != "pelican_procedural_geometry") continue;
            const auto &value = std::any_cast<const ProceduralGeometryComponent &>(
                component.requireRuntimeValue());
            const auto found = declarations.find(value.asset);
            if (found == declarations.end()) {
                throw std::runtime_error(
                    "pelican.procedural_asset_unresolved@1: asset='" +
                    value.asset + "'");
            }
            candidate->instances.push_back(State::Instance{
                .authoring_object_id = object.authoring_object_id.value,
                .occurrence = componentOccurrence(component),
                .asset = value.asset,
            });
        }
    }

    for (const auto &instance : candidate->instances) {
        if (vkcore == nullptr) {
            throw std::logic_error(
                "procedural scene preparation requires Vulkan");
        }
        if (candidate->assets.contains(instance.asset)) continue;
        const auto &declaration = *declarations.at(instance.asset);
        if (const auto existing = active_->assets.find(instance.asset);
            existing != active_->assets.end() && existing->second.declaration == declaration) {
            candidate->assets.emplace(instance.asset, existing->second);
            continue;
        }
        vk::AabbPositionsKHR aabb{
            declaration.aabb_min[0], declaration.aabb_min[1],
            declaration.aabb_min[2], declaration.aabb_max[0],
            declaration.aabb_max[1], declaration.aabb_max[2]};
        auto buffer = std::make_shared<BufferWrapper>(vkcore->allocBuf(
            sizeof(aabb),
            vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR |
                vk::BufferUsageFlagBits::eShaderDeviceAddress,
            vma::MemoryUsage::eAutoPreferDevice,
            vma::AllocationCreateFlagBits::eHostAccessSequentialWrite));
        vkcore->writeBuf(*buffer, &aabb, 0, sizeof(aabb));
        const auto address = vkcore->getDevice().getBufferAddress(
            vk::BufferDeviceAddressInfo{buffer->buffer.get()});
        // VkAccelerationStructureGeometryAabbsDataKHR::data requires
        // eight-byte device-address alignment (VkDeviceOrHostAddressConstKHR).
        if (address == 0 || address % 8u != 0) {
            throw std::runtime_error(
                "pelican.procedural_aabb_buffer_invalid@1: AABB device address is zero or misaligned");
        }
        auto allocation_id = fnv1a(declaration.name);
        for (const auto value : declaration.aabb_min)
            allocation_id = appendF32(allocation_id, value);
        for (const auto value : declaration.aabb_max)
            allocation_id = appendF32(allocation_id, value);
        if (allocation_id == 0) allocation_id = 1;
        auto asset_identity = fnv1a(declaration.name);
        if (asset_identity == 0) asset_identity = 1;
        candidate->assets.emplace(
            declaration.name,
            State::Asset{declaration, std::move(buffer), address,
                         allocation_id, asset_identity});
    }
    std::sort(candidate->instances.begin(), candidate->instances.end(),
              [](const auto &left, const auto &right) {
                  return std::tie(left.authoring_object_id, left.occurrence,
                                  left.asset) <
                         std::tie(right.authoring_object_id, right.occurrence,
                                  right.asset);
              });
    return PreparedSceneState{.state = std::move(candidate)};
}

void ProceduralGeometryContainer::publishPrepared(
    PreparedSceneState &prepared,
    SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::publish);
    active_.swap(prepared.state);
    prepared.published = !prepared.published;
}

void ProceduralGeometryContainer::dropPreparedAabbBuffersForMutation(
    PreparedSceneState &prepared) noexcept {
    if (!prepared.state) return;
    for (auto &[_, asset] : prepared.state->assets) {
        asset.aabb_buffer.reset();
        asset.buffer_address = 0;
    }
}

void ProceduralGeometryContainer::dropPreparedAllocationGenerationForMutation(
    PreparedSceneState &prepared) noexcept {
    if (prepared.state) prepared.state->allocation_generation = 0;
}

void ProceduralGeometryContainer::dropPreparedInstancesForMutation(
    PreparedSceneState &prepared) noexcept {
    if (prepared.state) prepared.state->instances.clear();
}

void ProceduralGeometryContainer::rollbackPrepared(
    PreparedSceneState &prepared) noexcept {
    if (!prepared.published) return;
    active_.swap(prepared.state);
    prepared.published = false;
}

void ProceduralGeometryContainer::enqueueRetirement(
    PreparedSceneState &&prepared,
    SceneLoadPhaseTrace &phase_trace) noexcept {
    phase_trace.record(SceneLoadPhase::retire);
    if (prepared.state) retire_queue_.push_back(std::move(prepared.state));
}

void ProceduralGeometryContainer::drainRetirements() noexcept {
    retire_queue_.clear();
}

void ProceduralGeometryContainer::clear() noexcept {
    active_ = std::make_shared<State>();
    retire_queue_.clear();
}

std::vector<RayQueryGeometryInstanceSnapshot>
ProceduralGeometryContainer::proceduralRayInstances() const {
    std::vector<RayQueryGeometryInstanceSnapshot> result;
    if (!active_ || active_->instances.empty()) return result;
    const auto &bindings = GET_MODULE(SceneLoader).runtimeObjectBindings();
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    result.reserve(active_->instances.size());
    for (const auto &instance : active_->instances) {
        const auto binding = std::find_if(
            bindings.begin(), bindings.end(), [&](const auto &candidate) {
                return candidate.authoring_object_id.value ==
                       instance.authoring_object_id;
            });
        if (binding == bindings.end()) {
            throw std::runtime_error(
                "pelican.procedural_instance_unbound@1: authoring object has no runtime entity");
        }
        const auto asset = active_->assets.find(instance.asset);
        if (asset == active_->assets.end()) {
            throw std::logic_error("published procedural asset is missing");
        }
        const auto *transform =
            ecs.tryComponent<TransformComponent>(binding->object_id);
        const auto *ray_instance =
            ecs.tryComponent<RayInstanceComponent>(binding->object_id);
        result.push_back(RayQueryGeometryInstanceSnapshot{
            .geometry_kind = RayGeometryKind::aabbs,
            .asset_identity = asset->second.asset_identity,
            .authoring_object_id = instance.authoring_object_id,
            .component_occurrence = instance.occurrence,
            .ray_instance = ray_instance != nullptr
                                ? std::optional<RayInstanceComponent>{
                                      *ray_instance}
                                : std::nullopt,
            .scene_epoch = active_->allocation_generation,
            .geometry_allocation_id = asset->second.allocation_id,
            .procedural_asset = asset->second.declaration.name,
            .procedural_record = asset->second.declaration.record,
            .aabb_buffer_address = asset->second.buffer_address,
            .geometry_lease = asset->second.aabb_buffer,
            .world_transform = worldMatrix(transform),
        });
    }
    return result;
}

std::vector<RayQueryGeometryInstanceSnapshot>
ProceduralGeometryContainer::preparedRayInstances(
    const ResolvedSceneView &scene, const PreparedSceneState *prepared) const {
    const auto &state = prepared ? prepared->state : active_;
    std::vector<RayQueryGeometryInstanceSnapshot> result;
    if (!state) return result;
    const auto transforms = resolvedSceneWorldTransforms(scene);
    for (const auto &instance : state->instances) {
        const auto object = std::find_if(scene.objects.begin(), scene.objects.end(),
            [&](const auto &candidate) {
                return candidate.authoring_object_id.value == instance.authoring_object_id;
            });
        if (object == scene.objects.end()) continue;
        const auto declaration = std::find_if(object->components.begin(), object->components.end(),
            [](const auto &component) { return component.name == "pelican_ray_instance"; });
        const auto &asset = state->assets.at(instance.asset);
        const auto transform = transforms.find(instance.authoring_object_id);
        result.push_back(RayQueryGeometryInstanceSnapshot{
            .geometry_kind = RayGeometryKind::aabbs,
            .asset_identity = asset.asset_identity,
            .authoring_object_id = instance.authoring_object_id,
            .component_occurrence = instance.occurrence,
            .ray_instance = declaration == object->components.end() ? std::nullopt
                : std::optional<RayInstanceComponent>{std::any_cast<const RayInstanceComponent &>(
                    declaration->requireRuntimeValue())},
            .scene_epoch = state->allocation_generation,
            .geometry_allocation_id = asset.allocation_id,
            .procedural_asset = asset.declaration.name,
            .procedural_record = asset.declaration.record,
            .aabb_buffer_address = asset.buffer_address,
            .geometry_lease = asset.aabb_buffer,
            .world_transform = worldMatrix(transform == transforms.end() ? nullptr : &transform->second),
        });
    }
    return result;
}

std::uint64_t
ProceduralGeometryContainer::allocationGeneration() const noexcept {
    return active_ ? active_->allocation_generation : 0;
}

std::size_t ProceduralGeometryContainer::instanceCount() const noexcept {
    return active_ ? active_->instances.size() : 0;
}

std::size_t ProceduralGeometryContainer::retirementCount() const noexcept {
    return retire_queue_.size();
}

} // namespace Pelican
