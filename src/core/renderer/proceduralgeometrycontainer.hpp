#pragma once

#include "../container.hpp"
#include "../vkcore/accelerationstructure.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace Pelican {

class VulkanManageCore;
struct AssetDataFormatDocument;
struct ResolvedSceneView;
struct SceneLoadPhaseTrace;

DECLARE_MODULE(ProceduralGeometryContainer) {
    struct State;
    std::shared_ptr<State> active_;
    std::vector<std::shared_ptr<State>> retire_queue_;

  public:
    struct PreparedSceneState {
        std::shared_ptr<State> state;
        bool published = false;
    };

    ProceduralGeometryContainer();
    ~ProceduralGeometryContainer();

    PreparedSceneState prepareScene(
        const AssetDataFormatDocument &assets,
        const ResolvedSceneView &scene,
        VulkanManageCore *vkcore,
        std::uint64_t allocation_generation,
        SceneLoadPhaseTrace &phase_trace);
    void publishPrepared(PreparedSceneState &prepared,
                         SceneLoadPhaseTrace &phase_trace) noexcept;
    void dropPreparedAabbBuffersForMutation(
        PreparedSceneState &prepared) noexcept;
    void dropPreparedAllocationGenerationForMutation(
        PreparedSceneState &prepared) noexcept;
    void dropPreparedInstancesForMutation(
        PreparedSceneState &prepared) noexcept;
    void rollbackPrepared(PreparedSceneState &prepared) noexcept;
    void enqueueRetirement(PreparedSceneState &&prepared,
                           SceneLoadPhaseTrace &phase_trace) noexcept;
    void drainRetirements() noexcept;
    void clear() noexcept;

    std::vector<RayQueryGeometryInstanceSnapshot>
    proceduralRayInstances(const PreparedSceneState *prepared = nullptr) const;
    std::vector<RayQueryGeometryInstanceSnapshot> preparedRayInstances(
        const ResolvedSceneView &scene,
        const PreparedSceneState *prepared = nullptr) const;
    std::uint64_t allocationGeneration() const noexcept;
    std::size_t instanceCount() const noexcept;
    std::size_t retirementCount() const noexcept;
};

} // namespace Pelican
