#include "proceduralassetpublication.hpp"

#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../loader/resolvedscene.hpp"
#include "../renderer/proceduralgeometrycontainer.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../../project/assetdataformat.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace Pelican {
struct PreparedProceduralAssetPublication::State {
    ProjectBasicConfig &config;
    ProceduralGeometryContainer &procedural;
    PolygonInstanceContainer &renderer;
    std::string cache;
    ProceduralGeometryContainer::PreparedSceneState procedural_state;
    PolygonInstanceContainer::PreparedSceneState renderer_state;
    SceneLoadPhaseTrace trace;
};

PreparedProceduralAssetPublication::PreparedProceduralAssetPublication(std::unique_ptr<State> state)
    : state_(std::move(state)) {}
PreparedProceduralAssetPublication::~PreparedProceduralAssetPublication() = default;

std::unique_ptr<PreparedProceduralAssetPublication> PreparedProceduralAssetPublication::prepare(
    ProjectBasicConfig &config, PathResolver &resolver,
    ProceduralGeometryContainer &procedural, PolygonInstanceContainer &renderer,
    VulkanManageCore *vkcore, std::string_view scene_id, std::string_view raw_assets) {
    const auto *scene = config.resolvedScene().findScene(scene_id);
    if (!scene) throw std::invalid_argument("Procedural asset publication requires the active resolved scene.");
    if (procedural.allocationGeneration() == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Procedural allocation generation exhausted.");
    auto state = std::make_unique<State>(State{config, procedural, renderer});
    state->cache = config.prepareAssetDataJson(raw_assets, resolver);
    const auto assets = parseAssetDataFormatJson(nlohmann::json::parse(state->cache));
    state->procedural_state = procedural.prepareScene(assets, *scene, vkcore,
        procedural.allocationGeneration() + 1, state->trace);
    state->renderer_state = renderer.snapshotPreparedScene();
    const auto instances = procedural.proceduralRayInstances(&state->procedural_state);
    renderer.prepareRayScenePlan(state->renderer_state,
        state->renderer_state.ray_scene_plan ? state->renderer_state.ray_scene_plan->project : nullptr,
        nullptr, instances);
    return std::unique_ptr<PreparedProceduralAssetPublication>(
        new PreparedProceduralAssetPublication(std::move(state)));
}

void PreparedProceduralAssetPublication::commit(const std::function<void()> &source_commit) {
    if (!state_) throw std::logic_error("Procedural asset publication was already committed.");
    source_commit();
    state_->config.publishAssetDataJson(std::move(state_->cache));
    state_->procedural.publishPrepared(state_->procedural_state, state_->trace);
    state_->renderer.publishPreparedScene(std::move(state_->renderer_state));
    state_->procedural.enqueueRetirement(std::move(state_->procedural_state), state_->trace);
    state_.reset();
}
} // namespace Pelican
